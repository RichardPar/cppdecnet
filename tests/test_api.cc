// Tests for the API server: the PyDECnet JSON protocol over a Unix socket.

#include "harness.h"

#include "decnet/api/server.h"
#include "decnet/common/json.h"
#include "decnet/common/socket.h"
#include "decnet/config.h"
#include "decnet/node.h"
#include "decnet/routing/routing.h"
#include "decnet/session/session.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace decnet;

namespace {

std::uint16_t free_port ()
{
    SourceAddress any ("127.0.0.1", 0);
    Socket s = any.create_server ();
    if (!s) throw std::runtime_error ("cannot find a free port");
    sockaddr_storage sa {};
    socklen_t len = sizeof sa;
    ::getsockname (s.fd (), reinterpret_cast<sockaddr *> (&sa), &len);
    return ntohs (reinterpret_cast<sockaddr_in *> (&sa)->sin_port);
}

template <typename P>
bool wait_until (P pred, std::chrono::milliseconds timeout
                     = std::chrono::seconds (15))
{
    auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        if (pred ()) return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    return pred ();
}

// A socket path short enough for sun_path, distinct per test.
std::string socket_path ()
{
    static int n = 0;
    return "/tmp/dnapi-" + std::to_string (::getpid ()) + "-"
        + std::to_string (++n) + ".sock";
}

Bytes bytes_of (const std::string &s) { return Bytes (s.begin (), s.end ()); }

// A client of the API, speaking JSON lines as connectors.py does.
class ApiClient {
public:
    explicit ApiClient (const std::string &path)
    {
        sock_ = Socket (::socket (AF_UNIX, SOCK_STREAM, 0));
        sockaddr_un a {};
        a.sun_family = AF_UNIX;
        std::strncpy (a.sun_path, path.c_str (), sizeof a.sun_path - 1);
        if (::connect (sock_.fd (), reinterpret_cast<sockaddr *> (&a),
                       sizeof a) < 0)
            sock_.close ();
    }

    bool connected () const { return sock_.valid (); }
    void close () { sock_.close (); }

    void send (const json::Object &o)
    {
        std::string text = o.encode () + "\n";
        (void) ::send (sock_.fd (), text.data (), text.size (), MSG_NOSIGNAL);
    }

    // The next message, or nothing if none arrives in time.
    std::optional<json::Object> recv (int ms = 15000)
    {
        auto deadline = std::chrono::steady_clock::now ()
                      + std::chrono::milliseconds (ms);
        for (;;) {
            std::size_t nl = pending_.find ('\n');
            if (nl != std::string::npos) {
                std::string line = pending_.substr (0, nl);
                pending_.erase (0, nl + 1);
                return json::Object::parse (line);
            }
            auto left = std::chrono::duration_cast<std::chrono::milliseconds> (
                deadline - std::chrono::steady_clock::now ()).count ();
            if (left <= 0) return std::nullopt;
            PollResult r = poll_socket (sock_.fd (), true, false,
                                        static_cast<int> (left));
            if (!r.readable) return std::nullopt;
            char buf[4096];
            ssize_t n = ::recv (sock_.fd (), buf, sizeof buf, 0);
            if (n <= 0) return std::nullopt;
            pending_.append (buf, static_cast<std::size_t> (n));
        }
    }

    // Send a session request and return the reply.
    std::optional<json::Object> session (const std::string &type,
                                         json::Object o = {})
    {
        o.set ("api", "session");
        o.set ("type", type);
        send (o);
        return recv ();
    }

private:
    Socket      sock_;
    std::string pending_;
};

// Two level 1 routers joined by a circuit, each with an API socket.
// Routers, not endnodes, because a connect made right after the adjacency
// comes up is the case that once failed.
struct Pair {
    std::uint16_t port = free_port ();
    std::string apath = socket_path (), bpath = socket_path ();
    Config acfg, bcfg;
    std::unique_ptr<Node> a, b;

    Pair ()
        : acfg (Config::from_string (
              "routing 1.1 --type l1router\nnode 1.1 NODEA\nnode 1.2 NODEB\n"
              "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
              + ":listen --t3 2\napi " + apath + "\n")),
          bcfg (Config::from_string (
              "routing 1.2 --type l1router\nnode 1.2 NODEB\nnode 1.1 NODEA\n"
              "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
              + ":connect --t3 2\napi " + bpath + "\n"))
    {
        a = std::make_unique<Node> (acfg);
        b = std::make_unique<Node> (bcfg);
    }

    void start ()
    {
        a->start ();
        b->start ();
        wait_until ([&] {
            return a->routing ()->adjacency_count () == 1
                && b->routing ()->adjacency_count () == 1;
        });
    }
    void stop () { if (b) b->stop (); if (a) a->stop (); }
    ~Pair () { stop (); }
};

// One node on its own, for the requests that need no network.
struct Single {
    std::string path;
    Config cfg;
    std::unique_ptr<Node> n;

    explicit Single (std::string p = socket_path ())
        : path (std::move (p)),
          cfg (Config::from_string (
              "routing 1.1 --type endnode\nnode 1.1 NODEA\n"
              "circuit mul-0 Multinet 127.0.0.1:1:connect\napi " + path + "\n"))
    {
        n = std::make_unique<Node> (cfg);
        n->start ();
    }
    ~Single () { n->stop (); }
};

}   // namespace

// ------------------------------------------------------------ configuration

DN_TEST (api, the_api_line_is_parsed)
{
    Config c = Config::from_string ("api /tmp/x.sock --mode 660\n");
    DN_ASSERT_EQ (c.api_socket (), std::string ("/tmp/x.sock"));
    DN_ASSERT_EQ (c.api_mode (), 0660u);

    // No name means the PyDECnet default.
    ::unsetenv ("DECNETAPI");
    Config d = Config::from_string ("api\n");
    DN_ASSERT_EQ (d.api_socket (), std::string ("/tmp/decnetapi.sock"));
    DN_ASSERT_EQ (d.api_mode (), 0666u);

    DN_ASSERT_THROWS (std::runtime_error,
                      Config::from_string ("api --mode 999\n"));
    DN_ASSERT (Config::from_string ("routing 1.1\n").api_socket ().empty ());
}

// ---------------------------------------------------------------- requests

DN_TEST (api, an_empty_request_lists_the_system)
{
    Single s;
    ApiClient c (s.path);
    DN_ASSERT (c.connected ());

    json::Object req;
    req.set ("tag", 7);
    c.send (req);
    auto r = c.recv ();
    DN_ASSERT (r.has_value ());
    // The node's name maps to the APIs it offers.
    DN_ASSERT (r->get ("NODEA") != nullptr);
    DN_ASSERT_EQ (r->get ("NODEA")->encode (), std::string ("[\"session\"]"));
    DN_ASSERT_EQ (r->num ("tag"), 7);
}

DN_TEST (api, bad_requests_get_an_error_with_their_tag)
{
    Single s;
    ApiClient c (s.path);

    auto ask = [&] (json::Object o) {
        o.set ("tag", 3);
        c.send (o);
        auto r = c.recv ();
        DN_ASSERT (r.has_value ());
        DN_ASSERT_EQ (r->num ("tag"), 3);
        return r->str ("error");
    };

    json::Object no_api;
    no_api.set ("type", "get");
    DN_ASSERT_EQ (ask (no_api), std::string ("required argument 'api' missing"));

    json::Object other_system;
    other_system.set ("api", "session");
    other_system.set ("system", "ELSEWHERE");
    DN_ASSERT_EQ (ask (other_system), std::string ("Unknown system name"));

    json::Object unsupported;
    unsupported.set ("api", "frob");
    DN_ASSERT_EQ (ask (unsupported), std::string ("Unsupported api"));

    json::Object bad_type;
    bad_type.set ("api", "session");
    bad_type.set ("type", "frob");
    bad_type.set ("handle", 42);
    DN_ASSERT_EQ (ask (bad_type), std::string ("Unknown handle"));
}

DN_TEST (api, a_request_that_is_not_json_is_answered_and_the_link_stays_up)
{
    Single s;
    int fd = ::socket (AF_UNIX, SOCK_STREAM, 0);
    Socket sock (fd);
    sockaddr_un a {};
    a.sun_family = AF_UNIX;
    std::strncpy (a.sun_path, s.path.c_str (), sizeof a.sun_path - 1);
    DN_ASSERT (::connect (fd, reinterpret_cast<sockaddr *> (&a), sizeof a) == 0);

    std::string text = "not json\n{}\n";
    DN_ASSERT (::send (fd, text.data (), text.size (), 0)
               == static_cast<ssize_t> (text.size ()));
    std::string got;
    char buf[1024];
    DN_ASSERT (wait_until ([&] {
        PollResult r = poll_socket (fd, true, false, 100);
        if (r.readable) {
            ssize_t n = ::recv (fd, buf, sizeof buf, 0);
            if (n > 0) got.append (buf, static_cast<std::size_t> (n));
        }
        return std::count (got.begin (), got.end (), '\n') >= 2;
    }));
    DN_ASSERT (got.find ("\"Parse error\"") != std::string::npos);
    DN_ASSERT (got.find ("\"NODEA\"") != std::string::npos);
}

// --------------------------------------------------------------- outbound

DN_TEST (api, loop_through_mirror)
{
    Pair p;
    p.start ();
    ApiClient c (p.bpath);

    json::Object req;
    req.set ("dest", "NODEA");
    req.set ("remuser", 25);
    req.set ("tag", 1);
    auto r = c.session ("connect", req);
    DN_ASSERT (r.has_value ());
    DN_ASSERT_EQ (r->str ("type"), std::string ("connecting"));
    DN_ASSERT_EQ (r->num ("tag"), 1);
    DN_ASSERT_EQ (r->str ("api"), std::string ("session"));
    DN_ASSERT_EQ (r->str ("system"), std::string ("NODEB"));
    std::int64_t h = r->num ("handle");

    // MIRROR accepts with its maximum message size.
    auto acc = c.recv ();
    DN_ASSERT (acc.has_value ());
    DN_ASSERT_EQ (acc->str ("type"), std::string ("accept"));
    DN_ASSERT_EQ (acc->num ("handle"), h);
    DN_ASSERT_EQ (acc->bytes ("data"), (Bytes { 0xff, 0xff }));

    // Every byte value, NUL included, survives both ways.
    Bytes payload { 0x00 };
    for (int i = 0; i < 256; ++i) payload.push_back (static_cast<std::uint8_t> (i));
    json::Object d;
    d.set ("handle", h);
    d.set_bytes ("data", payload);
    d.set ("api", "session");
    d.set ("type", "data");
    c.send (d);
    auto reply = c.recv ();
    DN_ASSERT (reply.has_value ());
    DN_ASSERT_EQ (reply->str ("type"), std::string ("data"));
    Bytes want = payload;
    want[0] = 0x01;
    DN_ASSERT_EQ (reply->bytes ("data"), want);

    json::Object bye;
    bye.set ("handle", h);
    bye.set ("api", "session");
    bye.set ("type", "disconnect");
    c.send (bye);
    // Nothing more arrives for a handle the client has closed.
    DN_ASSERT (!c.recv (500).has_value ());
}

DN_TEST (api, an_unknown_node_is_rejected_at_once)
{
    Single s;
    ApiClient c (s.path);
    json::Object req;
    req.set ("dest", "NOWHERE");
    req.set ("remuser", 25);
    auto r = c.session ("connect", req);
    DN_ASSERT (r.has_value ());
    DN_ASSERT_EQ (r->str ("type"), std::string ("reject"));
    DN_ASSERT_EQ (r->num ("reason"), 2);
}

DN_TEST (api, an_unknown_object_is_rejected_by_the_far_end)
{
    Pair p;
    p.start ();
    ApiClient c (p.bpath);
    json::Object req;
    req.set ("dest", "1.1");
    req.set ("remuser", "NOSUCH");
    auto r = c.session ("connect", req);
    DN_ASSERT (r.has_value ());
    DN_ASSERT_EQ (r->str ("type"), std::string ("connecting"));
    auto rej = c.recv ();
    DN_ASSERT (rej.has_value ());
    DN_ASSERT_EQ (rej->str ("type"), std::string ("reject"));
    DN_ASSERT_EQ (rej->num ("reason"), 4);
    DN_ASSERT_EQ (rej->num ("handle"), r->num ("handle"));
}

// ---------------------------------------------------------------- inbound

DN_TEST (api, a_bound_object_takes_a_conversation)
{
    Pair p;
    p.start ();
    ApiClient server (p.apath), client (p.bpath);

    json::Object b;
    b.set ("name", "pnwtest");
    auto bound = server.session ("bind", b);
    DN_ASSERT (bound.has_value ());
    DN_ASSERT_EQ (bound->str ("type"), std::string ("bind"));
    std::int64_t lh = bound->num ("handle");

    json::Object req;
    req.set ("dest", "NODEA");
    req.set ("remuser", "PNWTEST");
    req.set ("localuser", "PNW");
    req.set ("data", "cdata");
    auto r = client.session ("connect", req);
    DN_ASSERT (r.has_value ());
    std::int64_t ch = r->num ("handle");

    auto in = server.recv ();
    DN_ASSERT (in.has_value ());
    DN_ASSERT_EQ (in->str ("type"), std::string ("connect"));
    DN_ASSERT_EQ (in->num ("listenhandle"), lh);
    DN_ASSERT_EQ (in->bytes ("data"), bytes_of ("cdata"));
    DN_ASSERT_EQ (in->str ("srcuser"), std::string ("PNW"));
    DN_ASSERT_EQ (in->str ("dstuser"), std::string ("PNWTEST"));
    std::int64_t sh = in->num ("handle");

    json::Object acc;
    acc.set ("handle", sh);
    acc.set ("data", "welcome");
    acc.set ("api", "session");
    acc.set ("type", "accept");
    server.send (acc);

    auto a = client.recv ();
    DN_ASSERT (a.has_value ());
    DN_ASSERT_EQ (a->str ("type"), std::string ("accept"));
    DN_ASSERT_EQ (a->bytes ("data"), bytes_of ("welcome"));

    // The client's first data puts the server's link in the run state, and
    // the server hears so before it sees the data.
    json::Object d;
    d.set ("handle", ch);
    d.set ("data", "ping");
    d.set ("api", "session");
    d.set ("type", "data");
    client.send (d);
    auto rs = server.recv ();
    DN_ASSERT (rs.has_value ());
    DN_ASSERT_EQ (rs->str ("type"), std::string ("runstate"));
    auto sd = server.recv ();
    DN_ASSERT (sd.has_value ());
    DN_ASSERT_EQ (sd->bytes ("data"), bytes_of ("ping"));

    // The client disconnects; the server is told, with reason 0.
    json::Object bye;
    bye.set ("handle", ch);
    bye.set ("api", "session");
    bye.set ("type", "disconnect");
    client.send (bye);
    auto gone = server.recv ();
    DN_ASSERT (gone.has_value ());
    DN_ASSERT_EQ (gone->str ("type"), std::string ("disconnect"));
    DN_ASSERT_EQ (gone->num ("reason"), 0);

    // Disconnecting the bind handle withdraws the object.
    json::Object unbind;
    unbind.set ("handle", lh);
    unbind.set ("api", "session");
    unbind.set ("type", "disconnect");
    server.send (unbind);
    DN_ASSERT (wait_until ([&] {
        return p.a->session ()->find_object ("PNWTEST") == nullptr;
    }));
}

DN_TEST (api, an_object_name_can_only_be_bound_once)
{
    Single s;
    ApiClient c (s.path);
    json::Object b;
    b.set ("num", 25);
    auto r = c.session ("bind", b);
    DN_ASSERT (r.has_value ());
    // MIRROR has it.
    DN_ASSERT (!r->str ("error").empty ());
}

DN_TEST (api, a_client_that_goes_away_releases_its_links_and_objects)
{
    Pair p;
    p.start ();
    auto server = std::make_unique<ApiClient> (p.apath);
    ApiClient client (p.bpath);

    json::Object b;
    b.set ("name", "DROPME");
    DN_ASSERT (server->session ("bind", b).has_value ());

    json::Object req;
    req.set ("dest", "NODEA");
    req.set ("remuser", "DROPME");
    auto r = client.session ("connect", req);
    DN_ASSERT (r.has_value ());
    auto in = server->recv ();
    DN_ASSERT (in.has_value ());
    json::Object acc;
    acc.set ("handle", in->num ("handle"));
    acc.set ("api", "session");
    acc.set ("type", "accept");
    server->send (acc);
    auto a = client.recv ();
    DN_ASSERT (a.has_value ());
    DN_ASSERT_EQ (a->str ("type"), std::string ("accept"));

    // The server's client vanishes without a word.
    server.reset ();

    // Its link fails with "object failed", as when an object program dies.
    auto gone = client.recv ();
    DN_ASSERT (gone.has_value ());
    DN_ASSERT_EQ (gone->str ("type"), std::string ("disconnect"));
    DN_ASSERT_EQ (gone->num ("reason"), 38);

    // And its object is free for someone else.
    DN_ASSERT (wait_until ([&] {
        return p.a->session ()->find_object ("DROPME") == nullptr;
    }));
    ApiClient again (p.apath);
    auto rb = again.session ("bind", b);
    DN_ASSERT (rb.has_value ());
    DN_ASSERT_EQ (rb->str ("type"), std::string ("bind"));
}

// ----------------------------------------------------------------- socket

DN_TEST (api, the_socket_is_removed_at_stop_and_a_stale_one_replaced)
{
    std::string path = socket_path ();
    // A file left behind by a server that died.
    {
        int fd = ::socket (AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un a {};
        a.sun_family = AF_UNIX;
        std::strncpy (a.sun_path, path.c_str (), sizeof a.sun_path - 1);
        DN_ASSERT (::bind (fd, reinterpret_cast<sockaddr *> (&a), sizeof a) == 0);
        ::close (fd);
    }
    DN_ASSERT (::access (path.c_str (), F_OK) == 0);

    {
        Single s (path);
        ApiClient c (path);
        DN_ASSERT (c.connected ());

        // A second server on the same path finds the first one answering.
        api::Server other (s.n.get (), path, 0600);
        DN_ASSERT (!other.start ());
    }
    // Stopping the node removed it.
    DN_ASSERT (::access (path.c_str (), F_OK) != 0);
}

DN_TEST (api, a_link_to_this_node_answers_connect_before_accept)
{
    // The whole handshake of a link to ourselves happens inside the connect
    // request.  The client must still see "connecting" first, since
    // PyDECnet's connector treats anything else for a new handle as a
    // sequence error.
    Single s;
    ApiClient c (s.path);
    json::Object req;
    req.set ("dest", "NODEA");
    req.set ("remuser", 25);
    auto r = c.session ("connect", req);
    DN_ASSERT (r.has_value ());
    DN_ASSERT_EQ (r->str ("type"), std::string ("connecting"));
    auto acc = c.recv ();
    DN_ASSERT (acc.has_value ());
    DN_ASSERT_EQ (acc->str ("type"), std::string ("accept"));
    DN_ASSERT_EQ (acc->num ("handle"), r->num ("handle"));

    json::Object d;
    d.set ("handle", r->num ("handle"));
    d.set_bytes ("data", Bytes { 0x00, 'h', 'i' });
    d.set ("api", "session");
    d.set ("type", "data");
    c.send (d);
    auto reply = c.recv ();
    DN_ASSERT (reply.has_value ());
    DN_ASSERT_EQ (reply->bytes ("data"), (Bytes { 0x01, 'h', 'i' }));
}
