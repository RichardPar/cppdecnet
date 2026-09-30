// api/server.cc -- the PyDECnet API over a Unix socket.
//
// Port of apiserver.py (the socket side) and of session.py's DictConnector
// and ApiConnector (the "session" API).

#include "decnet/api/server.h"

#include "decnet/common/json.h"
#include "decnet/common/logging.h"
#include "decnet/common/work.h"
#include "decnet/node.h"
#include "decnet/session/session.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <optional>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace decnet::api {

using session::EndUser;
using session::SessionConnection;

namespace {

// The longest request line accepted.  A session data message is at most a
// few kilobytes; this is only a guard against a runaway client.
constexpr std::size_t MAX_LINE = 1 << 20;

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
    }

    // Wake the reader and wait for it.  From the reader's own thread, which
    // is where the last reference can drop, there is nothing to wait for.
    void stop ()
    {
        sock_.shutdown ();
        if (!reader_.joinable ()) return;
        if (reader_.get_id () == std::this_thread::get_id ())
            reader_.detach ();
        else
            reader_.join ();
    }

    bool finished () const noexcept { return done_; }
    bool gone () const noexcept { return gone_; }

    // Send one message.  Any thread.  Never blocks: a client whose socket
    // buffer is full is not reading, and is dropped rather than allowed to
    // stall the node.
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

    // handle() less the holding of events.
    void answer (const json::Object &req);
    json::Object do_bind (const json::Object &req);

    Node             *node_;
    Socket            sock_;
    std::thread       reader_;
    std::atomic<bool> done_ { false };
    std::mutex        write_m_;

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
        ssize_t n = ::recv (sock_.fd (), buf, sizeof buf, 0);
        if (n < 0 && errno == EINTR) continue;
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
    std::lock_guard l (write_m_);
    std::size_t off = 0;
    while (off < text.size ()) {
        ssize_t n = ::send (sock_.fd (), text.data () + off, text.size () - off,
                            MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                DN_WARN ("API client is not reading; disconnecting it");
            else
                DN_DEBUG ("API send failure: {}", std::strerror (errno));
            // Part of a line may have gone, so the stream is unusable.
            sock_.shutdown ();
            return;
        }
        off += static_cast<std::size_t> (n);
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
        ret->set (node_->name (), json::Value (json::Value::Array {
            json::Value ("session") }));
    } else {
        std::string system = req.str ("system", node_->name ());
        std::string subsys = req.str ("api");
        std::string type   = req.str ("type", "get");
        if (subsys.empty ()) {
            ret = error ("required argument 'api' missing");
        } else if (upper (system) != upper (node_->name ())) {
            ret = error ("Unknown system name");
            ret->set ("system", system);
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
    send_session (std::move (o));
    return h;
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
        Socket probe (::socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
        if (probe && ::connect (probe.fd (), reinterpret_cast<sockaddr *> (&a),
                                sizeof a) == 0) {
            DN_ERROR ("api: another server is already using {}", path_);
            return false;
        }
        ::unlink (path_.c_str ());
    }
    listener_ = Socket (::socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!listener_
        || ::bind (listener_.fd (), reinterpret_cast<sockaddr *> (&a),
                   sizeof a) < 0) {
        DN_ERROR ("api: cannot bind {}: {}", path_, std::strerror (errno));
        listener_.close ();
        return false;
    }
    ::chmod (path_.c_str (), mode_);
    if (::listen (listener_.fd (), 8) < 0) {
        DN_ERROR ("api: cannot listen on {}: {}", path_, std::strerror (errno));
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
        int fd = ::accept4 (listener_.fd (), nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
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
