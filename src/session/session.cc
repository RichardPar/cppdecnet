#include "decnet/session/session.h"
#include "decnet/events/logger.h"

#include "decnet/common/logging.h"
#include "decnet/config.h"
#include "decnet/nice/nml.h"
#include "decnet/node.h"
#include "decnet/session/process.h"

#include <algorithm>

namespace decnet::session {

// ------------------------------------------------------ SessionConnection

void SessionConnection::accept (Bytes data, std::uint8_t fcopt)
{
    if (conn_) conn_->accept (std::move (data), fcopt);
}

void SessionConnection::reject (unsigned reason, Bytes data)
{
    if (conn_) conn_->reject (reason, std::move (data));
}

void SessionConnection::send_data (Bytes data)
{
    if (conn_) conn_->send_data (std::move (data));
}

void SessionConnection::disconnect (unsigned reason, Bytes data)
{
    if (conn_) conn_->disconnect (reason, std::move (data));
}

bool SessionConnection::interrupt (Bytes data)
{
    return conn_ && conn_->send_interrupt (std::move (data));
}

bool SessionConnection::can_interrupt () const noexcept
{
    return conn_ && conn_->can_interrupt ();
}

bool SessionConnection::running () const noexcept
{
    return conn_ && conn_->running ();
}

Nodeid SessionConnection::remote () const noexcept
{
    return conn_ ? conn_->dest () : Nodeid ();
}

// ---------------------------------------------------------------- Session

Session::Session (Element *parent, const Config &config)
    : Element (parent)
{
    DN_DEBUG ("initializing session control");
    // Objects declared in the configuration file.  Each names a program
    // that is run when a connection for it arrives.
    for (const ObjectConfig &o : config.objects ()) {
        try {
            add_object (static_cast<std::uint8_t> (o.number), o.name,
                        process_application (node (), o.file, o.arguments));
            DN_DEBUG ("object {} ({}) is program {}", o.number,
                      o.name.empty () ? "-" : o.name, o.file);
        } catch (const std::exception &e) {
            DN_ERROR ("cannot register object {} ({}): {}", o.number,
                      o.name, e.what ());
        }
    }
}

Session::~Session () = default;

void Session::start ()
{
    DN_DEBUG ("starting session control");
    for (const Object &o : objects_)
        DN_DEBUG ("session control object {} ({})", o.number,
                  o.name.empty () ? "-" : o.name);
}

void Session::stop ()
{
    DN_DEBUG ("stopping session control");
    for (auto &[c, l] : live_)
        finished_.push_back (Retired { std::move (l),
                                       std::chrono::steady_clock::now () });
    live_.clear ();
}

void Session::add_object (std::uint8_t number, std::string name,
                          ApplicationFactory factory)
{
    if (!number && name.empty ())
        throw std::invalid_argument ("an object needs a number or a name");
    std::transform (name.begin (), name.end (), name.begin (),
                    [] (unsigned char c) { return std::toupper (c); });
    if (name.size () > 16)
        throw std::invalid_argument ("object name longer than 16 characters");
    if (number && by_number_.count (number))
        throw std::invalid_argument ("object number " + std::to_string (number)
                                     + " already registered");
    if (!name.empty () && by_name_.count (name))
        throw std::invalid_argument ("object " + name + " already registered");

    std::size_t idx = objects_.size ();
    objects_.push_back (Object { number, name, std::move (factory) });
    if (number)         by_number_[number] = idx;
    if (!name.empty ()) by_name_[name] = idx;
}

const Object *Session::find_object (std::uint8_t number) const
{
    auto it = by_number_.find (number);
    return it == by_number_.end () ? nullptr : &objects_[it->second];
}

const Object *Session::find_object (const std::string &name) const
{
    std::string key = name;
    std::transform (key.begin (), key.end (), key.begin (),
                    [] (unsigned char c) { return std::toupper (c); });
    auto it = by_name_.find (key);
    return it == by_name_.end () ? nullptr : &objects_[it->second];
}

Session::Live *Session::find_live (nsp::Connection &c)
{
    auto it = live_.find (&c);
    return it == live_.end () ? nullptr : &it->second;
}

void Session::retire (nsp::Connection &c)
{
    auto it = live_.find (&c);
    if (it == live_.end ()) return;
    // Before, not after: see the note in NSP::close_connection.  The
    // conversation being retired is retired from inside a callback into
    // the application it owns, and must outlive this dispatch.
    sweep_finished ();

    finished_.push_back (Retired { std::move (it->second),
                                   std::chrono::steady_clock::now () });
    live_.erase (it);
}

void Session::sweep_finished ()
{
    auto now = std::chrono::steady_clock::now ();
    std::size_t before = finished_.size ();
    std::erase_if (finished_, [&] (const Retired &r) {
        return now - r.when >= finished_grace_;
    });
    if (std::size_t gone = before - finished_.size ())
        DN_TRACE ("reclaimed {} finished conversation(s), {} still held",
                  gone, finished_.size ());
}

// ---------------------------------------------------------------- inbound

void Session::connect_received (nsp::Connection &c, ByteView payload)
{
    ConnectData cd;
    try {
        cd = ConnectData::parse_message (payload);
    } catch (const DecodeError &e) {
        DN_DEBUG ("invalid connect message from {}: {}", c.dest ().str (),
                  e.what ());
        c.reject (BAD_FMT);
        return;
    }

    // Which object is being asked for?  By number if it has one, else by
    // name; that is the order the Python checks in.
    const Object *obj = nullptr;
    if (cd.dstname.fmt == EndUser::by_number && cd.dstname.num)
        obj = find_object (cd.dstname.num);
    else if (!cd.dstname.name.empty ())
        obj = find_object (cd.dstname.name);

    if (!obj || !obj->factory) {
        DN_DEBUG ("no such object {} asked for by {}", cd.dstname.str (),
                  c.dest ().str ());
        c.reject (NO_OBJ);
        return;
    }

    DN_TRACE ("connection from {} to object {}", c.dest ().str (),
              cd.dstname.str ());
    Live live;
    live.conn = std::make_unique<SessionConnection> (this, &c);
    live.conn->dstname_ = cd.dstname;
    live.conn->srcname_ = cd.srcname;
    live.app = obj->factory ();
    Live &l = live_[&c] = std::move (live);
    l.app->connect_received (
        *l.conn, ByteView (cd.connectdata.data (), cd.connectdata.size ()));
}

// --------------------------------------------------------------- outbound

SessionConnection *Session::connect (Nodeid dest, EndUser dstname,
                                     EndUser srcname, Bytes connectdata,
                                     std::unique_ptr<Application> app)
{
    if (!dstname.valid () || !srcname.valid ()) {
        DN_ERROR ("invalid end user in connect to {}", dest.str ());
        return nullptr;
    }
    ConnectData cd;
    cd.dstname     = std::move (dstname);
    cd.srcname     = std::move (srcname);
    cd.connectdata = std::move (connectdata);
    return connect (dest, std::move (cd), std::move (app));
}

SessionConnection *Session::connect (Nodeid dest, ConnectData cd,
                                     std::unique_ptr<Application> app)
{
    if (!cd.dstname.valid () || !cd.srcname.valid ()) {
        DN_ERROR ("invalid end user in connect to {}", dest.str ());
        return nullptr;
    }
    Bytes payload;
    try {
        payload = cd.encode_message ();
    } catch (const DecodeError &e) {
        DN_ERROR ("cannot build connect message: {}", e.what ());
        return nullptr;
    }

    nsp::NSP *n = node () ? node ()->nsp () : nullptr;
    if (!n) return nullptr;
    nsp::Connection *c = n->connect (dest, std::move (payload));
    if (!c) return nullptr;

    Live live;
    live.conn = std::make_unique<SessionConnection> (this, c);
    live.conn->dstname_ = cd.dstname;
    live.conn->srcname_ = cd.srcname;
    live.app = std::move (app);
    Live &l = live_[c] = std::move (live);
    return l.conn.get ();
}

// -------------------------------------------------- the rest of the API

void Session::connect_confirmed (nsp::Connection &c, ByteView data)
{
    Live *l = find_live (c);
    if (!l) return;
    // An accept is delivered to the application as a connect, so that an
    // outbound and an inbound conversation look the same to it.
    l->app->connect_received (*l->conn, data);
}

void Session::connect_rejected (nsp::Connection &c, unsigned reason,
                                ByteView)
{
    Live *l = find_live (c);
    if (!l) return;
    l->app->disconnected (*l->conn, reason);
    retire (c);
}

void Session::data_received (nsp::Connection &c, ByteView data)
{
    Live *l = find_live (c);
    if (!l) return;
    l->app->data_received (*l->conn, data);
}

void Session::interrupt_received (nsp::Connection &c, ByteView data)
{
    Live *l = find_live (c);
    if (!l) return;
    l->app->interrupt_received (*l->conn, data);
}

void Session::disconnected (nsp::Connection &c, unsigned reason, ByteView)
{
    Live *l = find_live (c);
    if (!l) return;
    l->app->disconnected (*l->conn, reason);
    retire (c);
}

// ----------------------------------------------------------------- MIRROR

namespace {

// The network management loopback object.  Port of modules/mirror.py.
class Mirror : public Application {
public:
    void connect_received (SessionConnection &c, ByteView) override
    {
        // The accept data is the largest message we will take, as a two
        // byte little endian value.  There is no limit here, so say so.
        Bytes limit { 0xff, 0xff };
        c.accept (limit);
    }

    void data_received (SessionConnection &c, ByteView data) override
    {
        if (data.empty ()) return;
        if (data[0] == 0) {
            // Function code 0 is "loop this back".  Answer with the
            // success status followed by the data.
            Bytes reply;
            reply.reserve (data.size ());
            reply.push_back (0x01);
            reply.insert (reply.end (), data.begin () + 1, data.end ());
            c.send_data (std::move (reply));
        } else {
            c.send_data (Bytes { 0xff });        // failure status
        }
    }
};

}   // namespace

std::unique_ptr<Application> make_mirror ()
{
    return std::make_unique<Mirror> ();
}

void add_default_objects (Session &s)
{
    // Object 25 is MIRROR, which is enabled by default upstream too: it is
    // what NCP LOOP NODE talks to, so a node without it cannot be tested
    // by the standard means.
    //
    // A configuration line for the same object wins: the built-in is a
    // default, not a fixture, and someone who names a program for object
    // 25 means it.
    if (!s.find_object (25) && !s.find_object ("MIRROR"))
        s.add_object (25, "MIRROR", [] { return make_mirror (); });
    // Object 26 is the event logger's receiving end: another node connects
    // to it to send us its event records.
    if (!s.find_object (26) && !s.find_object ("EVENTLOGGER")) {
        Node *n = s.node ();
        s.add_object (26, "EVENTLOGGER",
                      [n] { return events::make_event_receiver (n); });
    }
    // Object 19 is the network management listener, which is what NCP on
    // another node connects to.  Enabled by default, as upstream does.
    if (!s.find_object (19) && !s.find_object ("NML")) {
        Node *n = s.node ();
        s.add_object (19, "NML", [n] { return nice::make_nml (n); });
    }
    // PORT: pmr (123) is the other object the Python enables by default.
}

}   // namespace decnet::session
