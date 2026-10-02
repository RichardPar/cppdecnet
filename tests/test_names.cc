// Node names learned from the network: node @neighbours.

#include "harness.h"

#include "decnet/common/socket.h"
#include "decnet/config.h"
#include "decnet/namelearner.h"
#include "decnet/node.h"
#include "decnet/routing/routing.h"
#include "decnet/session/session.h"

#include <chrono>
#include <thread>

using namespace decnet;
using namespace decnet::session;

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
                     = std::chrono::seconds (30))
{
    auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        if (pred ()) return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (20));
    }
    return pred ();
}

// Talks to MIRROR and says nothing: enough for a link to run.
class Quiet : public Application {
public:
    void connect_received (SessionConnection &, ByteView) override { accepted = true; }
    void data_received (SessionConnection &, ByteView) override {}
    std::atomic<bool> accepted { false };
};

std::string name_of (Node &n, const char *address)
{
    const Nodeinfo *i = n.find_node (Nodeid::parse (address), false);
    return i ? i->name : std::string ();
}

}   // namespace

DN_TEST (names, the_configuration_turns_it_on)
{
    Config off = Config::from_string ("routing 1.1 --type endnode\nnode 1.1 A\n");
    DN_ASSERT (!off.learn_names ());
    Config on = Config::from_string (
        "routing 1.1 --type endnode\nnode 1.1 A\nnode @neighbours\n");
    DN_ASSERT (on.learn_names ());
    DN_ASSERT_EQ (on.learn_refresh (), 3600u);
    Config often = Config::from_string (
        "routing 1.1 --type endnode\nnode @neighbours --refresh 60\n");
    DN_ASSERT_EQ (often.learn_refresh (), 60u);
    DN_ASSERT_THROWS (std::runtime_error, Config::from_string (
        "routing 1.1 --type endnode\nnode @neighbours --refresh 0\n"));
}

DN_TEST (names, a_learned_name_only_fills_a_gap)
{
    Config c = Config::from_string (
        "routing 1.1 --type endnode\nnode 1.1 NODEA\nnode 1.2 NODEB\n"
        "circuit mul-0 Multinet 127.0.0.1:9:connect\n");
    Node n (c);                         // never started
    DN_ASSERT (n.learn_node_name (Nodeid::parse ("1.3"), "nodec"));
    DN_ASSERT_EQ (name_of (n, "1.3"), std::string ("NODEC"));
    // Named already: by the configuration, or by an earlier lesson.
    DN_ASSERT (!n.learn_node_name (Nodeid::parse ("1.2"), "OTHER"));
    DN_ASSERT (!n.learn_node_name (Nodeid::parse ("1.3"), "OTHER"));
    DN_ASSERT_EQ (name_of (n, "1.2"), std::string ("NODEB"));
    // A name that's taken, or this node's own, or not a name at all.
    DN_ASSERT (!n.learn_node_name (Nodeid::parse ("1.4"), "NODEB"));
    DN_ASSERT (!n.learn_node_name (Nodeid::parse ("1.1"), "ME"));
    DN_ASSERT (!n.learn_node_name (Nodeid::parse ("1.5"), "TOOLONGNAME"));
    DN_ASSERT (!n.learn_node_name (Nodeid::parse ("1.5"), "123"));
    DN_ASSERT_EQ (name_of (n, "1.4"), std::string ());
}

DN_TEST (names, a_neighbour_tells_us_the_names_it_knows)
{
    // A knows nobody; its neighbour B knows itself and two others.
    std::uint16_t port = free_port ();
    Config ac = Config::from_string (
        "routing 1.1 --type endnode\nnode 1.1 NODEA\nnode @neighbours\n"
        "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
        + ":listen --t3 2\n");
    Config bc = Config::from_string (
        "routing 1.2 --type endnode\nnode 1.2 NODEB\nnode 1.1 NODEA\n"
        "node 1.3 NODEC\nnode 1.4 NODED\n"
        "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
        + ":connect --t3 2\n");
    Node a (ac), b (bc);
    a.start ();
    b.start ();
    bool learned = wait_until ([&] { return a.name_learner ()->learned () >= 3; });
    b.stop ();
    a.stop ();
    DN_ASSERT (learned);
    DN_ASSERT_EQ (name_of (a, "1.2"), std::string ("NODEB"));
    DN_ASSERT_EQ (name_of (a, "1.3"), std::string ("NODEC"));
    DN_ASSERT_EQ (name_of (a, "1.4"), std::string ("NODED"));
    DN_ASSERT_EQ (name_of (a, "1.1"), std::string ("NODEA"));
}

DN_TEST (names, a_node_we_talk_to_tells_us_its_own_name)
{
    // A - R - C.  Nobody has told A or R what C is called; C knows.  A
    // learns it only by opening a link to C.
    std::uint16_t p1 = free_port (), p2 = free_port ();
    Config ac = Config::from_string (
        "routing 1.1 --type endnode\nnode 1.1 NODEA\nnode @neighbours\n"
        "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (p1)
        + ":connect --t3 2\n");
    Config rc = Config::from_string (
        "routing 1.2 --type l1router\nnode 1.2 ROUTER\n"
        "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (p1)
        + ":listen --t3 2\n"
        "circuit mul-1 Multinet 127.0.0.1:" + std::to_string (p2)
        + ":listen --t3 2\n");
    Config cc = Config::from_string (
        "routing 1.3 --type endnode\nnode 1.3 NODEC\n"
        "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (p2)
        + ":connect --t3 2\n");
    Node a (ac), r (rc), c (cc);
    r.start ();
    a.start ();
    c.start ();
    bool up = wait_until ([&] {
        return a.routing ()->adjacency_count () == 1
            && c.routing ()->adjacency_count () == 1
            && r.routing ()->adjacency_count () == 2;
    });

    auto quiet = std::make_unique<Quiet> ();
    Quiet *q = quiet.get ();
    bool linked = false, learned = false;
    if (up) {
        // Routing needs a moment to know the way to C.
        linked = wait_until ([&] {
            if (q->accepted) return true;
            static auto last = std::chrono::steady_clock::time_point {};
            if (std::chrono::steady_clock::now () - last > std::chrono::seconds (3)) {
                last = std::chrono::steady_clock::now ();
                if (quiet)
                    a.session ()->connect (Nodeid::parse ("1.3"), EndUser::number (25),
                                           EndUser::named ("TEST"), {},
                                           std::move (quiet));
            }
            return false;
        });
        learned = wait_until ([&] { return a.name_learner ()->learned () >= 2; });
    }
    c.stop ();
    a.stop ();
    r.stop ();
    DN_ASSERT (up);
    DN_ASSERT (linked);
    DN_ASSERT (learned);
    DN_ASSERT_EQ (name_of (a, "1.3"), std::string ("NODEC"));
    DN_ASSERT_EQ (name_of (a, "1.2"), std::string ("ROUTER"));
}
