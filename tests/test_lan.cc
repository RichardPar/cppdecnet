// The LAN routing sublayer: hellos, two-way confirmation via the router
// list, designated router election, and the endnode's previous hop cache.

#include "harness.h"

#include "decnet/common/socket.h"
#include "decnet/config.h"
#include "decnet/node.h"
#include "decnet/routing/l1router.h"
#include "decnet/routing/lan.h"
#include "decnet/routing/routing.h"

#include <chrono>
#include <thread>

using namespace decnet;
using namespace decnet::routing;

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

template <typename P>
bool wait_until (P pred, std::chrono::milliseconds timeout
                     = std::chrono::seconds (20))
{
    auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        if (pred ()) return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (25));
    }
    return pred ();
}

// Two nodes sharing a UDP-carried LAN.
struct Lan {
    std::uint16_t pa = free_udp_port (), pb = free_udp_port ();
    Config acfg, bcfg;
    std::unique_ptr<Node> a, b;

    Lan (const std::string &aid, const std::string &atype,
         const std::string &bid, const std::string &btype,
         const std::string &aextra = "", const std::string &bextra = "")
        : acfg (Config::from_string (
              "routing " + aid + " --type " + atype
              + "\nnode " + aid + " NODEA\nnode " + bid + " NODEB\n"
              "circuit eth-0 Ethernet udp:" + std::to_string (pa)
              + ":127.0.0.1:" + std::to_string (pb) + " --t3 2" + aextra + "\n")),
          bcfg (Config::from_string (
              "routing " + bid + " --type " + btype
              + "\nnode " + bid + " NODEB\nnode " + aid + " NODEA\n"
              "circuit eth-0 Ethernet udp:" + std::to_string (pb)
              + ":127.0.0.1:" + std::to_string (pa) + " --t3 2" + bextra + "\n"))
    {
        a = std::make_unique<Node> (acfg);
        b = std::make_unique<Node> (bcfg);
    }

    void start () { a->start (); b->start (); }
    void stop () { if (b) b->stop (); if (a) a->stop (); }

    LanCircuit *ca () { return a->routing ()->lan_circuit ("eth-0"); }
    LanCircuit *cb () { return b->routing ()->lan_circuit ("eth-0"); }
    RoutingLanCircuit *ra () { return dynamic_cast<RoutingLanCircuit *> (ca ()); }
    RoutingLanCircuit *rb () { return dynamic_cast<RoutingLanCircuit *> (cb ()); }
    EndnodeLanCircuit *ea () { return dynamic_cast<EndnodeLanCircuit *> (ca ()); }
    EndnodeLanCircuit *eb () { return dynamic_cast<EndnodeLanCircuit *> (cb ()); }
};

}   // namespace

DN_TEST (lan, a_lan_circuit_is_built_for_an_ethernet)
{
    Lan l ("1.1", "l1router", "1.2", "endnode");
    // The router gets a routing LAN circuit, the endnode an endnode one --
    // and neither gets a point to point circuit.
    DN_ASSERT (l.ra () != nullptr);
    DN_ASSERT (l.eb () != nullptr);
    DN_ASSERT (l.a->routing ()->circuits ().empty ());
    DN_ASSERT_EQ (l.a->routing ()->lan_circuits ().size (), 1u);
}

DN_TEST (lan, endnode_finds_the_designated_router)
{
    Lan l ("1.1", "l1router", "1.2", "endnode");
    l.start ();

    // The endnode has heard no router yet.
    DN_ASSERT (!l.eb ()->have_dr ());
    DN_ASSERT (wait_until ([&] { return l.eb ()->have_dr (); }));
    DN_ASSERT_EQ (l.eb ()->dr (), Nodeid::parse ("1.1"));

    // And the router has heard the endnode.
    DN_ASSERT (wait_until ([&] { return l.ra ()->adjacency_count () >= 1; }));

    l.stop ();
}

DN_TEST (lan, routers_confirm_two_way_before_bringing_an_adjacency_up)
{
    // Hearing a router only proves it can reach us.  The adjacency comes
    // up when we see ourselves listed in its hello.
    Lan l ("1.1", "l1router", "1.2", "l1router");
    l.start ();

    DN_ASSERT (wait_until ([&] {
        return l.ra ()->two_way (Nodeid::parse ("1.2"))
            && l.rb ()->two_way (Nodeid::parse ("1.1"));
    }));
    DN_ASSERT_EQ (l.ra ()->adjacency_count (), 1u);
    DN_ASSERT_EQ (l.rb ()->adjacency_count (), 1u);

    l.stop ();
}

DN_TEST (lan, designated_router_is_chosen_by_priority)
{
    // The higher priority wins regardless of address.
    Lan l ("1.1", "l1router", "1.2", "l1router", " --priority 20",
           " --priority 90");
    l.start ();
    DN_ASSERT (wait_until ([&] {
        return l.ra ()->two_way (Nodeid::parse ("1.2"));
    }));

    // Node 1.2 has the higher priority, so both should settle on it --
    // and 1.2 only acts on it after the DR delay.
    DN_ASSERT (wait_until ([&] {
        return l.ra ()->designated_router () == Nodeid::parse ("1.2");
    }));
    DN_ASSERT (wait_until ([&] { return l.rb ()->is_dr (); },
                           std::chrono::seconds (20)));
    DN_ASSERT (!l.ra ()->is_dr ());

    l.stop ();
}

DN_TEST (lan, equal_priority_is_broken_by_address)
{
    // Same priority: the higher address wins, which is what makes every
    // router on the LAN reach the same answer.
    Lan l ("1.1", "l1router", "1.9", "l1router", " --priority 64",
           " --priority 64");
    l.start ();
    DN_ASSERT (wait_until ([&] {
        return l.ra ()->two_way (Nodeid::parse ("1.9"));
    }));
    DN_ASSERT (wait_until ([&] {
        return l.ra ()->designated_router () == Nodeid::parse ("1.9");
    }));
    DN_ASSERT (!l.ra ()->is_dr ());

    l.stop ();
}

DN_TEST (lan, a_lone_router_becomes_designated_router_after_the_delay)
{
    // With nobody else on the LAN, this router is the best candidate --
    // but it waits DRDELAY before acting, so a late arrival can still win.
    std::uint16_t p = free_udp_port (), q = free_udp_port ();
    Config cfg = Config::from_string (
        "routing 1.1 --type l1router\nnode 1.1 NODEA\n"
        "circuit eth-0 Ethernet udp:" + std::to_string (p)
        + ":127.0.0.1:" + std::to_string (q) + " --t3 2\n");
    Node n (cfg);
    auto *c = dynamic_cast<RoutingLanCircuit *> (
        n.routing ()->lan_circuit ("eth-0"));
    DN_ASSERT (c != nullptr);
    n.start ();

    DN_ASSERT (!c->is_dr ());          // not immediately
    DN_ASSERT (wait_until ([&] { return c->is_dr (); },
                           std::chrono::seconds (20)));
    DN_ASSERT_EQ (c->designated_router (), Nodeid::parse ("1.1"));

    n.stop ();
}

DN_TEST (lan, out_of_area_hellos_are_ignored_by_level_1_routers)
{
    Lan l ("1.1", "l1router", "2.1", "l1router");
    l.start ();
    // Give both ends several hello intervals.
    std::this_thread::sleep_for (std::chrono::seconds (5));
    DN_ASSERT_EQ (l.ra ()->adjacency_count (), 0u);
    DN_ASSERT_EQ (l.rb ()->adjacency_count (), 0u);
    l.stop ();
}

DN_TEST (lan, area_routers_hear_each_other_across_areas)
{
    // The one pair allowed to span areas.
    Lan l ("1.1", "l2router", "2.1", "l2router");
    l.start ();
    DN_ASSERT (wait_until ([&] {
        return l.ra ()->two_way (Nodeid::parse ("2.1"));
    }));
    l.stop ();
}

DN_TEST (lan, endnode_data_reaches_the_router)
{
    Lan l ("1.1", "l1router", "1.2", "endnode");
    l.start ();
    DN_ASSERT (wait_until ([&] { return l.eb ()->have_dr (); }));

    auto *er = dynamic_cast<EndnodeRouting *> (l.b->routing ());
    DN_ASSERT (er != nullptr);
    DN_ASSERT_EQ (l.a->routing ()->packets_for_us (), 0u);

    er->send (Bytes { 'h', 'i' }, Nodeid::parse ("1.1"));
    DN_ASSERT (wait_until ([&] {
        return l.a->routing ()->packets_for_us () == 1;
    }));

    l.stop ();
}

DN_TEST (lan, endnode_caches_the_previous_hop)
{
    // A reply should go back the way the traffic came, not via the router.
    Lan l ("1.1", "l1router", "1.2", "endnode");
    l.start ();
    DN_ASSERT (wait_until ([&] { return l.eb ()->have_dr (); }));

    DN_ASSERT_EQ (l.eb ()->cache_size (), 0u);

    // Router to endnode; the endnode should remember who delivered it.
    ShortData pkt;
    pkt.dstnode = Nodeid::parse ("1.2");
    pkt.srcnode = Nodeid::parse ("1.1");
    l.ra ()->send_to_mac (pkt, Macaddr::from_nodeid (Nodeid::parse ("1.2")));

    DN_ASSERT (wait_until ([&] { return l.eb ()->cache_size () >= 1; }));
    l.stop ();
}

DN_TEST (lan, a_lan_adjacency_reaches_the_routing_table)
{
    // The point of the whole exercise: a neighbour discovered over
    // Ethernet has to become a route, not just a log line.
    Lan l ("1.1", "l1router", "1.2", "endnode");
    l.start ();
    DN_ASSERT (wait_until ([&] { return l.eb ()->have_dr (); }));

    auto *rtr = dynamic_cast<L1Router *> (l.a->routing ());
    DN_ASSERT (rtr != nullptr);

    // The router learned the endnode: one hop, at the circuit's cost.
    DN_ASSERT (wait_until ([&] { return rtr->reachable (2); }));
    DN_ASSERT_EQ (rtr->hops_to (2), 1u);
    DN_ASSERT_EQ (rtr->cost_to (2), 4u);
    DN_ASSERT (rtr->next_hop (Nodeid::parse ("1.2")) != nullptr);

    // And the endnode has an adjacency to the router in its own table.
    DN_ASSERT_EQ (l.b->routing ()->adjacency_count (), 1u);

    l.stop ();
}

DN_TEST (lan, two_routers_exchange_routing_messages_over_a_lan)
{
    Lan l ("1.1", "l1router", "1.2", "l1router");
    l.start ();
    DN_ASSERT (wait_until ([&] {
        return l.ra ()->two_way (Nodeid::parse ("1.2"))
            && l.rb ()->two_way (Nodeid::parse ("1.1"));
    }));

    auto *ra = dynamic_cast<L1Router *> (l.a->routing ());
    auto *rb = dynamic_cast<L1Router *> (l.b->routing ());

    // Each router advertises itself; the other should learn a route.
    DN_ASSERT (wait_until ([&] { return ra->reachable (2); }));
    DN_ASSERT (wait_until ([&] { return rb->reachable (1); }));
    DN_ASSERT_EQ (ra->hops_to (2), 1u);
    DN_ASSERT_EQ (rb->hops_to (1), 1u);

    // And data flows between them over the LAN.
    ra->send (Bytes { 'h', 'i' }, Nodeid::parse ("1.2"));
    DN_ASSERT (wait_until ([&] { return rb->packets_for_us () == 1; }));

    l.stop ();
}

DN_TEST (lan, a_neighbour_that_stops_sending_hellos_is_dropped)
{
    Lan l ("1.1", "l1router", "1.2", "endnode");
    l.start ();
    DN_ASSERT (wait_until ([&] { return l.ra ()->adjacency_count () >= 1; }));

    auto *rtr = dynamic_cast<L1Router *> (l.a->routing ());
    DN_ASSERT (wait_until ([&] { return rtr->reachable (2); }));

    // Take the endnode away.  Its listen timer is t3 * BCT3MULT, so the
    // router should notice within a few seconds and withdraw the route.
    l.b->stop ();
    l.b.reset ();

    DN_ASSERT (wait_until ([&] { return !rtr->reachable (2); },
                           std::chrono::seconds (25)));
    DN_ASSERT_EQ (l.ra ()->adjacency_count (), 0u);

    l.a->stop ();
}

DN_TEST (lan, endnode_with_no_router_addresses_the_destination_directly)
{
    // All an endnode can do with no designated router is try the LAN
    // address the destination's node number implies.
    Lan l ("1.1", "endnode", "1.2", "endnode");
    l.start ();

    auto *ea = dynamic_cast<EndnodeRouting *> (l.a->routing ());
    DN_ASSERT (!l.ea ()->have_dr ());
    ea->send (Bytes { 'x' }, Nodeid::parse ("1.2"));

    DN_ASSERT (wait_until ([&] {
        return l.b->routing ()->packets_for_us () == 1;
    }));
    l.stop ();
}
