// Port of tests/test_routing.py and test_route_ptp.py, at the level this
// pass builds: two Phase IV endnodes bring up an adjacency over a real
// Multinet circuit and exchange a data packet.
//
// These run the whole stack -- datalink receive threads, the node work
// queues, the timer wheel and both state machines -- so they are what
// catches the ordering mistakes that unit tests on the packet formats
// cannot.

#include "harness.h"

#include "decnet/common/socket.h"
#include "decnet/config.h"
#include "decnet/node.h"
#include "decnet/routing/ptp.h"
#include "decnet/routing/routing.h"

#include <chrono>
#include <thread>

using namespace decnet;
using namespace decnet::routing;

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

// Poll a condition until it holds or the deadline passes.  Everything here
// happens on other threads, so there is nothing to wait on directly.
template <typename P>
bool wait_until (P pred, std::chrono::milliseconds timeout
                     = std::chrono::seconds (10))
{
    auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        if (pred ()) return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    return pred ();
}

// A pair of endnodes joined by a Multinet circuit on the loopback.
struct NodePair {
    std::uint16_t port = free_port ();
    Config lcfg, ccfg;
    std::unique_ptr<Node> listener, connector;

    explicit NodePair (const std::string &extra = "")
        : lcfg (Config::from_string (
              "routing 1.1 --type endnode\nnode 1.1 LISTEN\nnode 1.2 CONN\n"
              "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
              + ":listen" + extra + "\n")),
          ccfg (Config::from_string (
              "routing 1.2 --type endnode\nnode 1.2 CONN\nnode 1.1 LISTEN\n"
              "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
              + ":connect" + extra + "\n"))
    {
        listener = std::make_unique<Node> (lcfg);
        connector = std::make_unique<Node> (ccfg);
    }

    void start ()
    {
        listener->start ();
        connector->start ();
    }

    void stop ()
    {
        connector->stop ();
        listener->stop ();
    }

    PtpCircuit *lcirc () { return listener->routing ()->circuit ("mul-0"); }
    PtpCircuit *ccirc () { return connector->routing ()->circuit ("mul-0"); }

    bool both_up ()
    {
        return listener->routing ()->adjacency_count () == 1
            && connector->routing ()->adjacency_count () == 1;
    }
};

}   // namespace

DN_TEST (routing, endnode_needs_exactly_one_circuit)
{
    // The architecture allows an endnode one circuit, and the Python enforces
    // it at startup rather than misbehaving later.
    Config none = Config::from_string (
        "routing 1.1 --type endnode\nnode 1.1 SOLO\n");
    DN_ASSERT_THROWS (std::invalid_argument, Node (none));
}

DN_TEST (routing, executor_needs_an_area_and_a_name)
{
    DN_ASSERT_THROWS (std::invalid_argument,
                      Node (Config::from_string (
                          "routing 5 --type endnode\nnode 5 NOAREA\n")));
    // A node with no name entry for its own address cannot identify itself.
    DN_ASSERT_THROWS (std::invalid_argument,
                      Node (Config::from_string (
                          "routing 1.1 --type endnode\nnode 1.2 OTHER\n")));
}

DN_TEST (routing, adjacency_comes_up_between_two_endnodes)
{
    NodePair p;
    p.start ();
    DN_ASSERT (wait_until ([&] { return p.both_up (); }));

    // Each side learned the other's address, type and block size from the
    // init message.
    DN_ASSERT_EQ (p.lcirc ()->neighbour (), Nodeid::parse ("1.2"));
    DN_ASSERT_EQ (p.ccirc ()->neighbour (), Nodeid::parse ("1.1"));
    DN_ASSERT_EQ (p.lcirc ()->neighbour_type (), ENDNODE);
    DN_ASSERT_EQ (p.ccirc ()->neighbour_type (), ENDNODE);
    DN_ASSERT_EQ (p.lcirc ()->neighbour_phase (), 4u);
    DN_ASSERT_EQ (p.ccirc ()->blksize (), 576);
    DN_ASSERT (p.lcirc ()->running ());
    DN_ASSERT (p.ccirc ()->running ());

    // And the adjacency is in the routing layer's table under that address.
    DN_ASSERT (p.listener->routing ()->find_adjacency (Nodeid::parse ("1.2"))
               != nullptr);
    DN_ASSERT (p.connector->routing ()->find_adjacency (Nodeid::parse ("1.1"))
               != nullptr);

    p.stop ();
}

DN_TEST (routing, data_packet_reaches_the_far_endnode)
{
    NodePair p;
    p.start ();
    DN_ASSERT (wait_until ([&] { return p.both_up (); }));

    auto *cr = dynamic_cast<EndnodeRouting *> (p.connector->routing ());
    auto *lr = dynamic_cast<EndnodeRouting *> (p.listener->routing ());
    DN_ASSERT (cr != nullptr && lr != nullptr);
    DN_ASSERT_EQ (lr->packets_for_us (), 0u);

    Bytes payload { 'h', 'e', 'l', 'l', 'o' };
    cr->send (payload, Nodeid::parse ("1.1"));

    DN_ASSERT (wait_until ([&] { return lr->packets_for_us () == 1; }));
    p.stop ();
}

DN_TEST (routing, packet_addressed_to_self_goes_straight_up)
{
    NodePair p;
    p.start ();
    DN_ASSERT (wait_until ([&] { return p.both_up (); }));

    auto *cr = dynamic_cast<EndnodeRouting *> (p.connector->routing ());
    cr->send (Bytes { 'x' }, Nodeid::parse ("1.2"));   // our own address
    DN_ASSERT (wait_until ([&] { return cr->packets_for_us () == 1; }));

    // ... and never reached the wire, so the peer saw nothing.
    auto *lr = dynamic_cast<EndnodeRouting *> (p.listener->routing ());
    DN_ASSERT_EQ (lr->packets_for_us (), 0u);
    p.stop ();
}

DN_TEST (routing, endnode_drops_a_packet_not_addressed_to_it)
{
    NodePair p;
    p.start ();
    DN_ASSERT (wait_until ([&] { return p.both_up (); }));

    // Hand the listener's routing layer a packet for somebody else.  An
    // endnode does not forward, so it must be dropped, not counted.
    auto *lr = dynamic_cast<EndnodeRouting *> (p.listener->routing ());
    ShortData stray;
    stray.dstnode = Nodeid::parse ("1.9");
    stray.srcnode = Nodeid::parse ("1.2");
    lr->forward (stray);
    DN_ASSERT_EQ (lr->packets_for_us (), 0u);

    p.stop ();
}

DN_TEST (routing, an_endnode_neighbour_only_accepts_its_own_traffic)
{
    // A point to point send to an endnode neighbour must be addressed to
    // that neighbour: there is nobody behind it to forward to.
    NodePair p;
    p.start ();
    DN_ASSERT (wait_until ([&] { return p.both_up (); }));

    ShortData ok;
    ok.dstnode = Nodeid::parse ("1.1");
    ok.srcnode = Nodeid::parse ("1.2");
    DN_ASSERT (p.ccirc ()->send (ok));

    ShortData elsewhere;
    elsewhere.dstnode = Nodeid::parse ("1.9");
    elsewhere.srcnode = Nodeid::parse ("1.2");
    DN_ASSERT (!p.ccirc ()->send (elsewhere));

    p.stop ();
}

DN_TEST (routing, circuit_recovers_when_the_peer_disappears)
{
    NodePair p ("");
    p.start ();
    DN_ASSERT (wait_until ([&] { return p.both_up (); }));

    // Drop one end.  The other must notice, take its adjacency down and go
    // back to waiting for the datalink -- not wedge or die.
    p.connector->stop ();
    p.connector.reset ();

    DN_ASSERT (wait_until (
        [&] { return p.listener->routing ()->adjacency_count () == 0; }));
    DN_ASSERT (!p.lcirc ()->running ());

    p.listener->stop ();
}

DN_TEST (routing, hellos_keep_the_adjacency_alive)
{
    // With a three second hello timer, an adjacency that only stays up
    // because hellos are flowing is still up several listen periods later.
    NodePair p (" --t3 1");
    p.start ();
    DN_ASSERT (wait_until ([&] { return p.both_up (); }));

    // The listen timer is t3 * 2.1, so this covers two of them.
    std::this_thread::sleep_for (std::chrono::milliseconds (4500));
    DN_ASSERT (p.both_up ());
    DN_ASSERT (p.lcirc ()->running ());
    DN_ASSERT (p.ccirc ()->running ());

    p.stop ();
}
