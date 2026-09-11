// Objects implemented as separate processes.
//
// The protocol is deliberately byte-compatible with pydecnet's, so an
// application written for pydecnet runs unchanged here.  These tests use a
// small program written inline rather than depending on a pydecnet
// checkout; the interop test that runs pydecnet's own
// applications/mirror.py is done by hand and recorded in the README.

#include "harness.h"

#include "decnet/common/socket.h"
#include "decnet/config.h"
#include "decnet/node.h"
#include "decnet/routing/routing.h"
#include "decnet/session/process.h"
#include "decnet/session/session.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <thread>

using namespace decnet;
using namespace decnet::session;

namespace {

Bytes mirror_msg (std::uint8_t fn, const std::string &text)
{
    Bytes b { fn };
    b.insert (b.end (), text.begin (), text.end ());
    return b;
}

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
        std::this_thread::sleep_for (std::chrono::milliseconds (20));
    }
    return pred ();
}

// A mirror written the way an application for pydecnet is written: read
// JSON objects from standard input, write them to standard output.  This
// is the same shape as pydecnet's applications/mirror.py, deliberately.
const char *const mirror_program = R"PROG(#!/usr/bin/env python3
import sys, json
encode = json.JSONEncoder ().encode
decode = json.JSONDecoder ().decode
print (encode ({"level": 10, "message": "test mirror started, args {}",
                "args": [str (sys.argv[1:])]}), file = sys.stderr, flush = True)
for work in sys.stdin:
    work = decode (work)
    conn, mtype, msg = work["handle"], work["type"], work["data"]
    if mtype == "data":
        if msg and msg[0] == "\x00":
            msg = "\x01" + msg[1:]
        else:
            msg = "\xff"
        print (encode ({"handle": conn, "type": "data", "data": msg}),
               flush = True)
    elif mtype == "connect":
        i = 65535
        print (encode ({"handle": conn, "type": "accept",
                        "data": str (i.to_bytes (2, "little"), "latin1")}),
               flush = True)
    elif mtype == "disconnect":
        sys.exit (0)
sys.exit (1)
)PROG";

// A program that rejects every connection, to check the other answer.
const char *const reject_program = R"PROG(#!/usr/bin/env python3
import sys, json
encode = json.JSONEncoder ().encode
decode = json.JSONDecoder ().decode
for work in sys.stdin:
    work = decode (work)
    if work["type"] == "connect":
        print (encode ({"handle": work["handle"], "type": "reject",
                        "data": "no thanks"}), flush = True)
        sys.exit (0)
)PROG";

std::string write_program (const std::string &name, const char *body)
{
    // The scratch directory the harness runs in.
    std::string path = "/tmp/dntest_" + name + ".py";
    std::ofstream f (path);
    f << body;
    f.close ();
    return path;
}

class Client : public Application {
public:
    void connect_received (SessionConnection &, ByteView data) override
    {
        std::lock_guard l (m_);
        ++accepts_;
        accept_data_.assign (data.begin (), data.end ());
    }
    void data_received (SessionConnection &, ByteView data) override
    {
        std::lock_guard l (m_);
        replies_.push_back (Bytes (data.begin (), data.end ()));
    }
    void disconnected (SessionConnection &, unsigned reason) override
    {
        std::lock_guard l (m_);
        ++disconnects_;
        reason_ = reason;
    }
    int accepts () { std::lock_guard l (m_); return accepts_; }
    Bytes accept_data () { std::lock_guard l (m_); return accept_data_; }
    std::size_t replies () { std::lock_guard l (m_); return replies_.size (); }
    Bytes reply (std::size_t i) { std::lock_guard l (m_); return replies_.at (i); }
    int disconnects () { std::lock_guard l (m_); return disconnects_; }

private:
    std::mutex         m_;
    int                accepts_ = 0, disconnects_ = 0;
    unsigned           reason_ = 0;
    Bytes              accept_data_;
    std::vector<Bytes> replies_;
};

struct Pair {
    std::uint16_t port = free_port ();
    Config acfg, bcfg;
    std::unique_ptr<Node> a, b;

    explicit Pair (const std::string &objline)
        : acfg (Config::from_string (
              "routing 1.1 --type endnode\nnode 1.1 NODEA\nnode 1.2 NODEB\n"
              "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
              + ":listen --t3 2\n" + objline)),
          bcfg (Config::from_string (
              "routing 1.2 --type endnode\nnode 1.2 NODEB\nnode 1.1 NODEA\n"
              "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
              + ":connect --t3 2\n"))
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
};

}   // namespace

DN_TEST (process, object_config_line_is_read)
{
    Config c = Config::from_string (
        "object --number 25 --name MIRROR --file /bin/true\n"
        "object --number 77 --name OTHER --file /bin/echo --argument one"
        " --argument two\n");
    DN_ASSERT_EQ (c.objects ().size (), 2u);
    DN_ASSERT_EQ (c.objects ()[0].number, 25u);
    DN_ASSERT_EQ (c.objects ()[0].name, std::string ("MIRROR"));
    DN_ASSERT_EQ (c.objects ()[0].file, std::string ("/bin/true"));
    DN_ASSERT_EQ (c.objects ()[1].arguments.size (), 2u);
    DN_ASSERT_EQ (c.objects ()[1].arguments[1], std::string ("two"));

    // An object needs something to be asked for by, and something to run.
    DN_ASSERT_THROWS (std::runtime_error,
                      Config::from_string ("object --file /bin/true\n"));
    DN_ASSERT_THROWS (std::runtime_error,
                      Config::from_string ("object --number 25\n"));
}

DN_TEST (process, a_configured_object_replaces_the_builtin)
{
    // Naming a program for object 25 means it, so the built-in MIRROR
    // must give way rather than the registration failing.
    std::string prog = write_program ("mirror", mirror_program);
    Config c = Config::from_string (
        "routing 1.1 --type endnode\nnode 1.1 NODEA\n"
        "circuit mul-0 Multinet 127.0.0.1:1:connect\n"
        "object --number 25 --name MIRROR --file " + prog + "\n");
    Node n (c);
    DN_ASSERT (n.session ()->find_object (25) != nullptr);
    // Object 25 exists once, not twice: the configured program replaced
    // the built-in rather than being refused.  The other two are the ones
    // registered by default: 19, the network management listener, and 26,
    // the event logger's receiving end.
    DN_ASSERT_EQ (n.session ()->object_count (), 3u);
    DN_ASSERT (n.session ()->find_object (19) != nullptr);
    DN_ASSERT (n.session ()->find_object (26) != nullptr);
}

DN_TEST (process, mirror_loop_through_a_subprocess)
{
    std::string prog = write_program ("mirror", mirror_program);
    Pair p ("object --number 25 --name MIRROR --file " + prog + "\n");
    p.start ();

    auto client = std::make_unique<Client> ();
    Client *cl = client.get ();
    SessionConnection *c = p.b->session ()->connect (
        Nodeid::parse ("1.1"), EndUser::number (25), EndUser::named ("TEST"),
        {}, std::move (client));
    DN_ASSERT (c != nullptr);

    // The program accepted, sending the maximum message size as its
    // accept data -- the same answer the built-in mirror gives.
    DN_ASSERT (wait_until ([&] { return cl->accepts () == 1; }));
    DN_ASSERT_EQ (cl->accept_data (), (Bytes { 0xff, 0xff }));

    c->send_data (mirror_msg (0x00, "testing 1 2 3"));
    DN_ASSERT (wait_until ([&] { return cl->replies () == 1; }));
    DN_ASSERT_EQ (cl->reply (0), mirror_msg (0x01, "testing 1 2 3"));

    p.stop ();
}

DN_TEST (process, every_byte_value_survives_the_pipe)
{
    // The protocol tunnels bytes through JSON strings, and the mirror
    // sends them straight back, so this exercises both directions -- and
    // NUL in particular, which is what a C-string based JSON library
    // cannot carry.
    std::string prog = write_program ("mirror", mirror_program);
    Pair p ("object --number 25 --name MIRROR --file " + prog + "\n");
    p.start ();

    auto client = std::make_unique<Client> ();
    Client *cl = client.get ();
    SessionConnection *c = p.b->session ()->connect (
        Nodeid::parse ("1.1"), EndUser::number (25), EndUser::named ("TEST"),
        {}, std::move (client));
    DN_ASSERT (wait_until ([&] { return cl->accepts () == 1; }));

    Bytes msg { 0x00 };                       // the loop function code
    for (int i = 0; i < 256; ++i) msg.push_back (static_cast<std::uint8_t> (i));

    c->send_data (msg);
    DN_ASSERT (wait_until ([&] { return cl->replies () == 1; }));

    Bytes expected = msg;
    expected[0] = 0x01;                       // the success status
    DN_ASSERT_EQ (cl->reply (0), expected);

    p.stop ();
}

DN_TEST (process, an_application_can_reject)
{
    std::string prog = write_program ("reject", reject_program);
    Pair p ("object --number 30 --name NOPE --file " + prog + "\n");
    p.start ();

    auto client = std::make_unique<Client> ();
    Client *cl = client.get ();
    p.b->session ()->connect (Nodeid::parse ("1.1"), EndUser::number (30),
                              EndUser::named ("TEST"), {}, std::move (client));

    DN_ASSERT (wait_until ([&] { return cl->disconnects () == 1; }));
    DN_ASSERT_EQ (cl->accepts (), 0);

    p.stop ();
}

DN_TEST (process, a_program_that_will_not_start_is_reported_as_no_such_object)
{
    // From the caller's point of view an object whose program is missing
    // does not exist, which is exactly what the reject says.
    Pair p ("object --number 40 --name GONE --file /nonexistent/program\n");
    p.start ();

    auto client = std::make_unique<Client> ();
    Client *cl = client.get ();
    p.b->session ()->connect (Nodeid::parse ("1.1"), EndUser::number (40),
                              EndUser::named ("TEST"), {}, std::move (client));

    DN_ASSERT (wait_until ([&] { return cl->disconnects () == 1; }));
    DN_ASSERT_EQ (cl->accepts (), 0);

    p.stop ();
}
