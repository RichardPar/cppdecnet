// Tests for the API server: the PyDECnet JSON protocol over a Unix socket.

#include "harness.h"
#include "posix_compat.h"

#include "decnet/api/server.h"
#include "decnet/common/json.h"
#include "decnet/common/socket.h"
#include "decnet/config.h"
#include "decnet/mop/mop.h"
#include "decnet/node.h"
#include "decnet/routing/routing.h"
#include "decnet/session/session.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>
#include <thread>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#endif

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
    return dntest::tmp_dir () + "/dnapi-" + std::to_string (::getpid ()) + "-"
        + std::to_string (++n) + ".sock";
}

Bytes bytes_of (const std::string &s) { return Bytes (s.begin (), s.end ()); }

// A client of the API, speaking JSON lines as connectors.py does.
class ApiClient {
public:
    explicit ApiClient (const std::string &path)
    {
        sock_ = Socket (sock_open (AF_UNIX, SOCK_STREAM));
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
        (void) sock_send (sock_.fd (), text.data (), text.size ());
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
            ssize_t n = sock_recv (sock_.fd (), buf, sizeof buf);
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
#ifdef _WIN32
    DN_ASSERT_EQ (d.api_socket (), dntest::tmp_dir () + "/decnetapi.sock");
#else
    DN_ASSERT_EQ (d.api_socket (), std::string ("/tmp/decnetapi.sock"));
#endif
    DN_ASSERT_EQ (d.api_mode (), 0666u);

    DN_ASSERT_THROWS (std::runtime_error,
                      Config::from_string ("api --mode 999\n"));
    DN_ASSERT (Config::from_string ("routing 1.1\n").api_socket ().empty ());

    // On demand: off unless asked for, and two hours idle unless told.
    DN_ASSERT (!d.api_on_demand ());
    Config e = Config::from_string ("api /tmp/x.sock --on-demand\n");
    DN_ASSERT (e.api_on_demand ());
    DN_ASSERT_EQ (e.api_idle (), 7200u);
    Config f = Config::from_string ("api /tmp/x.sock --on-demand --idle 60\n");
    DN_ASSERT_EQ (f.api_idle (), 60u);
    DN_ASSERT_THROWS (std::runtime_error,
                      Config::from_string ("api --on-demand --idle 0\n"));
}

// --------------------------------------------------------------- on demand

DN_TEST (api, on_demand_circuits_follow_the_api_clients)
{
    // A is an ordinary router; B brings its circuit to A up only while an
    // API client is connected, and takes it down two seconds after.
    std::uint16_t port = free_port ();
    std::string bpath = socket_path ();
    Config acfg = Config::from_string (
        "routing 1.1 --type l1router\nnode 1.1 NODEA\nnode 1.2 NODEB\n"
        "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
        + ":listen --t3 2\n");
    Config bcfg = Config::from_string (
        "routing 1.2 --type endnode\nnode 1.2 NODEB\nnode 1.1 NODEA\n"
        "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
        + ":connect --t3 2\napi " + bpath + " --on-demand --idle 2\n");
    Node a (acfg), b (bcfg);
    a.start ();
    b.start ();

    // Nobody has asked: no circuit, however long we wait.
    std::this_thread::sleep_for (std::chrono::seconds (3));
    DN_ASSERT_EQ (a.routing ()->adjacency_count (), 0u);

    {
        ApiClient c (bpath);
        DN_ASSERT (c.connected ());
        DN_ASSERT (wait_until ([&] { return a.routing ()->adjacency_count () == 1; }));
        // Up while the client stays, past the idle time.
        std::this_thread::sleep_for (std::chrono::seconds (3));
        DN_ASSERT_EQ (a.routing ()->adjacency_count (), 1u);
    }
    // The client has gone: down after the idle time.
    DN_ASSERT (wait_until ([&] { return a.routing ()->adjacency_count () == 0; }));

    // And up again for the next one.
    {
        ApiClient c (bpath);
        DN_ASSERT (wait_until ([&] { return a.routing ()->adjacency_count () == 1; }));
    }
    b.stop ();
    a.stop ();
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
    int fd = sock_open (AF_UNIX, SOCK_STREAM);
    Socket sock (fd);
    sockaddr_un a {};
    a.sun_family = AF_UNIX;
    std::strncpy (a.sun_path, s.path.c_str (), sizeof a.sun_path - 1);
    DN_ASSERT (::connect (fd, reinterpret_cast<sockaddr *> (&a), sizeof a) == 0);

    std::string text = "not json\n{}\n";
    DN_ASSERT (sock_send (fd, text.data (), text.size ())
               == static_cast<ssize_t> (text.size ()));
    std::string got;
    char buf[1024];
    DN_ASSERT (wait_until ([&] {
        PollResult r = poll_socket (fd, true, false, 100);
        if (r.readable) {
            ssize_t n = sock_recv (fd, buf, sizeof buf);
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
        int fd = sock_open (AF_UNIX, SOCK_STREAM);
        sockaddr_un a {};
        a.sun_family = AF_UNIX;
        std::strncpy (a.sun_path, path.c_str (), sizeof a.sun_path - 1);
        DN_ASSERT (::bind (fd, reinterpret_cast<sockaddr *> (&a), sizeof a) == 0);
        sock_close (fd);
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

// --------------------------------------------------------------------- MOP

namespace {

std::uint16_t free_udp_port ()
{
    SourceAddress any ("127.0.0.1", 0);
    Socket s = any.bind_socket (AF_INET, SOCK_DGRAM);
    if (!s) throw std::runtime_error ("cannot find a free port");
    sockaddr_storage sa {};
    socklen_t len = sizeof sa;
    ::getsockname (s.fd (), reinterpret_cast<sockaddr *> (&sa), &len);
    return ntohs (reinterpret_cast<sockaddr_in *> (&sa)->sin_port);
}

// Two stations on a LAN carried over UDP, both running MOP; the API is on
// A.
struct MopLan {
    std::uint16_t pa = free_udp_port (), pb = free_udp_port ();
    std::string path = socket_path ();
    Config acfg, bcfg;
    std::unique_ptr<Node> a, b;

    MopLan ()
        : acfg (Config::from_string (
              "node 1.1 NODEA\nnode 1.2 NODEB\ncircuit eth-0 Ethernet udp:"
              + std::to_string (pa) + ":127.0.0.1:" + std::to_string (pb)
              + " --random-address --mop\napi " + path + "\n")),
          bcfg (Config::from_string (
              "node 1.2 NODEB\ncircuit eth-0 Ethernet udp:"
              + std::to_string (pb) + ":127.0.0.1:" + std::to_string (pa)
              + " --random-address --mop\n"))
    {
        a = std::make_unique<Node> (acfg);
        b = std::make_unique<Node> (bcfg);
        a->start ();
        b->start ();
    }
    ~MopLan () { b->stop (); a->stop (); }

    std::string addr_b ()
    { return b->mop ()->circuit ("eth-0")->datalink ()->hwaddr ().str (); }
};

json::Object mop_req (const std::string &type, int tag)
{
    json::Object o;
    o.set ("api", "mop");
    o.set ("type", type);
    o.set ("tag", tag);
    return o;
}

}   // namespace

DN_TEST (api, mop_is_listed_where_there_is_a_mop_circuit)
{
    MopLan l;
    ApiClient c (l.path);
    c.send (json::Object ());
    auto r = c.recv ();
    DN_ASSERT (r.has_value ());
    // A node that does not route has no name, and no session control: only
    // MOP.
    DN_ASSERT_EQ (r->size (), 1u);
    DN_ASSERT_EQ (r->get (r->keys ().front ())->encode (),
                  std::string ("[\"mop\"]"));

    c.send (mop_req ("get", 1));
    r = c.recv ();
    DN_ASSERT (r.has_value ());
    const auto &circuits = r->get ("circuits")->as_array ();
    DN_ASSERT_EQ (circuits.size (), 1u);
    DN_ASSERT (!circuits[0].as_object ().str ("hwaddr").empty ());
    DN_ASSERT_EQ (r->str ("api"), std::string ("mop"));
    DN_ASSERT_EQ (r->num ("tag"), 1);
}

DN_TEST (api, mop_asks_a_station_who_it_is_and_remembers)
{
    MopLan l;
    ApiClient c (l.path);
    json::Object q = mop_req ("sysid", 2);
    q.set ("dest", l.addr_b ());
    c.send (q);
    auto r = c.recv ();
    DN_ASSERT (r.has_value ());
    DN_ASSERT_EQ (r->str ("status"), std::string ("ok"));
    DN_ASSERT_EQ (r->num ("tag"), 2);
    const json::Object &s = r->get ("sysid")->as_array ().at (0).as_object ();
    DN_ASSERT_EQ (s.str ("srcaddr"), l.addr_b ());
    DN_ASSERT_EQ (s.str ("software"), std::string ("DECnet/C++"));
    DN_ASSERT_EQ (s.str ("processor"), std::string ("Communication Server"));
    DN_ASSERT_EQ (s.get ("services")->encode (),
                  std::string ("[\"loop\",\"counters\"]"));

    // Now in the list of stations heard.
    c.send (mop_req ("sysid", 3));
    r = c.recv ();
    DN_ASSERT (r.has_value ());
    const auto &heard = r->get ("sysid")->as_array ();
    DN_ASSERT_EQ (heard.size (), 1u);
    DN_ASSERT_EQ (heard[0].as_object ().str ("srcaddr"), l.addr_b ());
    DN_ASSERT (heard[0].as_object ().has ("age"));
}

DN_TEST (api, mop_counters_and_loop)
{
    MopLan l;
    ApiClient c (l.path);
    json::Object q = mop_req ("counters", 4);
    q.set ("dest", l.addr_b ());
    c.send (q);
    auto r = c.recv ();
    DN_ASSERT (r.has_value ());
    DN_ASSERT_EQ (r->str ("status"), std::string ("ok"));
    DN_ASSERT (r->num ("pkts_recv") >= 1);

    // Two messages, without the second's wait.
    q = mop_req ("loop", 5);
    q.set ("dest", l.addr_b ());
    q.set ("packets", 2);
    q.set ("fast", true);
    c.send (q);
    r = c.recv ();
    DN_ASSERT (r.has_value ());
    DN_ASSERT_EQ (r->str ("status"), std::string ("ok"));
    DN_ASSERT_EQ (r->str ("dest"), l.addr_b ());
    const auto &d = r->get ("delays")->as_array ();
    DN_ASSERT_EQ (d.size (), 2u);
    DN_ASSERT (d[0].as_double () >= 0 && d[1].as_double () >= 0);

    // No destination: the loopback multicast, which B answers.
    c.send (mop_req ("loop", 6));
    r = c.recv ();
    DN_ASSERT (r.has_value ());
    DN_ASSERT_EQ (r->str ("dest"), l.addr_b ());

    // Nobody there: the time out is a -1.
    q = mop_req ("loop", 7);
    q.set ("dest", "aa-00-04-00-99-99");
    q.set ("timeout", 1);
    c.send (q);
    r = c.recv ();
    DN_ASSERT (r.has_value ());
    DN_ASSERT_EQ (r->get ("delays")->encode (), std::string ("[-1]"));
}

DN_TEST (api, mop_names_stand_for_their_decnet_address)
{
    MopLan l;
    ApiClient c (l.path);
    // NODEB is 1.2, so AA-00-04-00-02-04.  B uses a random address, so
    // nothing answers, but the request goes to the right place.
    json::Object q = mop_req ("loop", 8);
    q.set ("dest", "NODEB");
    q.set ("timeout", 1);
    c.send (q);
    auto r = c.recv ();
    DN_ASSERT (r.has_value ());
    DN_ASSERT_EQ (r->str ("dest"), std::string ("aa-00-04-00-02-04"));
    DN_ASSERT_EQ (r->get ("delays")->encode (), std::string ("[-1]"));
}

DN_TEST (api, mop_bad_requests)
{
    MopLan l;
    ApiClient c (l.path);
    json::Object q = mop_req ("counters", 9);
    q.set ("dest", "not-a-station");
    c.send (q);
    auto r = c.recv ();
    DN_ASSERT (r.has_value ());
    DN_ASSERT_EQ (r->str ("error"), std::string ("invalid dest"));
    DN_ASSERT_EQ (r->num ("tag"), 9);

    q = mop_req ("loop", 10);
    q.set ("timeout", 0);
    c.send (q);
    r = c.recv ();
    DN_ASSERT_EQ (r->str ("error"), std::string ("invalid timeout"));

    q = mop_req ("loop", 11);
    q.set ("circuit", "eth-9");
    c.send (q);
    r = c.recv ();
    DN_ASSERT_EQ (r->str ("error"), std::string ("invalid circuit argument"));

    c.send (mop_req ("dump", 12));
    r = c.recv ();
    DN_ASSERT_EQ (r->str ("error"), std::string ("Unsupported operation"));
}
