// Level 1 routing: the routing table, the route computation, forwarding,
// and the update process that tells neighbours what we can reach.

#include "harness.h"

#include "decnet/common/socket.h"
#include "decnet/config.h"
#include "decnet/node.h"
#include "decnet/routing/l1router.h"
#include "decnet/routing/ptp.h"

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

// Two nodes joined by a Multinet circuit, with configurable types.
struct Pair {
    std::uint16_t port = free_port ();
    Config lcfg, ccfg;
    std::unique_ptr<Node> a, b;

    Pair (const std::string &atype, const std::string &btype,
          const std::string &extra = "")
        : lcfg (Config::from_string (
              "routing 1.1 --type " + atype + "\nnode 1.1 RTRA\nnode 1.2 RTRB\n"
              "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
              + ":listen" + extra + "\n")),
          ccfg (Config::from_string (
              "routing 1.2 --type " + btype + "\nnode 1.2 RTRB\nnode 1.1 RTRA\n"
              "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
              + ":connect" + extra + "\n"))
    {
        a = std::make_unique<Node> (lcfg);
        b = std::make_unique<Node> (ccfg);
    }

    void start () { a->start (); b->start (); }
    void stop () { b->stop (); a->stop (); }

    bool both_up ()
    {
        return a->routing ()->adjacency_count () >= 1
            && b->routing ()->adjacency_count () >= 1;
    }

    L1Router *ra () { return dynamic_cast<L1Router *> (a->routing ()); }
    L1Router *rb () { return dynamic_cast<L1Router *> (b->routing ()); }
};

}   // namespace

DN_TEST (l1, router_starts_and_knows_itself)
{
    Config cfg = Config::from_string (
        "routing 1.1 --type l1router\nnode 1.1 RTRA\n"
        "circuit mul-0 Multinet 127.0.0.1:1:connect\n");
    Node n (cfg);
    auto *r = dynamic_cast<L1Router *> (n.routing ());
    DN_ASSERT (r != nullptr);
    DN_ASSERT_EQ (r->ntype (), L1ROUTER);

    n.start ();
    // Our own address is reachable at zero cost through the self column.
    DN_ASSERT (wait_until ([&] { return r->reachable (1); },
                           std::chrono::seconds (2)));
    DN_ASSERT_EQ (r->hops_to (1), 0u);
    DN_ASSERT_EQ (r->cost_to (1), 0u);
    // Nothing else is.
    DN_ASSERT (!r->reachable (2));
    DN_ASSERT_EQ (r->hops_to (2), INFHOPS);
    n.stop ();
}

DN_TEST (l1, unlike_an_endnode_a_router_may_have_several_circuits)
{
    // The endnode check must not apply here.
    std::uint16_t p1 = free_port (), p2 = free_port ();
    Config cfg = Config::from_string (
        "routing 1.1 --type l1router\nnode 1.1 RTRA\n"
        "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (p1) + ":listen\n"
        "circuit mul-1 Multinet 127.0.0.1:" + std::to_string (p2) + ":listen\n");
    Node n (cfg);
    DN_ASSERT_EQ (n.routing ()->circuits ().size (), 2u);
}

DN_TEST (l1, two_routers_learn_a_route_to_each_other)
{
    Pair p ("l1router", "l1router", " --t3 2");
    p.start ();
    DN_ASSERT (wait_until ([&] { return p.both_up (); }));

    // Each router advertises itself; the other should end up with a route
    // to it at one hop, at the cost of the circuit between them.
    DN_ASSERT (wait_until ([&] { return p.ra ()->reachable (2); }));
    DN_ASSERT (wait_until ([&] { return p.rb ()->reachable (1); }));

    DN_ASSERT_EQ (p.ra ()->hops_to (2), 1u);
    DN_ASSERT_EQ (p.rb ()->hops_to (1), 1u);
    // The default circuit cost is 4.
    DN_ASSERT_EQ (p.ra ()->cost_to (2), 4u);
    DN_ASSERT_EQ (p.rb ()->cost_to (1), 4u);

    // And the next hop for that destination is the neighbour itself.
    Adjacency *nh = p.ra ()->next_hop (Nodeid::parse ("1.2"));
    DN_ASSERT (nh != nullptr);
    DN_ASSERT_EQ (nh->nodeid (), Nodeid::parse ("1.2"));

    p.stop ();
}

DN_TEST (l1, circuit_cost_is_charged_to_the_route)
{
    Pair p ("l1router", "l1router", " --cost 9 --t3 2");
    p.start ();
    DN_ASSERT (wait_until ([&] { return p.both_up (); }));
    DN_ASSERT (wait_until ([&] { return p.ra ()->reachable (2); }));
    DN_ASSERT_EQ (p.ra ()->cost_to (2), 9u);
    p.stop ();
}

DN_TEST (l1, data_flows_between_two_routers)
{
    Pair p ("l1router", "l1router", " --t3 2");
    p.start ();
    DN_ASSERT (wait_until ([&] { return p.both_up (); }));
    DN_ASSERT (wait_until ([&] { return p.ra ()->reachable (2); }));

    DN_ASSERT_EQ (p.rb ()->packets_for_us (), 0u);
    p.ra ()->send (Bytes { 'h', 'i' }, Nodeid::parse ("1.2"));
    DN_ASSERT (wait_until ([&] { return p.rb ()->packets_for_us () == 1; }));

    p.stop ();
}

DN_TEST (l1, router_learns_an_endnode_neighbour)
{
    // An endnode does not advertise, so the router fills the entry in
    // itself: one hop, at the circuit's cost.
    Pair p ("l1router", "endnode", " --t3 2");
    p.start ();
    DN_ASSERT (wait_until ([&] { return p.both_up (); }));

    DN_ASSERT (wait_until ([&] { return p.ra ()->reachable (2); }));
    DN_ASSERT_EQ (p.ra ()->hops_to (2), 1u);
    DN_ASSERT_EQ (p.ra ()->cost_to (2), 4u);

    // And traffic reaches it.
    p.ra ()->send (Bytes { 'x' }, Nodeid::parse ("1.2"));
    DN_ASSERT (wait_until ([&] { return p.b->routing ()->packets_for_us () == 1; }));

    p.stop ();
}

DN_TEST (l1, route_is_withdrawn_when_the_neighbour_goes_away)
{
    Pair p ("l1router", "l1router", " --t3 1");
    p.start ();
    DN_ASSERT (wait_until ([&] { return p.both_up (); }));
    DN_ASSERT (wait_until ([&] { return p.ra ()->reachable (2); }));

    p.b->stop ();
    p.b.reset ();

    // The adjacency goes down and the route with it.
    DN_ASSERT (wait_until ([&] { return !p.ra ()->reachable (2); }));
    DN_ASSERT_EQ (p.ra ()->hops_to (2), INFHOPS);
    DN_ASSERT (p.ra ()->next_hop (Nodeid::parse ("1.2")) == nullptr);

    p.a->stop ();
}

DN_TEST (l1, unreachable_destination_is_counted_and_dropped)
{
    Config cfg = Config::from_string (
        "routing 1.1 --type l1router\nnode 1.1 RTRA\n"
        "circuit mul-0 Multinet 127.0.0.1:1:connect\n");
    Node n (cfg);
    auto *r = dynamic_cast<L1Router *> (n.routing ());
    n.start ();

    ShortData pkt;
    pkt.dstnode = Nodeid::parse ("1.99");
    pkt.srcnode = Nodeid::parse ("1.1");
    r->forward (pkt);
    DN_ASSERT_EQ (r->unreach_loss (), 1u);
    DN_ASSERT_EQ (r->packets_for_us (), 0u);

    n.stop ();
}

DN_TEST (l1, visit_limit_ages_a_looping_packet)
{
    Pair p ("l1router", "l1router", " --t3 2");
    p.start ();
    DN_ASSERT (wait_until ([&] { return p.both_up (); }));
    DN_ASSERT (wait_until ([&] { return p.ra ()->reachable (2); }));

    // A packet that has already been round too many times is dropped
    // rather than forwarded again.
    ShortData old;
    old.dstnode = Nodeid::parse ("1.2");
    old.srcnode = Nodeid::parse ("1.9");
    old.visit   = static_cast<std::uint8_t> (p.ra ()->maxvisits ());
    p.ra ()->forward (old);
    DN_ASSERT_EQ (p.ra ()->aged_loss (), 1u);

    // One below the limit still goes.
    ShortData ok;
    ok.dstnode = Nodeid::parse ("1.2");
    ok.srcnode = Nodeid::parse ("1.9");
    ok.visit   = static_cast<std::uint8_t> (p.ra ()->maxvisits () - 1);
    p.ra ()->forward (ok);
    DN_ASSERT (wait_until ([&] { return p.rb ()->packets_for_us () >= 1; }));

    p.stop ();
}

DN_TEST (l1, return_to_sender_turns_an_undeliverable_packet_round)
{
    Config cfg = Config::from_string (
        "routing 1.1 --type l1router\nnode 1.1 RTRA\n"
        "circuit mul-0 Multinet 127.0.0.1:1:connect\n");
    Node n (cfg);
    auto *r = dynamic_cast<L1Router *> (n.routing ());
    n.start ();

    // Unreachable destination, but the sender asked for it back -- and the
    // sender is us, so it comes straight back up.
    ShortData pkt;
    pkt.dstnode = Nodeid::parse ("1.99");
    pkt.srcnode = Nodeid::parse ("1.1");
    pkt.rqr     = true;
    r->forward (pkt);

    DN_ASSERT_EQ (r->packets_for_us (), 1u);
    DN_ASSERT_EQ (r->unreach_loss (), 0u);

    n.stop ();
}

DN_TEST (l1, update_messages_describe_what_we_can_reach)
{
    Pair p ("l1router", "l1router", " --t3 2");
    p.start ();
    DN_ASSERT (wait_until ([&] { return p.both_up (); }));
    DN_ASSERT (wait_until ([&] { return p.ra ()->reachable (2); }));

    // What we advertise for ourselves is zero hops at zero cost, and for
    // the neighbour, one hop at the circuit cost.
    DN_ASSERT_EQ (p.ra ()->advertised_entry (1), route_entry (0, 0));
    DN_ASSERT_EQ (p.ra ()->advertised_entry (2), route_entry (1, 4));
    // An address we know nothing about is advertised as unreachable.
    DN_ASSERT_EQ (p.ra ()->advertised_entry (500),
                  route_entry (INFHOPS, INFCOST));

    // And a full update encodes to a decodable L1 routing message.
    Update *u = p.ra ()->update_for (p.a->routing ()->circuits ().front ());
    DN_ASSERT (u != nullptr);
    std::vector<Bytes> pkts = u->build (true);
    DN_ASSERT (!pkts.empty ());
    for (const Bytes &b : pkts) {
        auto msg = RoutingPacketBase::parse_frame (b);
        DN_ASSERT (msg != nullptr);
        DN_ASSERT_EQ (std::string (msg->packet_name ()),
                      std::string ("L1Routing"));
    }

    p.stop ();
}

DN_TEST (l1, updates_are_split_to_fit_the_block_size)
{
    // A full table for 1023 nodes does not fit one 576 byte message, so
    // the update process must produce several.
    Pair p ("l1router", "l1router", " --t3 2");
    p.start ();
    DN_ASSERT (wait_until ([&] { return p.both_up (); }));

    Update *u = p.ra ()->update_for (p.a->routing ()->circuits ().front ());
    std::vector<Bytes> pkts = u->build (true);
    DN_ASSERT (pkts.size () > 1);
    for (const Bytes &b : pkts) {
        DN_ASSERT (b.size () <= 576u);
        DN_ASSERT (RoutingPacketBase::parse_frame (b) != nullptr);
    }

    p.stop ();
}
