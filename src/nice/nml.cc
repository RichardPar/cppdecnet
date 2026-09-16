// src/nice/nml.cc -- network management listener, object 19.
//
// Port of modules/nml.py.  Decodes NICE requests, collects answers through
// Node::nice_read and sends replies.
//
// A request for one entity gets a single "success" reply.  A request that
// produces several replies gets "multiple items", the replies, then an end
// marker.  Replies for the same entity are grouped with "more for this
// entity" on all but the last.
//
// PORT: only READ INFORMATION and LOOP NODE.  See nml.h.

#include "decnet/nice/nml.h"

#include "decnet/common/logging.h"
#include "decnet/node.h"
#include "decnet/session/packets.h"
#include "decnet/session/session.h"

namespace decnet::nice {

namespace {

// The mirror object, which LOOP NODE talks to.
constexpr std::uint8_t mirror_object = 25;

// Map a session control reject reason to the detail code a loop error
// reply carries.  Port of nml.ses2mirror.
unsigned mirror_detail (unsigned reason)
{
    switch (reason) {
    case session::NO_OBJ:   return 7;
    case session::BAD_FMT:  return 6;
    case session::BAD_AUTH: return 8;
    case session::BAD_ACCT: return 8;
    case session::OBJ_FAIL: return 12;
    case session::UNREACH:  return 3;
    default:                return 5;   // rejected by the application
    }
}

class NmlApplication;

// Application for the loop connection to the remote MIRROR.  Forwards
// events to the listener.
class LoopApplication : public session::Application {
public:
    explicit LoopApplication (NmlApplication *nml) noexcept : nml_ (nml) {}

    // The listener is going away; stop calling into it.
    void orphan () noexcept { nml_ = nullptr; }

    void connect_received (session::SessionConnection &c, ByteView data) override;
    void data_received (session::SessionConnection &c, ByteView data) override;
    void disconnected (session::SessionConnection &c, unsigned reason) override;

private:
    NmlApplication *nml_;
};

class NmlApplication : public session::Application {
public:
    explicit NmlApplication (Node *node) noexcept : node_ (node) {}

    ~NmlApplication () override
    {
        // Detach from the listener in case the connection outlives it.
        if (loop_app_) loop_app_->orphan ();
    }

    // ------------------------------------------------- the NCP connection

    void connect_received (session::SessionConnection &c, ByteView data) override
    {
        // Connect data is the NICE version.  No version means Phase II NCP, which
        // is not supported; requests will be refused.
        phase2_ = data.empty ();
        if (phase2_) {
            DN_TRACE ("NICE connection from {} is Phase II, unsupported",
                      c.remote ().str ());
            c.accept ();
        } else {
            DN_TRACE ("NICE connection from {}, version {}.{}.{}",
                      c.remote ().str (), data[0],
                      data.size () > 1 ? data[1] : 0,
                      data.size () > 2 ? data[2] : 0);
            c.accept (Bytes (std::begin (nice_version),
                             std::end (nice_version)));
        }
        conn_ = &c;
    }

    void data_received (session::SessionConnection &c, ByteView data) override
    {
        conn_ = &c;
        if (phase2_) { send (NiceReply::error (rc_unrecognized_function)); return; }

        NiceRequest req;
        try {
            req = NiceRequest::parse (data);
        } catch (const DecodeError &e) {
            DN_TRACE ("invalid NICE request: {}", e.what ());
            send (NiceReply::error (rc_unrecognized_function));
            return;
        }
        DN_TRACE ("NICE request: {}", req.str ());

        switch (req.function) {
        case fn_read: read (req); return;
        case fn_test: loop (req); return;
        case fn_set:
            // Not implemented, same as PyDECnet: "unrecognized function" (-1).
            send (NiceReply::error (rc_unrecognized_function));
            return;
        case fn_zero:
            // Writes are refused with "privilege violation", as PyDECnet does when
            // read-only.  Credentials are not authenticated.  See NOTDONE.md.
            send (NiceReply::error (rc_privilege_violation));
            return;
        default:
            send (NiceReply::error (rc_unrecognized_function));
            return;
        }
    }

    void disconnected (session::SessionConnection &c, unsigned) override
    {
        if (&c != conn_) return;
        DN_TRACE ("NICE connection from {} closed", c.remote ().str ());
        conn_ = nullptr;
        abandon_loop ();
    }

    // ------------------------------------------ the loop connection's end

    void loop_accepted (ByteView data)
    {
        // The accept data is the largest message the mirror will take.
        unsigned maxlen = 0;
        if (data.size () >= 2)
            maxlen = static_cast<unsigned> (data[0])
                   | (static_cast<unsigned> (data[1]) << 8);
        if (maxlen && maxlen < loop_data_.size () + 1) {
            // Our messages will not fit, which is a bad loop length rather
            // than a failure of the far end.
            abandon_loop ();
            send_loop_error (rc_invalid_parameter, 151);
            return;
        }
        send_loop_message ();
    }

    void loop_data (ByteView data)
    {
        // The mirror answers with a status byte then the data it echoed.
        bool good = !data.empty () && data[0] == 1
                 && data.size () == loop_data_.size () + 1
                 && std::equal (loop_data_.begin (), loop_data_.end (),
                                data.begin () + 1);
        if (!good) {
            abandon_loop ();
            send_loop_error (rc_bad_loopback, 0xffff);
            return;
        }
        if (--loop_count_ < 1) {
            close_loop ();
            NiceReply r;
            r.retcode = rc_success;
            send (r);
            return;
        }
        send_loop_message ();
    }

    void loop_down (unsigned reason)
    {
        // The link went away before the loop finished.  A reject arrives
        // here too, which is why the reason is mapped rather than assumed.
        if (!loop_conn_) return;
        loop_conn_ = nullptr;
        if (loop_app_) { loop_app_->orphan (); loop_app_ = nullptr; }
        send_loop_error (rc_mirror_connect_failed, mirror_detail (reason));
    }

private:
    // ------------------------------------------------------------- replies

    void send (const NiceReply &r)
    {
        if (!conn_ || !conn_->running ()) return;
        conn_->send_data (r.encode ());
    }

    void send_loop_error (int retcode, unsigned detail)
    {
        NiceReply r;
        r.retcode = retcode;
        r.detail = static_cast<std::uint16_t> (detail);
        r.has_notlooped = true;
        r.notlooped = static_cast<std::uint16_t> (loop_count_ < 0
                                                  ? 0 : loop_count_);
        send (r);
    }

    // ---------------------------------------------------------- read info

    void read (const NiceRequest &creq)
    {
        NiceRequest req = creq;
        if (req.permanent) {
            // No permanent database; configuration is not writable via NICE.
            send (NiceReply::error (rc_unrecognized_function));
            return;
        }

        ReplyDict replies (req.entity_type, node_);
        int err = node_ ? node_->nice_read (req, replies) : rc_operation_failure;
        if (err) { send (NiceReply::error (err)); return; }

        auto groups = replies.sorted (req);
        if (groups.empty ()) {
            // No replies: unknown entity for a singular request, empty result for a
            // plural one.
            if (req.entity.mult ()) {
                send (NiceReply (rc_multiple));
                send (NiceReply (rc_done));
            } else {
                send (NiceReply::error (rc_unrecognized_component));
            }
            return;
        }

        // One entity with one reply is the simple case: answer it directly
        // rather than wrapping it in a multiple-item exchange.
        if (groups.size () == 1 && groups[0].size () == 1) {
            NiceReply &r = *groups[0][0];
            r.retcode = rc_success;
            send (r);
            return;
        }

        send (NiceReply (rc_multiple));
        for (auto &group : groups) {
            for (std::size_t i = 0; i + 1 < group.size (); ++i) {
                group[i]->retcode = rc_more;
                send (*group[i]);
            }
            group.back ()->retcode = rc_success;
            send (*group.back ());
        }
        send (NiceReply (rc_done));
    }

    // --------------------------------------------------------- loop node

    void loop (const NiceRequest &req)
    {
        if (req.test_type != test_node) {
            // LOOP CIRCUIT and LOOP LINE drive MOP loopback, which is
            // there but is not wired to this yet.  See NOTDONE.md.
            loop_count_ = 0;
            send_loop_error (rc_unrecognized_function, 0xffff);
            return;
        }
        if (loop_conn_) {
            // One loop at a time on one connection.
            send_loop_error (rc_operation_failure, 0xffff);
            return;
        }

        // Argument validation, in PyDECnet's order so the same bad request
        // gets the same complaint.
        std::uint8_t fill = 0x55;
        unsigned badarg = 0;
        switch (req.loop_with) {
        case 0: fill = 0x00; break;
        case 1: fill = 0xff; break;
        case 2: fill = 0x55; break;
        default: badarg = 152; break;
        }
        if (req.loop_length < 1 || req.loop_length > 65535) badarg = 151;
        if (req.loop_count < 1) badarg = 150;
        loop_count_ = static_cast<int> (req.loop_count);
        if (badarg) {
            send_loop_error (rc_invalid_parameter, badarg);
            return;
        }

        // Loop requests do not go through Node::nice_read, so resolve a node name
        // here.
        Nodeid target = req.entity.id;
        if (req.entity.code > 0) {
            Nodeinfo *info = node_ ? node_->find_node (req.entity.name)
                                   : nullptr;
            if (!info) {
                send_loop_error (rc_mirror_connect_failed, 2);  // unknown node
                return;
            }
            target = info->id;
        }
        if (!target.value ()) {
            send_loop_error (rc_mirror_connect_failed, 2);
            return;
        }

        loop_data_.assign (req.loop_length, fill);

        session::Session *s = node_ ? node_->session () : nullptr;
        if (!s) { send_loop_error (rc_mirror_connect_failed, 0); return; }

        session::ConnectData cd;
        cd.dstname = session::EndUser::number (mirror_object);
        cd.srcname = session::EndUser::named ("NML");
        cd.rqstrid = req.username;
        cd.passwrd = req.password;
        cd.account = req.account;
        cd.auth    = req.access_ctl;

        auto app = std::make_unique<LoopApplication> (this);
        loop_app_ = app.get ();
        loop_conn_ = s->connect (target, std::move (cd), std::move (app));
        if (!loop_conn_) {
            // No link available, or the node is not one we can reach.
            loop_app_ = nullptr;
            send_loop_error (rc_mirror_connect_failed, 2);
        }
    }

    void send_loop_message ()
    {
        if (!loop_conn_ || !loop_conn_->running ()) {
            abandon_loop ();
            send_loop_error (rc_mirror_disconnected, 0);
            return;
        }
        Bytes msg;
        msg.reserve (loop_data_.size () + 1);
        msg.push_back (0);                  // "loop this back"
        msg.insert (msg.end (), loop_data_.begin (), loop_data_.end ());
        loop_conn_->send_data (std::move (msg));
    }

    void close_loop ()
    {
        if (loop_app_) { loop_app_->orphan (); loop_app_ = nullptr; }
        if (loop_conn_) { loop_conn_->disconnect (); loop_conn_ = nullptr; }
    }

    // Drop the loop link without sending anything about it.
    void abandon_loop () { close_loop (); }

    Node                       *node_;
    session::SessionConnection *conn_ = nullptr;
    bool                        phase2_ = false;

    session::SessionConnection *loop_conn_ = nullptr;
    LoopApplication            *loop_app_ = nullptr;
    Bytes                       loop_data_;
    int                         loop_count_ = 0;
};

void LoopApplication::connect_received (session::SessionConnection &,
                                        ByteView data)
{
    if (nml_) nml_->loop_accepted (data);
}

void LoopApplication::data_received (session::SessionConnection &,
                                     ByteView data)
{
    if (nml_) nml_->loop_data (data);
}

void LoopApplication::disconnected (session::SessionConnection &,
                                    unsigned reason)
{
    if (nml_) nml_->loop_down (reason);
}

}   // namespace

std::unique_ptr<session::Application> make_nml (Node *node)
{
    return std::make_unique<NmlApplication> (node);
}

}   // namespace decnet::nice
