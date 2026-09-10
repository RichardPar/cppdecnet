// decnet/routing/l1router.h -- level 1 routing.
//
// Port of routing.L1Router and routing.Update.  This is the part that makes
// a node a router rather than a leaf: it keeps a routing table, recomputes
// it when an adjacency or a neighbour's advertisement changes, forwards
// packets towards their destination, and tells its neighbours what it can
// reach.
//
// The table is the matrix the routing spec describes.  One column per
// adjacency records what that neighbour says it can reach, plus one column
// for this node itself and one shared column for all endnode adjacencies.
// The route computation picks, for each destination, the column offering
// the lowest cost -- and that column's adjacency becomes the output for
// that destination.
//
// L2Router, below, builds a second matrix indexed by area rather than node
// and reuses the same route computation over it.

#ifndef DECNET_ROUTING_L1ROUTER_H
#define DECNET_ROUTING_L1ROUTER_H

#include "decnet/common/timers.h"
#include "decnet/events/events.h"
#include "decnet/routing/routing.h"

#include <functional>
#include <vector>

namespace decnet::routing {

class PtpCircuit;
class LanCircuit;
class L1Router;

// One column of the routing matrix: what one neighbour says it can reach.
// Port of routing.RouteInfo.
struct RouteInfo {
    std::vector<std::uint8_t>  hops;
    std::vector<std::uint16_t> cost;
    Adjacency                 *adj = nullptr;   // whose column this is
    Nodeid                     nodeid;

    explicit RouteInfo (unsigned maxnodes)
        : hops (maxnodes + 1, static_cast<std::uint8_t> (INFHOPS)),
          cost (maxnodes + 1, static_cast<std::uint16_t> (INFCOST)) {}

    // The adjacency to use for destination i.  For a router's column that
    // is always the neighbour itself; the endnode column overrides this.
    virtual Adjacency *adjacency (unsigned) const noexcept { return adj; }
    virtual ~RouteInfo () = default;
};

// The shared column for endnode adjacencies.  Endnodes do not advertise, so
// this node fills the column in itself: one hop, the circuit's cost, and a
// different adjacency per destination.  Port of EndnodesRouteInfo.
struct EndnodesRouteInfo : RouteInfo {
    std::vector<Adjacency *> adjacencies;

    explicit EndnodesRouteInfo (unsigned maxnodes)
        : RouteInfo (maxnodes), adjacencies (maxnodes + 1, nullptr) {}

    Adjacency *adjacency (unsigned i) const noexcept override
    { return i < adjacencies.size () ? adjacencies[i] : nullptr; }
};

// The per-circuit update process: decides when to send routing messages and
// what to put in them.  Port of routing.Update.
class Update : public Element, public Timer {
public:
    // level is 1 or 2: which routing table this process advertises, and so
    // which message type it builds.  pydecnet keeps one Update per circuit
    // per type for the same reason.
    Update (Circuit *circuit, L1Router *router, double t1,
            unsigned level = 1);

    // Mark destinations as needing to be advertised, and schedule a send.
    // Port of Update.setsrm.
    void set_srm (unsigned id);
    void set_srm (unsigned first, unsigned last);

    void start ();
    void stop ();

    void dispatch (Work &) override {}
    void timeout () override;
    Element *timer_owner () noexcept override { return this; }

    // Build the messages a send would produce, without sending them.
    // Exposed because it is the part worth testing directly.
    std::vector<Bytes> build (bool complete) const;

private:
    void send_now ();

    Circuit         *circuit_;
    L1Router        *router_;
    double           t1_;
    unsigned         level_;
    std::vector<bool> srm_;
    bool             any_srm_ = false;
    bool             holdoff_ = false;
    bool             running_ = false;
};

// One routing matrix: the columns, and the best route derived from them.
// Level 1 routing keeps one indexed by node number; level 2 keeps a second
// indexed by area.  Port of the vectors L1Router and L2Router allocate --
// minhops/mincost/oadj and aminhops/amincost/aoadj -- gathered into one
// place so the route computation can serve both.
struct RouteMatrix {
    std::map<Adjacency *, std::unique_ptr<RouteInfo>> columns;
    std::vector<std::uint8_t>  minhops;
    std::vector<std::uint16_t> mincost;
    std::vector<Adjacency *>   oadj;
    unsigned maxid = 0, maxhops = 0, maxcost = 0;

    void resize (unsigned max_id, unsigned max_hops, unsigned max_cost)
    {
        maxid = max_id; maxhops = max_hops; maxcost = max_cost;
        minhops.assign (maxid + 1, static_cast<std::uint8_t> (INFHOPS));
        mincost.assign (maxid + 1, static_cast<std::uint16_t> (INFCOST));
        oadj.assign (maxid + 1, nullptr);
    }
};

class L1Router : public BaseRouter {
public:
    L1Router (Element *parent, const Config &config);
    ~L1Router () override;

    std::uint8_t ntype () const noexcept override { return L1ROUTER; }

    void start () override;

    // Adjacency changes drive the route computation.
    void adj_up (const AdjacencyPtr &adj) override;
    void adj_down (const AdjacencyPtr &adj) override;

    // A data packet arrived and needs forwarding.
    void forward (ShortData &pkt) override;

    // A routing message arrived from a neighbour.
    void routing_message (const RoutingMessage &msg, Adjacency *from,
                          unsigned circuit_cost) override;

    // Send NSP data to a destination.
    void send (Bytes data, Nodeid dest, bool rqr = false);
    void send_nsp (const Bytes &data, Nodeid dest, bool rqr = false) override
    { send (data, dest, rqr); }

    // ------------------------------------------------- routing table view
    // The output adjacency for a destination, or null if unreachable.
    Adjacency *next_hop (Nodeid dest) const;
    unsigned hops_to (unsigned tid) const;
    unsigned cost_to (unsigned tid) const;
    bool reachable (unsigned tid) const;

    // What this node advertises for destination tid.
    std::uint16_t advertised_entry (unsigned tid) const;

    unsigned maxhops () const noexcept { return l1_.maxhops; }
    unsigned maxcost () const noexcept { return l1_.maxcost; }
    unsigned maxvisits () const noexcept { return maxvisits_; }

    // Counters the tests and the monitoring interfaces both want.
    std::uint64_t aged_loss () const noexcept { return aged_loss_; }
    std::uint64_t unreach_loss () const noexcept { return unreach_loss_; }
    std::uint64_t oor_loss () const noexcept { return oor_loss_; }
    std::uint64_t transit_sent () const noexcept { return transit_sent_; }

    // Mark destinations for advertisement on every circuit.
    void set_srm (unsigned first, unsigned last);

    Update *update_for (Circuit *c) const;

protected:
    // Recompute the best route for destinations first..last in one matrix.
    // Port of L1Router.doroute, whose l2 flag chooses which matrix; here
    // the matrix is the argument.  "extra" is an additional column that is
    // not in the map -- the shared endnode column, for level 1.
    //
    // on_change is called for each destination whose advertised hops or
    // cost moved, which is what schedules an update.
    // on_reach is called when a destination becomes reachable or stops
    // being reachable, which is a different question from whether its cost
    // moved and is what the reachability change event reports.
    void compute (RouteMatrix &m, unsigned first, unsigned last,
                  const RouteInfo *extra,
                  const std::function<void (unsigned)> &on_change,
                  const std::function<void (unsigned, bool)> &on_reach);

    // Raise a node or area reachability change event.
    void reach_event (events::EventId ev, nice::Entity entity, bool reachable);

    // Where should a packet for dest go?  Null for unreachable; sets
    // out_of_range when the address cannot exist here at all.
    virtual Adjacency *find_oadj (Nodeid dest, bool &out_of_range) const;

    // Recompute level 1 routes for a range of node numbers.
    void compute_routes (unsigned first, unsigned last);

    unsigned maxvisits_ = 32;
    // The periodic routing message interval.  Point to point circuits use
    // one value, broadcast circuits a much shorter one; ports of --t1 and
    // --bct1.
    double   ptp_t1_ = 600.0;
    double   bc_t1_ = 10.0;

    std::unique_ptr<SelfAdjacency> selfadj_;
    std::unique_ptr<EndnodesRouteInfo> endnodes_;
    RouteMatrix l1_;

    std::map<Circuit *, std::unique_ptr<Update>> updates_;

    std::uint64_t aged_loss_ = 0, unreach_loss_ = 0, oor_loss_ = 0;
    std::uint64_t transit_sent_ = 0;
};

// Routing for a level 2 (area) router.  Port of routing.L2Router.
//
// It is a level 1 router that additionally keeps a second matrix indexed by
// area, exchanges L2Routing messages with other area routers, and -- when
// it can reach any area other than its own -- declares itself "attached",
// which it advertises to its own area as a route to destination 0, the
// "nearest level 2 router" entry every level 1 router uses for out of area
// traffic.
class L2Router : public L1Router {
public:
    L2Router (Element *parent, const Config &config);

    std::uint8_t ntype () const noexcept override { return L2ROUTER; }

    void start () override;
    void adj_up (const AdjacencyPtr &adj) override;
    void adj_down (const AdjacencyPtr &adj) override;
    void routing_message (const RoutingMessage &msg, Adjacency *from,
                          unsigned circuit_cost) override;

    bool attached () const noexcept { return attached_; }
    unsigned maxarea () const noexcept { return maxarea_; }

    Update *area_update_for (Circuit *c) const;

    // The area routing table, for tests and monitoring.
    bool area_reachable (unsigned area) const;
    unsigned area_hops (unsigned area) const;
    unsigned area_cost (unsigned area) const;
    std::uint16_t advertised_area_entry (unsigned area) const;

    // Mark areas for advertisement on every circuit.
    void set_asrm (unsigned first, unsigned last);

protected:
    Adjacency *find_oadj (Nodeid dest, bool &out_of_range) const override;

private:
    // Recompute area routes, then re-derive the attached flag.  Port of
    // L2Router.aroute.
    void compute_areas (unsigned first, unsigned last);

    RouteMatrix l2_;
    bool        attached_ = false;
    unsigned    maxarea_ = 63;
    std::map<Circuit *, std::unique_ptr<Update>> area_updates_;
};

}   // namespace decnet::routing

#endif  // DECNET_ROUTING_L1ROUTER_H
