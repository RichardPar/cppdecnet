// Level 2 (area) routing: the area table, the attached flag, and out of
// area forwarding.

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

// Two nodes with arbitrary addresses and types, joined by one circuit.
struct Pair {
    std::uint16_t port = free_port ();
    Config acfg, bcfg;
    std::unique_ptr<Node> a, b;

    Pair (const std::string &aid, const std::string &atype,
          const std::string &bid, const std::string &btype)
        : acfg (Config::from_string (
              "routing " + aid + " --type " + atype
              + "\nnode " + aid + " NODEA\nnode " + bid + " NODEB\n"
              "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
              + ":listen --t3 2\n")),
          bcfg (Config::from_string (
              "routing " + bid + " --type " + btype
              + "\nnode " + bid + " NODEB\nnode " + aid + " NODEA\n"
              "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
              + ":connect --t3 2\n"))
    {
        a = std::make_unique<Node> (acfg);
        b = std::make_unique<Node> (bcfg);
    }

    void start () { a->start (); b->start (); }
    void stop () { if (b) b->stop (); if (a) a->stop (); }

    bool both_up ()
    {
        return a->routing ()->adjacency_count () >= 1
            && b->routing ()->adjacency_count () >= 1;
    }

    L2Router *l2a () { return dynamic_cast<L2Router *> (a->routing ()); }
    L2Router *l2b () { return dynamic_cast<L2Router *> (b->routing ()); }
    L1Router *l1a () { return dynamic_cast<L1Router *> (a->routing ()); }
    L1Router *l1b () { return dynamic_cast<L1Router *> (b->routing ()); }
};

}   // namespace

DN_TEST (l2, area_router_starts_and_knows_its_own_area)
{
    Config cfg = Config::from_string (
        "routing 1.1 --type l2router\nnode 1.1 AREA1\n"
        "circuit mul-0 Multinet 127.0.0.1:1:connect\n");
    Node n (cfg);
    auto *r = dynamic_cast<L2Router *> (n.routing ());
    DN_ASSERT (r != nullptr);
    DN_ASSERT_EQ (r->ntype (), L2ROUTER);

    n.start ();
    DN_ASSERT (wait_until ([&] { return r->area_reachable (1); },
                           std::chrono::seconds (2)));
    DN_ASSERT_EQ (r->area_hops (1), 0u);
    DN_ASSERT_EQ (r->area_cost (1), 0u);

    // No other area is reachable, so we are not attached: an area router
    // alone in its area is not a way out of it.
    DN_ASSERT (!r->attached ());
    DN_ASSERT (!r->area_reachable (2));

    // It is still a level 1 router for its own area.
    DN_ASSERT (r->reachable (1));
    DN_ASSERT_EQ (r->hops_to (1), 0u);
    n.stop ();
}

DN_TEST (l2, two_area_routers_learn_each_others_areas)
{
    Pair p ("1.1", "l2router", "2.1", "l2router");
    p.start ();
    DN_ASSERT (wait_until ([&] { return p.both_up (); }));

    // Each learns the other's area from its L2 routing messages.
    DN_ASSERT (wait_until ([&] { return p.l2a ()->area_reachable (2); }));
    DN_ASSERT (wait_until ([&] { return p.l2b ()->area_reachable (1); }));

    DN_ASSERT_EQ (p.l2a ()->area_hops (2), 1u);
    DN_ASSERT_EQ (p.l2a ()->area_cost (2), 4u);
    DN_ASSERT_EQ (p.l2b ()->area_hops (1), 1u);

    p.stop ();
}

DN_TEST (l2, reaching_another_area_makes_a_router_attached)
{
    Pair p ("1.1", "l2router", "2.1", "l2router");
    p.start ();
    DN_ASSERT (wait_until ([&] { return p.both_up (); }));

    DN_ASSERT (wait_until ([&] { return p.l2a ()->attached (); }));
    DN_ASSERT (wait_until ([&] { return p.l2b ()->attached (); }));

    // An attached area router advertises itself to its own area as the
    // nearest level 2 router, which is destination 0 of the level 1 table.
    DN_ASSERT (wait_until ([&] { return p.l2a ()->reachable (0); }));
    DN_ASSERT_EQ (p.l2a ()->advertised_entry (0), route_entry (0, 0));

    p.stop ();
}

DN_TEST (l2, attachment_is_withdrawn_when_the_last_area_link_goes)
{
    Pair p ("1.1", "l2router", "2.1", "l2router");
    p.start ();
    DN_ASSERT (wait_until ([&] { return p.both_up (); }));
    DN_ASSERT (wait_until ([&] { return p.l2a ()->attached (); }));

    p.b->stop ();
    p.b.reset ();

    DN_ASSERT (wait_until ([&] { return !p.l2a ()->attached (); }));
    DN_ASSERT (!p.l2a ()->area_reachable (2));
    // And the claim to be the nearest level 2 router goes with it.
    DN_ASSERT_EQ (p.l2a ()->advertised_entry (0),
                  route_entry (INFHOPS, INFCOST));

    p.a->stop ();
}

DN_TEST (l2, out_of_area_traffic_routes_by_area)
{
    Pair p ("1.1", "l2router", "2.1", "l2router");
    p.start ();
    DN_ASSERT (wait_until ([&] { return p.both_up (); }));
    DN_ASSERT (wait_until ([&] { return p.l2a ()->area_reachable (2); }));

    DN_ASSERT_EQ (p.l2b ()->packets_for_us (), 0u);
    p.l2a ()->send (Bytes { 'x' }, Nodeid::parse ("2.1"));
    DN_ASSERT (wait_until ([&] { return p.l2b ()->packets_for_us () == 1; }));

    p.stop ();
}

DN_TEST (l2, an_unreachable_area_is_dropped)
{
    Pair p ("1.1", "l2router", "2.1", "l2router");
    p.start ();
    DN_ASSERT (wait_until ([&] { return p.l2a ()->attached (); }));

    // Area 9 exists in nobody's table.
    p.l2a ()->send (Bytes { 'x' }, Nodeid::parse ("9.1"));
    DN_ASSERT (wait_until ([&] { return p.l2a ()->unreach_loss () >= 1; },
                           std::chrono::seconds (3)));

    p.stop ();
}

DN_TEST (l2, an_l1_router_sends_out_of_area_traffic_to_the_area_router)
{
    // The level 1 router has no area table at all: it routes anything out
    // of area to destination 0, whoever advertises being nearest.
    Pair p ("1.1", "l2router", "1.2", "l1router");
    p.start ();
    DN_ASSERT (wait_until ([&] { return p.both_up (); }));

    // With no second area anywhere, the area router is not attached, so it
    // does not claim destination 0 and the level 1 router has no way out.
    DN_ASSERT (!p.l2a ()->attached ());
    DN_ASSERT (wait_until ([&] { return !p.l1b ()->reachable (0); },
                           std::chrono::seconds (3)));

    p.stop ();
}

DN_TEST (l2, area_updates_encode_as_l2_routing_messages)
{
    Pair p ("1.1", "l2router", "2.1", "l2router");
    p.start ();
    DN_ASSERT (wait_until ([&] { return p.both_up (); }));
    DN_ASSERT (wait_until ([&] { return p.l2a ()->area_reachable (2); }));

    DN_ASSERT_EQ (p.l2a ()->advertised_area_entry (1), route_entry (0, 0));
    DN_ASSERT_EQ (p.l2a ()->advertised_area_entry (2), route_entry (1, 4));
    DN_ASSERT_EQ (p.l2a ()->advertised_area_entry (30),
                  route_entry (INFHOPS, INFCOST));

    Update *u = p.l2a ()->area_update_for (
        p.a->routing ()->circuits ().front ());
    DN_ASSERT (u != nullptr);
    std::vector<Bytes> pkts = u->build (true);
    DN_ASSERT (!pkts.empty ());
    for (const Bytes &b : pkts) {
        auto msg = RoutingPacketBase::parse_frame (b);
        DN_ASSERT (msg != nullptr);
        DN_ASSERT_EQ (std::string (msg->packet_name ()),
                      std::string ("L2Routing"));
    }
    p.stop ();
}

DN_TEST (l2, area_table_starts_at_area_one)
{
    // There is no area zero, so a level 2 update must never mention it.
    Pair p ("1.1", "l2router", "2.1", "l2router");
    p.start ();
    DN_ASSERT (wait_until ([&] { return p.both_up (); }));

    Update *u = p.l2a ()->area_update_for (
        p.a->routing ()->circuits ().front ());
    std::vector<Bytes> pkts = u->build (true);
    DN_ASSERT (!pkts.empty ());
    auto msg = RoutingPacketBase::parse_frame (pkts.front ());
    auto *l2 = dynamic_cast<L2Routing *> (msg.get ());
    DN_ASSERT (l2 != nullptr);
    DN_ASSERT (!l2->segments.empty ());
    DN_ASSERT_EQ (l2->segments.front ().startid, 1);

    p.stop ();
}

DN_TEST (l2, an_l1_router_ignores_area_routing_messages)
{
    // Level 2 messages carry areas; a level 1 router has nowhere to put
    // them and must not mistake them for node numbers.
    Config cfg = Config::from_string (
        "routing 1.1 --type l1router\nnode 1.1 RTRA\n"
        "circuit mul-0 Multinet 127.0.0.1:1:connect\n");
    Node n (cfg);
    auto *r = dynamic_cast<L1Router *> (n.routing ());
    n.start ();

    L2Routing msg;
    msg.srcnode = 0x0802;
    RouteSegment s;
    s.startid = 1;
    s.entries = { route_entry (0, 0), route_entry (1, 4) };
    msg.segments.push_back (s);

    // No adjacency to attribute it to, and no area table either way.
    r->routing_message (msg, nullptr, 4);
    DN_ASSERT (!r->reachable (2));

    n.stop ();
}
