// Session control: the connect message format, the object database, and a
// MIRROR loop run between two real nodes.

#include "harness.h"

#include "decnet/common/socket.h"
#include "decnet/config.h"
#include "decnet/node.h"
#include "decnet/routing/routing.h"
#include "decnet/session/session.h"

#include <chrono>
#include <mutex>
#include <thread>

using namespace decnet;
using namespace decnet::session;

namespace {

Bytes bytes_of (const std::string &s) { return Bytes (s.begin (), s.end ()); }

// A mirror message: a function code byte followed by text.  Built this way
// because a string literal cannot carry an embedded NUL through the
// std::string constructor -- it would stop there.
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
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    return pred ();
}

}   // namespace

// ------------------------------------------------------ the connect message

DN_TEST (session, end_user_formats)
{
    // Format 0 names an object by number, format 1 by name.
    EndUser n = EndUser::number (25);
    DN_ASSERT (n.valid ());
    DN_ASSERT_EQ (n.str (), std::string ("25"));

    EndUser m = EndUser::named ("MIRROR");
    DN_ASSERT (m.valid ());
    DN_ASSERT_EQ (m.str (), std::string ("MIRROR"));

    // Zero is not an object number, and a nameless format 1 names nothing.
    DN_ASSERT (!EndUser::number (0).valid ());
    DN_ASSERT (!EndUser::named ("").valid ());
}

DN_TEST (session, connect_message_matches_pydecnet)
{
    // The exact bytes pydecnet builds for a connect to object 25 from a
    // named source, which is what its own client sends.
    ConnectData c;
    c.dstname = EndUser::number (25);
    c.srcname = EndUser::named ("TEST");
    Bytes wire = c.encode_message ();

    Bytes expected { 0x00, 0x19,                          // dst: number 25
                     0x01, 0x00, 0x04, 'T', 'E', 'S', 'T', // src: named TEST
                     0x00 };                              // no flags set
    DN_ASSERT_EQ (wire, expected);

    ConnectData q = ConnectData::parse_message (wire);
    DN_ASSERT_EQ (q.dstname, c.dstname);
    DN_ASSERT_EQ (q.srcname, c.srcname);
    DN_ASSERT (!q.auth);
    DN_ASSERT (!q.userdata);
}

DN_TEST (session, connect_message_with_data_and_access_control)
{
    ConnectData c;
    c.dstname     = EndUser::named ("FAL");
    c.srcname     = EndUser::number (1);
    c.rqstrid     = "user";
    c.passwrd     = "secret";
    c.account     = "";
    c.connectdata = bytes_of ("hello");

    Bytes wire = c.encode_message ();
    ConnectData q = ConnectData::parse_message (wire);

    // The flags are derived from what is present, so they cannot disagree
    // with the payload they describe.
    DN_ASSERT (q.auth);
    DN_ASSERT (q.userdata);
    DN_ASSERT_EQ (q.rqstrid, std::string ("user"));
    DN_ASSERT_EQ (q.passwrd, std::string ("secret"));
    DN_ASSERT_EQ (q.account, std::string (""));
    DN_ASSERT_EQ (q.connectdata, bytes_of ("hello"));
    DN_ASSERT_EQ (q.dstname.str (), std::string ("FAL"));
}

DN_TEST (session, malformed_connect_messages_are_rejected)
{
    // A source that names nothing, which pydecnet also refuses.
    DN_ASSERT_THROWS (DecodeError,
                      ConnectData::parse_message (
                          Bytes { 0x00, 0x19, 0x00, 0x00, 0x00 }));
    // Truncated.
    DN_ASSERT_THROWS (DecodeError,
                      ConnectData::parse_message (Bytes { 0x00 }));
    // Flags claim access control data that is not there.
    DN_ASSERT_THROWS (DecodeError,
                      ConnectData::parse_message (
                          Bytes { 0x00, 0x19, 0x00, 0x01, 0x01 }));
    // An end user format that does not exist.
    DN_ASSERT_THROWS (DecodeError,
                      ConnectData::parse_message (
                          Bytes { 0x09, 0x19, 0x00, 0x01, 0x00 }));
}

// ------------------------------------------------------ the object database

DN_TEST (session, object_database)
{
    Config cfg = Config::from_string (
        "routing 1.1 --type endnode\nnode 1.1 NODEA\n"
        "circuit mul-0 Multinet 127.0.0.1:1:connect\n");
    Node n (cfg);
    Session *s = n.session ();
    DN_ASSERT (s != nullptr);

    // MIRROR is registered by default: it is what NCP LOOP NODE talks to.
    DN_ASSERT (s->find_object (25) != nullptr);
    DN_ASSERT_EQ (s->find_object (25)->name, std::string ("MIRROR"));
    DN_ASSERT (s->find_object ("MIRROR") != nullptr);
    // Lookup by name is case insensitive.
    DN_ASSERT (s->find_object ("mirror") != nullptr);
    DN_ASSERT (s->find_object (99) == nullptr);
    DN_ASSERT (s->find_object ("NOSUCH") == nullptr);
}

DN_TEST (session, object_registration_rules)
{
    Config cfg = Config::from_string (
        "routing 1.1 --type endnode\nnode 1.1 NODEA\n"
        "circuit mul-0 Multinet 127.0.0.1:1:connect\n");
    Node n (cfg);
    Session *s = n.session ();

    // An object nothing can ask for is useless.
    DN_ASSERT_THROWS (std::invalid_argument,
                      s->add_object (0, "", [] { return make_mirror (); }));
    // And a duplicate would shadow whatever came first.
    DN_ASSERT_THROWS (std::invalid_argument,
                      s->add_object (25, "OTHER", [] { return make_mirror (); }));
    DN_ASSERT_THROWS (std::invalid_argument,
                      s->add_object (99, "MIRROR", [] { return make_mirror (); }));

    s->add_object (77, "TESTOBJ", [] { return make_mirror (); });
    DN_ASSERT (s->find_object (77) != nullptr);
    DN_ASSERT (s->find_object ("testobj") != nullptr);
}

// ------------------------------------------------------------ the mirror

namespace {

// A client application: records what it is told.
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
    unsigned reason () { std::lock_guard l (m_); return reason_; }

private:
    std::mutex         m_;
    int                accepts_ = 0, disconnects_ = 0;
    unsigned           reason_ = 0;
    Bytes              accept_data_;
    std::vector<Bytes> replies_;
};

// Two endnodes joined by a circuit; each has MIRROR by default.
struct Pair {
    std::uint16_t port = free_port ();
    Config acfg, bcfg;
    std::unique_ptr<Node> a, b;

    Pair ()
        : acfg (Config::from_string (
              "routing 1.1 --type endnode\nnode 1.1 NODEA\nnode 1.2 NODEB\n"
              "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
              + ":listen --t3 2\n")),
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

DN_TEST (session, mirror_loop_by_object_number)
{
    Pair p;
    p.start ();

    auto client = std::make_unique<Client> ();
    Client *cl = client.get ();
    SessionConnection *c = p.b->session ()->connect (
        Nodeid::parse ("1.1"), EndUser::number (25), EndUser::named ("TEST"),
        {}, std::move (client));
    DN_ASSERT (c != nullptr);

    // MIRROR accepts, and its accept data is the largest message it will
    // take, as a two byte little endian value.
    DN_ASSERT (wait_until ([&] { return cl->accepts () == 1; }));
    DN_ASSERT_EQ (cl->accept_data (), (Bytes { 0xff, 0xff }));

    // Function code 0 means "loop this back"; the reply is the success
    // status followed by the same data.
    c->send_data (mirror_msg (0x00, "testing 1 2 3"));
    DN_ASSERT (wait_until ([&] { return cl->replies () == 1; }));
    DN_ASSERT_EQ (cl->reply (0), mirror_msg (0x01, "testing 1 2 3"));

    p.stop ();
}

DN_TEST (session, finished_conversations_are_reclaimed)
{
    // The same rule as NSP's closed connections, and for the same reason:
    // a conversation is retired from inside a callback into the
    // application being retired, so it is moved aside rather than
    // destroyed -- and then it has to be destroyed eventually, or a node
    // that serves many connections grows without bound.  BUGS.md item 6.
    Pair p;
    p.start ();
    p.a->session ()->set_finished_grace (std::chrono::seconds (0));
    p.b->session ()->set_finished_grace (std::chrono::seconds (0));

    for (int i = 0; i < 5; ++i) {
        auto client = std::make_unique<Client> ();
        Client *cl = client.get ();
        SessionConnection *c = p.b->session ()->connect (
            Nodeid::parse ("1.1"), EndUser::number (25),
            EndUser::named ("TEST"), {}, std::move (client));
        DN_ASSERT (c != nullptr);
        DN_ASSERT (wait_until ([&] { return cl->accepts () == 1; }));
        c->disconnect ();
        DN_ASSERT (wait_until ([&] { return cl->disconnects () == 1; }));
    }

    // Five conversations came and went; the graveyard does not hold five.
    DN_ASSERT (wait_until ([&] {
        return p.b->session ()->finished_count () <= 1;
    }));

    p.stop ();
}

DN_TEST (session, mirror_loop_by_object_name)
{
    Pair p;
    p.start ();

    auto client = std::make_unique<Client> ();
    Client *cl = client.get ();
    SessionConnection *c = p.b->session ()->connect (
        Nodeid::parse ("1.1"), EndUser::named ("MIRROR"),
        EndUser::named ("TEST"), {}, std::move (client));
    DN_ASSERT (c != nullptr);
    DN_ASSERT (wait_until ([&] { return cl->accepts () == 1; }));

    c->send_data (mirror_msg (0x00, "abc"));
    DN_ASSERT (wait_until ([&] { return cl->replies () == 1; }));
    DN_ASSERT_EQ (cl->reply (0), mirror_msg (0x01, "abc"));

    p.stop ();
}

DN_TEST (session, mirror_rejects_an_unknown_function)
{
    Pair p;
    p.start ();
    auto client = std::make_unique<Client> ();
    Client *cl = client.get ();
    SessionConnection *c = p.b->session ()->connect (
        Nodeid::parse ("1.1"), EndUser::number (25), EndUser::named ("TEST"),
        {}, std::move (client));
    DN_ASSERT (wait_until ([&] { return cl->accepts () == 1; }));

    // Function code 0 is the only one MIRROR knows; anything else gets the
    // one byte failure status.
    c->send_data (mirror_msg (0x07, "nope"));
    DN_ASSERT (wait_until ([&] { return cl->replies () == 1; }));
    DN_ASSERT_EQ (cl->reply (0), (Bytes { 0xff }));

    p.stop ();
}

DN_TEST (session, connecting_to_an_object_that_does_not_exist_is_rejected)
{
    Pair p;
    p.start ();

    auto client = std::make_unique<Client> ();
    Client *cl = client.get ();
    SessionConnection *c = p.b->session ()->connect (
        Nodeid::parse ("1.1"), EndUser::number (99), EndUser::named ("TEST"),
        {}, std::move (client));
    DN_ASSERT (c != nullptr);

    // Reason 4: the destination end user does not exist.
    DN_ASSERT (wait_until ([&] { return cl->disconnects () == 1; }));
    DN_ASSERT_EQ (cl->reason (), static_cast<unsigned> (NO_OBJ));
    DN_ASSERT_EQ (cl->accepts (), 0);

    p.stop ();
}

DN_TEST (session, an_invalid_end_user_is_refused_before_it_is_sent)
{
    Pair p;
    p.start ();
    // Zero is not an object number, so there is nothing to connect to.
    DN_ASSERT (p.b->session ()->connect (
                   Nodeid::parse ("1.1"), EndUser::number (0),
                   EndUser::named ("TEST"), {},
                   std::make_unique<Client> ()) == nullptr);
    p.stop ();
}

DN_TEST (session, connect_data_reaches_the_object)
{
    // An application that echoes the connect data back as its accept data.
    struct Echo : Application {
        void connect_received (SessionConnection &c, ByteView data) override
        { c.accept (Bytes (data.begin (), data.end ())); }
        void data_received (SessionConnection &, ByteView) override {}
    };

    Pair p;
    p.a->session ()->add_object (60, "ECHO",
                                 [] { return std::make_unique<Echo> (); });
    p.start ();

    auto client = std::make_unique<Client> ();
    Client *cl = client.get ();
    p.b->session ()->connect (Nodeid::parse ("1.1"), EndUser::number (60),
                              EndUser::named ("TEST"), bytes_of ("payload"),
                              std::move (client));

    DN_ASSERT (wait_until ([&] { return cl->accepts () == 1; }));
    DN_ASSERT_EQ (cl->accept_data (), bytes_of ("payload"));

    p.stop ();
}
