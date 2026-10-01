// api/server.cc -- the PyDECnet API over a Unix socket.
//
// Port of apiserver.py (the socket side) and of session.py's DictConnector
// and ApiConnector (the "session" API).

#include "decnet/api/server.h"

#include "decnet/common/json.h"
#include "decnet/common/logging.h"
#include "decnet/common/work.h"
#include "decnet/mop/mop.h"
#include "decnet/mop/names.h"
#include "decnet/node.h"
#include "decnet/session/session.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <optional>

#ifdef _WIN32
#include <io.h>
// No close-on-exec for sockets: the object spawner passes children only
// the handles it names.
#define SOCK_CLOEXEC 0
#define F_OK 0
#else
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace decnet::api {

using session::EndUser;
using session::SessionConnection;

namespace {

// The longest request line accepted.  A session data message is at most a
// few kilobytes; this is only a guard against a runaway client.
constexpr std::size_t MAX_LINE = 1 << 20;

// The most output waiting for a client before it counts as not reading.
constexpr std::size_t MAX_QUEUED = 64u << 20;

bool make_address (const std::string &path, sockaddr_un &a)
{
    std::memset (&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (path.size () >= sizeof a.sun_path) return false;
    std::memcpy (a.sun_path, path.c_str (), path.size () + 1);
    return true;
}

std::string upper (std::string s)
{
    std::transform (s.begin (), s.end (), s.begin (),
                    [] (unsigned char c) { return std::toupper (c); });
    return s;
}

}   // namespace

// ================================================================= Client

// One API connection.  The socket side runs on the reader thread; the
// session side (handles, bound objects) is node thread only.
class Client : public std::enable_shared_from_this<Client> {
public:
    Client (Node *node, Socket s) : node_ (node), sock_ (std::move (s)) {}
    ~Client () { stop (); }

    void start ()
    {
        reader_ = std::thread ([self = shared_from_this ()] { self->read (); });
        writer_ = std::thread ([self = shared_from_this ()] { self->write (); });
    }

    // Wake both threads and wait for them.  From one of their own threads,
    // which is where the last reference can drop, there is nothing to wait
    // for.
    void stop ()
    {
        {
            std::lock_guard l (out_m_);
            closing_ = true;
        }
        out_cv_.notify_all ();
        sock_.shutdown ();
        for (std::thread *t : { &reader_, &writer_ }) {
            if (!t->joinable ()) continue;
            if (t->get_id () == std::this_thread::get_id ()) t->detach ();
            else t->join ();
        }
    }

    bool finished () const noexcept { return done_; }
    bool gone () const noexcept { return gone_; }

    // Send one message.  Any thread.  Never blocks: messages queue for the
    // writer thread.  A client whose queue passes MAX_QUEUED is not reading,
    // and is dropped rather than allowed to use up memory.
    void send (const json::Object &o);

    // ------------------------------------------------- node thread only

    void handle (const json::Object &req);

    // The client has gone: abandon its links and withdraw its objects.
    // Port of the ApplicationExited clean up in BaseConnector.dispatch.
    void closed ();

    // Session events, from the applications below.
    std::int64_t add_inbound (SessionConnection &c, std::int64_t listen,
                              ByteView data);
    bool known (std::int64_t h) const { return conns_.count (h) != 0; }
    void forget (std::int64_t h) { conns_.erase (h); }
    void confirmed (std::int64_t h) { if (known (h)) conns_[h].confirmed = true; }
    bool is_confirmed (std::int64_t h) const
    {
        auto it = conns_.find (h);
        return it != conns_.end () && it->second.confirmed;
    }

    // A session message to the client: PyDECnet's send_dict adds the api
    // and system names to everything the session API sends.
    void send_session (json::Object o);

private:
    void read ();
    void write ();

    json::Object error (const std::string &text) const
    {
        json::Object o;
        o.set ("error", text);
        return o;
    }

    // Session API requests.  Each returns the reply, or nothing when the
    // request has no reply of its own, as with data.
    std::optional<json::Object> session_request (const std::string &type,
                                                 const json::Object &req);
    json::Object do_connect (const json::Object &req);

    // MOP API requests.  Those that ask another station answer later,
    // through send_mop, with tag.
    std::optional<json::Object> mop_request (const std::string &type,
                                             const json::Object &req,
                                             const json::Value *tag);
    void send_mop (json::Object o, const json::Value *tag);
    void mop_loop (mop::MopCircuit *c, std::vector<Macaddr> dest,
                   int packets, double timeout, bool fast,
                   std::shared_ptr<json::Value::Array> delays,
                   json::Value tag);
    // A station: a MAC address, or a node name or address, which stands
    // for its DECnet MAC address.
    std::optional<Macaddr> station (const json::Value *v) const;

    // handle() less the holding of events.
    void answer (const json::Object &req);
    json::Object do_bind (const json::Object &req);

    Node             *node_;
    Socket            sock_;
    std::thread       reader_, writer_;
    std::atomic<bool> done_ { false };

    // Messages waiting for the writer thread.
    std::mutex              out_m_;
    std::condition_variable out_cv_;
    std::deque<std::string> outq_;
    std::size_t             outq_bytes_ = 0;
    bool                    closing_ = false;

    // Node thread state.
    struct Conn {
        SessionConnection *sc = nullptr;
        bool outbound = false;
        // Outbound: accepted by the far end.  Inbound: accepted by us.
        bool confirmed = false;
    };
    struct Bound {
        std::uint8_t number = 0;
        std::string  name;
    };
    bool                            gone_ = false;
    bool                            holding_ = false;
    std::vector<json::Object>       held_;
    std::int64_t                    next_handle_ = 1;
    std::map<std::int64_t, Conn>    conns_;
    std::map<std::int64_t, Bound>   binds_;
};

// ---------------------------------------------------------- applications

namespace {

// The session side of one API conversation, in either direction.  Session
// control makes one per connection; it forwards events to the client as
// JSON, in the form DictConnector.dispatch2 sends them.
class ApiApplication : public session::Application {
public:
    // Outbound: the handle already given to the client.
    ApiApplication (std::shared_ptr<Client> client, std::int64_t handle)
        : client_ (std::move (client)), handle_ (handle), outbound_ (true) {}

    // Inbound, for an object the client bound.
    ApiApplication (std::shared_ptr<Client> client, std::int64_t listen,
                    bool)
        : client_ (std::move (client)), listen_ (listen) {}

    void connect_received (SessionConnection &c, ByteView data) override
    {
        if (!outbound_) {
            // The client may have gone since session control chose us.
            if (client_->gone ()) { c.reject (session::OBJ_FAIL); return; }
            handle_ = client_->add_inbound (c, listen_, data);
            return;
        }
        // Our connect was accepted; session control delivers that as a
        // connect so both directions look alike.
        if (!client_->known (handle_)) return;
        client_->confirmed (handle_);
        send ("accept", data);
    }

    void data_received (SessionConnection &, ByteView data) override
    {
        if (client_->known (handle_)) send ("data", data);
    }

    void interrupt_received (SessionConnection &, ByteView data) override
    {
        if (client_->known (handle_)) send ("interrupt", data);
    }

    void run_state (SessionConnection &) override
    {
        if (client_->known (handle_)) send ("runstate", {});
    }

    void disconnected (SessionConnection &, unsigned reason) override
    {
        if (!client_->known (handle_)) return;
        // An outbound link that never ran was rejected; the client's
        // connect() waits for exactly that word.
        bool rejected = outbound_ && !client_->is_confirmed (handle_);
        client_->forget (handle_);
        json::Object o;
        o.set ("handle", handle_);
        o.set ("data", "");
        o.set ("type", rejected ? "reject" : "disconnect");
        o.set ("reason", static_cast<std::int64_t> (reason));
        client_->send_session (std::move (o));
    }

private:
    void send (const char *type, ByteView data)
    {
        json::Object o;
        o.set ("handle", handle_);
        o.set_bytes ("data", data);
        o.set ("type", type);
        client_->send_session (std::move (o));
    }

    std::shared_ptr<Client> client_;
    std::int64_t            handle_ = 0;
    std::int64_t            listen_ = 0;
    bool                    outbound_ = false;
};

}   // namespace

// -------------------------------------------------------- client, socket

void Client::read ()
{
    logging::set_thread_name (node_ ? node_->name () + ".api" : "api");
    DN_TRACE ("API connection start");
    std::string pending;
    char buf[4096];
    for (;;) {
        ssize_t n = sock_recv (sock_.fd (), buf, sizeof buf);
        if (n < 0 && sock_interrupted (sock_errno ())) continue;
        if (n <= 0) break;
        pending.append (buf, static_cast<std::size_t> (n));

        std::size_t nl;
        while ((nl = pending.find ('\n')) != std::string::npos) {
            std::string line = pending.substr (0, nl);
            pending.erase (0, nl + 1);
            DN_TRACE ("API request: {}", line);
            json::Object req;
            try {
                req = json::Object::parse (line);
            } catch (const std::exception &e) {
                json::Object o = error ("Parse error");
                o.set ("exception", e.what ());
                send (o);
                continue;
            }
            node_->add_work (std::make_unique<CallbackWork> (
                [self = shared_from_this (), req = std::move (req)] {
                    self->handle (req);
                }));
        }
        if (pending.size () > MAX_LINE) {
            DN_DEBUG ("API client sent a line longer than {} bytes", MAX_LINE);
            break;
        }
    }
    sock_.shutdown ();
    node_->add_work (std::make_unique<CallbackWork> (
        [self = shared_from_this ()] { self->closed (); }));
    done_ = true;
}

void Client::send (const json::Object &o)
{
    std::string text = o.encode ();
    DN_TRACE ("message to API client: {}", text);
    text += '\n';
    {
        std::lock_guard l (out_m_);
        if (closing_) return;
        if (outq_bytes_ + text.size () > MAX_QUEUED) {
            // A burst of data can run well ahead of a client -- a file
            // arrives as fast as the far node sends it, and binary data is
            // six times its size in JSON -- so only a queue this large says
            // the client has stopped reading.
            DN_WARN ("API client is not reading ({} bytes waiting); "
                     "disconnecting it", outq_bytes_);
            closing_ = true;
        } else {
            outq_bytes_ += text.size ();
            outq_.push_back (std::move (text));
        }
    }
    out_cv_.notify_one ();
    if (closing_) sock_.shutdown ();
}

void Client::write ()
{
    logging::set_thread_name (node_ ? node_->name () + ".api" : "api");
    for (;;) {
        std::string text;
        {
            std::unique_lock l (out_m_);
            out_cv_.wait (l, [&] { return closing_ || !outq_.empty (); });
            if (closing_) return;
            text = std::move (outq_.front ());
            outq_.pop_front ();
            outq_bytes_ -= text.size ();
        }
        std::size_t off = 0;
        while (off < text.size ()) {
            ssize_t n = sock_send (sock_.fd (), text.data () + off,
                                   text.size () - off);
            if (n < 0 && sock_interrupted (sock_errno ())) continue;
            if (n <= 0) {
                DN_DEBUG ("API send failure: {}", sock_strerror (sock_errno ()));
                std::lock_guard l (out_m_);
                closing_ = true;
                sock_.shutdown ();
                return;
            }
            off += static_cast<std::size_t> (n);
        }
    }
}

void Client::send_session (json::Object o)
{
    o.set ("api", "session");
    o.set ("system", node_->name ());
    // While a request is being handled its reply has not gone yet, and an
    // event it caused must not overtake it: a link to this same node runs
    // its whole handshake inside the connect request.
    if (holding_) held_.push_back (std::move (o));
    else          send (o);
}

// ------------------------------------------------------ client, requests

void Client::handle (const json::Object &req)
{
    if (gone_) return;
    holding_ = true;
    answer (req);
    holding_ = false;
    for (const json::Object &o : held_) send (o);
    held_.clear ();
}

void Client::answer (const json::Object &req)
{
    const json::Value *tag = req.get ("tag");
    std::size_t fields = req.size () - (tag ? 1 : 0);

    std::optional<json::Object> ret;
    if (fields == 0) {
        // An empty request asks for the list of systems and their APIs.
        ret.emplace ();
        json::Value::Array apis;
        if (node_->session ()) apis.push_back (json::Value ("session"));
        if (node_->mop () && !node_->mop ()->circuits ().empty ())
            apis.push_back (json::Value ("mop"));
        ret->set (node_->name (), json::Value (std::move (apis)));
    } else {
        std::string system = req.str ("system", node_->name ());
        std::string subsys = req.str ("api");
        std::string type   = req.str ("type", "get");
        if (subsys.empty ()) {
            ret = error ("required argument 'api' missing");
        } else if (upper (system) != upper (node_->name ())) {
            ret = error ("Unknown system name");
            ret->set ("system", system);
        } else if (subsys == "mop" && node_->mop ()
                   && !node_->mop ()->circuits ().empty ()) {
            ret = mop_request (type, req, tag);
            if (ret) {
                ret->set ("system", node_->name ());
                ret->set ("api", subsys);
            }
        } else if (subsys != "session" || !node_->session ()) {
            ret = error ("Unsupported api");
            ret->set ("api", subsys);
        } else {
            ret = session_request (type, req);
            if (ret) {
                ret->set ("system", node_->name ());
                ret->set ("api", subsys);
            }
        }
    }
    if (ret) {
        if (tag) ret->set ("tag", *tag);
        send (*ret);
    }
}

std::optional<json::Object>
Client::session_request (const std::string &type, const json::Object &req)
{
    if (type == "connect") return do_connect (req);
    if (type == "bind")    return do_bind (req);

    // Everything else acts on a handle.
    std::int64_t h = req.num ("handle");
    Bytes data = req.bytes ("data");

    if (auto b = binds_.find (h); b != binds_.end ()) {
        // Disconnecting a bind handle withdraws the object, as
        // ApiListener.disconnect does.
        if (type == "disconnect" || type == "abort") {
            node_->session ()->remove_object (b->second.number,
                                              b->second.name);
            binds_.erase (b);
            return std::nullopt;
        }
        return error ("Invalid request for a bound object");
    }

    auto it = conns_.find (h);
    if (it == conns_.end ()) {
        json::Object o = error ("Unknown handle");
        o.set ("handle", h);
        return o;
    }
    Conn &c = it->second;

    if (type == "accept") {
        c.confirmed = true;
        c.sc->accept (std::move (data));
    } else if (type == "reject") {
        c.sc->reject (session::APPLICATION, std::move (data));
        conns_.erase (it);
    } else if (type == "disconnect") {
        c.sc->disconnect (session::APPLICATION, std::move (data));
        conns_.erase (it);
    } else if (type == "abort") {
        c.sc->disconnect (session::ABORT, std::move (data));
        conns_.erase (it);
    } else if (type == "data") {
        c.sc->send_data (std::move (data));
    } else if (type == "interrupt") {
        if (!c.sc->interrupt (std::move (data)))
            DN_DEBUG ("API interrupt refused: no credit, or the link is not "
                      "running");
    } else if (type == "setsockopt") {
        // PORT: PyDECnet's only socket option is proxy, on connect.
    } else {
        json::Object o = error ("Invalid API request");
        o.set ("type", type);
        return o;
    }
    return std::nullopt;
}

json::Object Client::do_connect (const json::Object &req)
{
    // The destination is a node name or address.
    Nodeid dest;
    const json::Value *d = req.get ("dest");
    if (d && d->is_int ()) {
        dest = Nodeid (static_cast<std::uint16_t> (d->as_int ()));
    } else if (d && d->is_string ()) {
        if (Nodeinfo *info = node_->find_node (upper (d->as_string ())))
            dest = info->id;
        else
            try { dest = Nodeid::parse (d->as_string ()); }
            catch (const std::exception &) {}
    }
    if (!dest) {
        json::Object o;
        o.set ("type", "reject");
        o.set ("reason", static_cast<std::int64_t> (session::UNK_NODE));
        return o;
    }

    // Numbers are object numbers, anything else a name.
    auto end_user = [] (const json::Value *v, EndUser dflt) {
        if (!v || v->is_null ()) return dflt;
        if (v->is_int ())
            return EndUser::number (static_cast<std::uint8_t> (v->as_int ()));
        return EndUser::named (upper (v->to_text ()));
    };

    session::ConnectData cd;
    cd.dstname = end_user (req.get ("remuser"), EndUser ());
    cd.srcname = end_user (req.get ("localuser"),
                           EndUser::named ("PyDECnet"));
    cd.connectdata = req.bytes ("data");
    cd.rqstrid = req.str ("username");
    cd.passwrd = req.str ("password");
    cd.account = req.str ("account");
    if (const json::Value *p = req.get ("proxy"); p && p->is_bool ()
        && p->as_bool ()) {
        cd.scver = session::ConnectData::SCVER2;
        cd.proxy = true;
    }
    if (!cd.dstname.valid ())
        return error ("invalid remote user");

    // Known before connecting, since a link to this node may be accepted,
    // or rejected, before connect returns.
    std::int64_t h = next_handle_++;
    conns_[h] = Conn { nullptr, true, false };
    SessionConnection *sc = node_->session ()->connect (
        dest, std::move (cd),
        std::make_unique<ApiApplication> (shared_from_this (), h));
    if (!sc) {
        conns_.erase (h);
        return error ("cannot connect");
    }
    if (auto it = conns_.find (h); it != conns_.end ()) it->second.sc = sc;

    json::Object o;
    o.set ("handle", h);
    o.set ("type", "connecting");
    return o;
}

json::Object Client::do_bind (const json::Object &req)
{
    auto number = static_cast<std::uint8_t> (req.num ("num"));
    std::string name = upper (req.str ("name"));
    std::int64_t h = next_handle_++;
    // PORT: --auth is accepted and ignored, as all access control is.
    try {
        std::weak_ptr<Client> wp = weak_from_this ();
        node_->session ()->add_object (number, name,
            [wp, h] () -> std::unique_ptr<session::Application> {
                auto c = wp.lock ();
                if (!c) return nullptr;
                return std::make_unique<ApiApplication> (c, h, true);
            });
    } catch (const std::exception &e) {
        return error (e.what ());
    }
    binds_[h] = Bound { number, name };

    json::Object o;
    o.set ("handle", h);
    o.set ("type", "bind");
    return o;
}

std::int64_t Client::add_inbound (SessionConnection &c, std::int64_t listen,
                                  ByteView data)
{
    std::int64_t h = next_handle_++;
    conns_[h] = Conn { &c, false, false };
    json::Object o;
    o.set ("handle", h);
    o.set_bytes ("data", data);
    o.set ("type", "connect");
    o.set ("destination", c.remote ().str ());
    o.set ("srcuser", c.source ().str ());
    o.set ("dstuser", c.destination ().str ());
    o.set ("listenhandle", listen);
    if (!c.username ().empty ()) o.set ("username", c.username ());
    if (!c.password ().empty ()) o.set ("password", c.password ());
    if (!c.account ().empty ())  o.set ("account", c.account ());
    if (c.proxy ()) o.set ("proxy", true);
    if (const Nodeinfo *n = node_->find_node (c.remote (), false);
        n && !n->name.empty ())
        o.set ("nodename", n->name);
    send_session (std::move (o));
    return h;
}

// ------------------------------------------------------------ client, MOP
//
// Port of the api methods in mop.py.  PyDECnet's requests and answers, plus
// "sysid" with a "dest", which asks that station who it is.

namespace {

std::string mac_text (const Bytes &b)
{
    if (b.size () != 6) return {};
    std::array<std::uint8_t, 6> a {};
    std::copy (b.begin (), b.end (), a.begin ());
    return Macaddr (a).str ();
}

// One station's system ID, as PyDECnet's SysIdHandler.api gives it.
json::Object sysid_item (const mop::SysId &s, Macaddr src)
{
    json::Object o;
    o.set ("srcaddr", src.str ());
    if (s.version) o.set ("version", s.version->str ());
    if (s.console_user) o.set ("console_user", mac_text (*s.console_user));
    if (s.reservation_timer)
        o.set ("reservation_timer", static_cast<std::int64_t> (*s.reservation_timer));
    if (s.hwaddr) o.set ("hwaddr", mac_text (*s.hwaddr));
    if (s.device) {
        o.set ("device", mop::device_name (*s.device));
        o.set ("device_code", static_cast<std::int64_t> (*s.device));
    }
    if (s.processor) o.set ("processor", mop::processor_name (*s.processor));
    if (s.datalink) o.set ("datalink", mop::datalink_name (*s.datalink));
    if (s.bufsize) o.set ("bufsize", static_cast<std::int64_t> (*s.bufsize));
    if (s.software) o.set ("software", s.software->str ());
    json::Value::Array services;
    for (const std::string &n : s.services ()) services.emplace_back (n);
    o.set ("services", json::Value (std::move (services)));
    if (s.carrier_reserved) o.set ("carrier_reserved", true);
    return o;
}

}   // namespace

void Client::send_mop (json::Object o, const json::Value *tag)
{
    o.set ("system", node_->name ());
    o.set ("api", "mop");
    if (tag) o.set ("tag", *tag);
    if (holding_) held_.push_back (std::move (o));
    else          send (o);
}

std::optional<Macaddr> Client::station (const json::Value *v) const
{
    if (!v) return std::nullopt;
    if (v->is_int ())
        return Macaddr::from_nodeid (Nodeid (static_cast<std::uint16_t> (v->as_int ())));
    if (!v->is_string ()) return std::nullopt;
    const std::string &text = v->as_string ();
    try {
        return Macaddr::parse (text);
    } catch (const std::exception &) {}
    if (Nodeinfo *info = node_->find_node (upper (text)))
        return Macaddr::from_nodeid (info->id);
    try {
        Nodeid n = Nodeid::parse (text);
        if (n) return Macaddr::from_nodeid (n);
    } catch (const std::exception &) {}
    return std::nullopt;
}

std::optional<json::Object>
Client::mop_request (const std::string &type, const json::Object &req,
                     const json::Value *tag)
{
    mop::Mop *m = node_->mop ();
    if (type == "get") {
        json::Value::Array circuits;
        for (mop::MopCircuit *c : m->circuits ()) {
            json::Object o;
            o.set ("name", c->name ());
            o.set ("hwaddr", c->datalink ()->hwaddr ().str ());
            o.set ("macaddr", c->loop ()->macaddr ().str ());
            o.set ("services", json::Value (json::Value::Array {
                json::Value ("loop"), json::Value ("counters") }));
            circuits.emplace_back (std::move (o));
        }
        json::Object o;
        o.set ("circuits", json::Value (std::move (circuits)));
        return o;
    }

    // Everything else is on one circuit: the one named, or the only one.
    mop::MopCircuit *c = m->circuit (req.str ("circuit"));
    if (!c) {
        return error (req.has ("circuit") ? "invalid circuit argument"
                                          : "circuit argument needed");
    }
    std::int64_t t = req.num ("timeout", 3);
    if (t < 1 || t > 60) return error ("invalid timeout");
    double timeout = static_cast<double> (t);
    std::weak_ptr<Client> self = weak_from_this ();
    json::Value keep_tag = tag ? *tag : json::Value ();

    if (type == "sysid") {
        if (!req.has ("dest")) {
            json::Value::Array list;
            auto now = std::chrono::steady_clock::now ();
            for (const auto &[k, h] : c->sysid ()->heard ()) {
                json::Object o = sysid_item (h.sysid, h.address);
                o.set ("age", static_cast<std::int64_t> (
                    std::chrono::duration_cast<std::chrono::seconds> (
                        now - h.last_heard).count ()));
                list.emplace_back (std::move (o));
            }
            json::Object o;
            o.set ("sysid", json::Value (std::move (list)));
            return o;
        }
        auto dest = station (req.get ("dest"));
        if (!dest) return error ("invalid dest");
        c->request_id (*dest, timeout,
                       [self, keep_tag] (const mop::SysId *s, Macaddr from) {
            auto me = self.lock ();
            if (!me || me->gone_) return;
            json::Object o;
            if (!s) {
                o.set ("status", "timeout");
            } else {
                o.set ("status", "ok");
                o.set ("sysid", json::Value (json::Value::Array {
                    json::Value (sysid_item (*s, from)) }));
            }
            me->send_mop (std::move (o), keep_tag.is_null () ? nullptr : &keep_tag);
        });
        return std::nullopt;
    }

    if (type == "counters") {
        auto dest = station (req.get ("dest"));
        if (!dest) return error ("invalid dest");
        c->request_counters (*dest, timeout,
                             [self, keep_tag] (const mop::Counters *k, Macaddr from) {
            auto me = self.lock ();
            if (!me || me->gone_) return;
            json::Object o;
            if (!k) {
                o.set ("status", "timeout");
            } else {
                o.set ("status", "ok");
                o.set ("srcaddr", from.str ());
                auto n = [&] (const char *name, std::uint32_t v) {
                    o.set (name, static_cast<std::int64_t> (v));
                };
                n ("time_since_zeroed", k->time_since_zeroed);
                n ("bytes_recv", k->bytes_recv);
                n ("bytes_sent", k->bytes_sent);
                n ("pkts_recv", k->pkts_recv);
                n ("pkts_sent", k->pkts_sent);
                n ("mcbytes_recv", k->mcbytes_recv);
                n ("mcpkts_recv", k->mcpkts_recv);
                n ("pkts_deferred", k->pkts_deferred);
                n ("pkts_1_collision", k->pkts_1_collision);
                n ("pkts_mult_collision", k->pkts_mult_collision);
                n ("send_fail", k->send_fail);
                n ("send_reasons", k->send_reasons);
                n ("recv_fail", k->recv_fail);
                n ("recv_reasons", k->recv_reasons);
                n ("unk_dest", k->unk_dest);
                n ("data_overrun", k->data_overrun);
                n ("no_sys_buf", k->no_sys_buf);
                n ("no_user_buf", k->no_user_buf);
            }
            me->send_mop (std::move (o), keep_tag.is_null () ? nullptr : &keep_tag);
        });
        return std::nullopt;
    }

    if (type == "loop") {
        // dest: one station or a list of up to three, the loop multicast
        // address if none.  The message goes through each in turn and
        // back here.
        std::vector<Macaddr> dest;
        const json::Value *d = req.get ("dest");
        if (d && d->is_array ()) {
            for (const json::Value &e : d->as_array ()) {
                auto a = station (&e);
                if (!a) return error ("invalid dest");
                dest.push_back (*a);
            }
        } else if (d) {
            auto a = station (d);
            if (!a) return error ("invalid dest");
            dest.push_back (*a);
        }
        if (dest.empty ()) dest.push_back (mop::loop_multicast ());
        if (dest.size () > 3) {
            json::Object o;
            o.set ("status", "too many addresses");
            return o;
        }
        for (std::size_t i = 0; i < dest.size (); ++i) {
            if (dest[i].is_multicast ()
                && !(i == 0 && dest.size () == 1 && dest[0] == mop::loop_multicast ())) {
                json::Object o;
                o.set ("status", "invalid address");
                return o;
            }
        }
        std::int64_t packets = req.num ("packets", 1);
        if (packets < 1 || packets > 10000) {
            json::Object o;
            o.set ("status", "invalid arguments");
            return o;
        }
        const json::Value *f = req.get ("fast");
        bool fast = f && f->is_bool () && f->as_bool ();
        mop_loop (c, std::move (dest), static_cast<int> (packets), timeout, fast,
                  std::make_shared<json::Value::Array> (), keep_tag);
        return std::nullopt;
    }

    json::Object o = error ("Unsupported operation");
    o.set ("type", type);
    return o;
}

// One loop message, then the next, until packets have gone: port of
// LoopConnection.  The answer lists each round trip in seconds, -1 for one
// that timed out, and the station that answered first.
void Client::mop_loop (mop::MopCircuit *c, std::vector<Macaddr> dest,
                       int packets, double timeout, bool fast,
                       std::shared_ptr<json::Value::Array> delays,
                       json::Value tag)
{
    static const std::string python = "Python! ";
    Bytes payload;
    for (int i = 0; i < 12; ++i) payload.insert (payload.end (), python.begin (), python.end ());

    std::vector<Macaddr> then (dest.begin () + 1, dest.end ());
    auto sent = std::chrono::steady_clock::now ();
    std::weak_ptr<Client> self = weak_from_this ();
    c->loop (dest[0], then, std::move (payload), timeout,
             [self, c, dest, packets, timeout, fast, delays, tag, sent]
             (bool ok, Macaddr from) mutable {
        auto me = self.lock ();
        if (!me || me->gone_) return;
        if (ok) {
            delays->emplace_back (std::chrono::duration<double> (
                std::chrono::steady_clock::now () - sent).count ());
            // A loop to the multicast address is answered by somebody;
            // later messages go to them.
            if (dest[0] == mop::loop_multicast ()) dest[0] = from;
        } else {
            delays->emplace_back (static_cast<std::int64_t> (-1));
        }
        if (static_cast<int> (delays->size ()) >= packets) {
            json::Object o;
            o.set ("status", "ok");
            o.set ("dest", dest[0].str ());
            o.set ("delays", json::Value (*delays));
            me->send_mop (std::move (o), tag.is_null () ? nullptr : &tag);
            return;
        }
        auto next = [self, c, dest, packets, timeout, fast, delays, tag] {
            if (auto client = self.lock (); client && !client->gone_)
                client->mop_loop (c, dest, packets, timeout, fast, delays, tag);
        };
        // A second between messages that were answered, as PyDECnet
        // does, unless asked to hurry.
        if (ok && !fast) c->after (1.0, next);
        else             next ();
    });
}

void Client::closed ()
{
    if (gone_) return;
    gone_ = true;
    DN_TRACE ("API client gone: {} links, {} objects", conns_.size (),
              binds_.size ());
    for (auto &[h, b] : binds_)
        node_->session ()->remove_object (b.number, b.name);
    binds_.clear ();
    // Links still open fail, as they do when an object program exits.
    auto conns = std::move (conns_);
    conns_.clear ();
    for (auto &[h, c] : conns) {
        if (!c.outbound && !c.confirmed)
            c.sc->reject (session::OBJ_FAIL);
        else
            c.sc->disconnect (session::OBJ_FAIL);
    }
}

// ================================================================= Server

Server::Server (Node *node, std::string path, unsigned mode)
    : Element (node), node_ (node), path_ (std::move (path)), mode_ (mode)
{
}

Server::~Server () { stop (); }

bool Server::start ()
{
    sockaddr_un a;
    if (!make_address (path_, a)) {
        DN_ERROR ("api: socket path too long: {}", path_);
        return false;
    }
    // A socket file left by a server that died is removed; one that
    // answers belongs to a server still running.
    if (::access (path_.c_str (), F_OK) == 0) {
        Socket probe (sock_open (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC));
        if (probe && ::connect (probe.fd (), reinterpret_cast<sockaddr *> (&a),
                                sizeof a) == 0) {
            DN_ERROR ("api: another server is already using {}", path_);
            return false;
        }
        ::unlink (path_.c_str ());
    }
    listener_ = Socket (sock_open (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC));
    if (!listener_
        || ::bind (listener_.fd (), reinterpret_cast<sockaddr *> (&a),
                   sizeof a) < 0) {
        DN_ERROR ("api: cannot bind {}: {}", path_,
                  sock_strerror (sock_errno ()));
        listener_.close ();
        return false;
    }
#ifndef _WIN32
    // On Windows the socket file takes its directory's ACL instead.
    ::chmod (path_.c_str (), mode_);
#endif
    if (::listen (listener_.fd (), 8) < 0) {
        DN_ERROR ("api: cannot listen on {}: {}", path_,
                  sock_strerror (sock_errno ()));
        listener_.close ();
        ::unlink (path_.c_str ());
        return false;
    }
    DN_INFO ("api: listening on {}", path_);
    thread_ = std::thread ([this] { run (); });
    return true;
}

void Server::stop ()
{
    if (!thread_.joinable ()) return;
    stopping_ = true;
    // Shutting the listener down is what wakes the accept.
    listener_.shutdown ();
#ifdef _WIN32
    // Shutdown does not stop a later accept on Windows; closing does.
    listener_.close ();
#endif
    thread_.join ();
    listener_.close ();
    ::unlink (path_.c_str ());

    std::vector<std::shared_ptr<Client>> clients;
    {
        std::lock_guard l (clients_m_);
        clients.swap (clients_);
    }
    for (auto &c : clients) c->stop ();
    DN_DEBUG ("api: shut down");
}

void Server::run ()
{
    logging::set_thread_name (node_ ? node_->name () : "api");
    while (!stopping_) {
#ifdef _WIN32
        int fd = sock_accept (listener_.fd ());
#else
        int fd = ::accept4 (listener_.fd (), nullptr, nullptr, SOCK_CLOEXEC);
#endif
        if (fd < 0) {
            if (sock_interrupted (sock_errno ())) continue;
            break;                      // listener shut down
        }
        reap ();
        auto c = std::make_shared<Client> (node_, Socket (fd));
        c->start ();
        std::lock_guard l (clients_m_);
        clients_.push_back (std::move (c));
    }
}

void Server::reap ()
{
    std::vector<std::shared_ptr<Client>> done;
    {
        std::lock_guard l (clients_m_);
        auto mid = std::partition (clients_.begin (), clients_.end (),
                                   [] (const auto &c) { return !c->finished (); });
        done.assign (std::make_move_iterator (mid),
                     std::make_move_iterator (clients_.end ()));
        clients_.erase (mid, clients_.end ());
    }
    for (auto &c : done) c->stop ();
}

std::size_t Server::client_count ()
{
    reap ();
    std::lock_guard l (clients_m_);
    return clients_.size ();
}

}   // namespace decnet::api
