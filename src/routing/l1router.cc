#include "decnet/routing/l1router.h"
#include "decnet/events/events.h"

#include "decnet/common/logging.h"
#include "decnet/config.h"
#include "decnet/node.h"
#include "decnet/routing/lan.h"
#include "decnet/routing/ptp.h"

#include <algorithm>

namespace decnet::routing {

namespace {
constexpr unsigned VISIT_LIMIT_MAX = 63;   // the visit field is six bits
}

// ------------------------------------------------------------- L1Router

L1Router::L1Router (Element *parent, const Config &config)
    : BaseRouter (parent, config)
{
    const auto &rc = *config.routing ();
    maxnodes_  = std::min (rc.maxnodes, 1023u);
    maxvisits_ = rc.maxvisits;
    ptp_t1_ = rc.t1 ? static_cast<double> (rc.t1) : 600.0;
    bc_t1_  = rc.bct1 ? static_cast<double> (rc.bct1) : 10.0;
    l1_.resize (maxnodes_, rc.maxhops, rc.maxcost);

    init_circuits (config);

    // The shared column for endnode adjacencies, and the pseudo-adjacency
    // that stands for this node itself.
    endnodes_ = std::make_unique<EndnodesRouteInfo> (maxnodes_);
    selfadj_ = std::make_unique<SelfAdjacency> (this, nodeid_, L1ROUTER);

    // One update process per circuit (L2Router adds one for the area table).
    // Broadcast circuits use the shorter --bct1 period.
    for (PtpCircuit *c : circuit_order_)
        updates_[c] = std::make_unique<Update> (c, this, ptp_t1_, 1);
    for (LanCircuit *c : lan_order_)
        updates_[c] = std::make_unique<Update> (c, this, bc_t1_, 1);
}

L1Router::~L1Router () = default;

void L1Router::start ()
{
    BaseRouter::start ();
    // Bring up the column that describes this node: zero hops and zero
    // cost to ourselves, infinite to everything else.
    auto self = std::make_unique<RouteInfo> (maxnodes_);
    self->adj = selfadj_.get ();
    self->nodeid = nodeid_;
    self->hops[tid ()] = 0;
    self->cost[tid ()] = 0;
    l1_.columns[selfadj_.get ()] = std::move (self);

    compute_routes (0, maxnodes_);
    for (auto &[c, u] : updates_) u->start ();
}

void L1Router::adj_up (const AdjacencyPtr &adj)
{
    BaseRouter::adj_up (adj);
    unsigned tid_ = adj->nodeid ().tid ();

    if (adj->ntype () == L1ROUTER || adj->ntype () == L2ROUTER) {
        // A router neighbour gets its own column, which its routing
        // messages will fill in.
        auto col = std::make_unique<RouteInfo> (maxnodes_);
        col->adj = adj.get ();
        col->nodeid = adj->nodeid ();
        l1_.columns[adj.get ()] = std::move (col);
        // Tell it everything we know, as soon as we may.
        set_srm (0, maxnodes_);
    } else {
        // An endnode does not advertise, so we fill its entry in ourselves:
        // one hop away, at the cost of the circuit it is on.
        if (tid_ <= maxnodes_) {
            unsigned cost = adj->circuit_cost ();
            endnodes_->hops[tid_] = 1;
            endnodes_->cost[tid_] = static_cast<std::uint16_t> (cost);
            endnodes_->adjacencies[tid_] = adj.get ();
            compute_routes (tid_, tid_);
        }
    }
}

void L1Router::adj_down (const AdjacencyPtr &adj)
{
    std::uint8_t ntype = adj->ntype ();
    unsigned tid_ = adj->nodeid ().tid ();
    BaseRouter::adj_down (adj);

    if (ntype == L1ROUTER || ntype == L2ROUTER) {
        l1_.columns.erase (adj.get ());
        // Everything that neighbour was carrying may have moved.
        compute_routes (0, maxnodes_);
    } else if (tid_ <= maxnodes_) {
        endnodes_->hops[tid_] = static_cast<std::uint8_t> (INFHOPS);
        endnodes_->cost[tid_] = static_cast<std::uint16_t> (INFCOST);
        endnodes_->adjacencies[tid_] = nullptr;
        compute_routes (tid_, tid_);
    }
}

void L1Router::routing_message (const RoutingMessage &msg, Adjacency *from,
                                unsigned circuit_cost)
{
    // A level 1 router only understands level 1 messages.
    if (dynamic_cast<const L2Routing *> (&msg)) return;

    auto it = l1_.columns.find (from);
    if (it == l1_.columns.end () || !it->second) {
        DN_TRACE ("routing message from {} with no column",
                  from ? from->nodeid ().str () : "?");
        return;
    }
    RouteInfo &col = *it->second;

    unsigned maxreach = 0;
    for (const RouteUpdate &u : msg.updates (circuit_cost)) {
        if (u.id > maxnodes_) {
            // The neighbour is telling us about nodes our table cannot
            // hold.  Worth an event, but only if it claims they exist.
            if (u.hops <= INFHOPS && u.cost < INFCOST)
                maxreach = std::max (maxreach, u.id);
            continue;
        }
        std::uint8_t h = static_cast<std::uint8_t> (std::min (u.hops, INFHOPS));
        std::uint16_t c = static_cast<std::uint16_t> (std::min (u.cost, INFCOST));
        if (col.hops[u.id] != h || col.cost[u.id] != c) {
            col.hops[u.id] = h;
            col.cost[u.id] = c;
            compute_routes (u.id, u.id);
        }
    }
    if (maxreach) {
        // Part of the neighbour's update named nodes our table cannot hold,
        // so that part of it is lost.  That is what the architecture calls
        // partial routing update loss, counter 920 and event 4.3.
        ++partial_update_loss_;
        DN_DEBUG ("routing update from {} mentions node {}, beyond maxnodes {}",
                  from->nodeid ().str (), maxreach, maxnodes_);
        if (Node *n = node ()) {
            events::Event e { { 4, 3 },
                              from->circuit ()
                                  ? nice::Entity::make_circuit (
                                        from->circuit ()->name ())
                                  : nice::Entity::make_none () };
            e.param (events::param::highest_address,
                     nice::Value::du (maxreach, 2));
            n->logevent (e);
        }
    }
}

namespace {

// "Packet header" event parameter: flags, addresses and visit count, as a
// coded multiple.
nice::Value packet_header (const ShortData &pkt)
{
    std::uint8_t flags = static_cast<std::uint8_t> (
        pkt.sfpd | (pkt.rqr ? 0x08 : 0) | (pkt.rts ? 0x10 : 0)
        | (pkt.vers ? 0x20 : 0) | (pkt.pf ? 0x80 : 0));
    return nice::Value::cm ({ nice::Value::h (flags, 1),
                              nice::Value::du (pkt.dstnode.value (), 2),
                              nice::Value::du (pkt.srcnode.value (), 2),
                              nice::Value::du (pkt.visit, 1) });
}

}   // namespace

// Report a reachability change for a node (level 1) or area (level 2).
void L1Router::reach_event (events::EventId ev, nice::Entity entity,
                            bool reachable)
{
    Node *n = node ();
    if (!n) return;
    events::Event e { ev, std::move (entity) };
    e.coded (events::param::status,
             reachable ? events::status::reachable
                       : events::status::unreachable);
    n->logevent (e);
}

void L1Router::compute (RouteMatrix &m, unsigned first, unsigned last,
                        const RouteInfo *extra,
                              const std::function<void (unsigned)> &on_change,
                        const std::function<void (unsigned, bool)> &on_reach)
{
    last = std::min (last, m.maxid);
    for (unsigned i = first; i <= last; ++i) {
        unsigned   besth = INFHOPS, bestc = INFCOST;
        Adjacency *besta = nullptr;

        auto consider = [&] (const RouteInfo &r) {
            Adjacency *a = r.adjacency (i);
            // Lowest cost wins; ties go to the higher neighbour address.
            if (r.cost[i] < bestc
                || (r.cost[i] == bestc && a && besta
                    && a->nodeid () > besta->nodeid ())) {
                bestc = r.cost[i];
                besth = r.hops[i];
                besta = a;
            }
        };
        for (const auto &[key, col] : m.columns)
            if (col) consider (*col);
        if (extra) consider (*extra);

        if (bestc > m.maxcost || besth > m.maxhops) {
            besth = INFHOPS;
            bestc = INFCOST;
            besta = nullptr;
        }

        if (m.minhops[i] != besth || m.mincost[i] != bestc) {
            m.minhops[i] = static_cast<std::uint8_t> (besth);
            m.mincost[i] = static_cast<std::uint16_t> (bestc);
            on_change (i);
            DN_TRACE ("destination {}: cost {}, hops {} via {}", i, bestc,
                      besth, besta ? besta->nodeid ().str () : "unreachable");
        }
        if (besta != m.oadj[i]) {
            bool reach_change = !besta || !m.oadj[i];
            m.oadj[i] = besta;
            if (reach_change && besta != selfadj_.get ()) {
                DN_DEBUG ("destination {} is now {}", i,
                          besta ? "reachable" : "unreachable");
                on_reach (i, besta != nullptr);
            }
        }
    }
}

void L1Router::compute_routes (unsigned first, unsigned last)
{
    compute (l1_, first, last, endnodes_.get (),
             [this] (unsigned i) { set_srm (i, i); },
             [this] (unsigned i, bool reachable) {
                 Nodeid id (homearea (), i);
                 reach_event ({ 4, 14 },
                              nice::Entity::make_node (node ()
                                  ? node ()->nicenode (id)
                                  : nice::NiceNode (id)),
                              reachable);
             });
}

Adjacency *L1Router::find_oadj (Nodeid dest, bool &out_of_range) const
{
    out_of_range = false;
    unsigned area = dest.area ();
    unsigned t = dest.tid ();
    if (area != homearea ()) {
        // Out of area traffic goes to entry zero, the nearest level 2 router.
        t = 0;
    }
    if (t >= l1_.oadj.size ()) {
        out_of_range = true;
        return nullptr;
    }
    return l1_.oadj[t];
}

Adjacency *L1Router::next_hop (Nodeid dest) const
{
    bool oor = false;
    return find_oadj (dest, oor);
}

unsigned L1Router::hops_to (unsigned t) const
{ return t < l1_.minhops.size () ? l1_.minhops[t] : INFHOPS; }

unsigned L1Router::cost_to (unsigned t) const
{ return t < l1_.mincost.size () ? l1_.mincost[t] : INFCOST; }

bool L1Router::reachable (unsigned t) const
{ return t < l1_.oadj.size () && l1_.oadj[t] != nullptr; }

std::uint16_t L1Router::advertised_entry (unsigned t) const
{
    if (t >= l1_.minhops.size ()) return route_entry (INFHOPS, INFCOST);
    return route_entry (l1_.minhops[t], l1_.mincost[t]);
}

void L1Router::set_srm (unsigned first, unsigned last)
{
    for (auto &[c, u] : updates_) u->set_srm (first, last);
}

Update *L1Router::update_for (Circuit *c) const
{
    auto it = updates_.find (c);
    return it == updates_.end () ? nullptr : it->second.get ();
}

void L1Router::forward (ShortData &pkt)
{
    bool oor = false;
    Adjacency *a = find_oadj (pkt.dstnode, oor);
    bool aged = false;
    // No arrival circuit means we originated this packet.  Port of the
    // "orig" test in routing.py's forward.
    const bool orig = pkt.src == nullptr;

    if (a && !oor) {
        unsigned limit = maxvisits_;
        if (!a->is_self ()) {
            // A packet being returned to its sender gets a doubled budget,
            // capped by what the six bit field can hold.
            if (pkt.rts) limit = std::min (limit * 2, VISIT_LIMIT_MAX);
            if (pkt.visit < limit) {
                ++pkt.visit;
                ++transit_sent_;
            } else {
                aged = true;
            }
        }
        if (!aged) {
            // Count the crossing before handing the packet over: sending
            // to ourselves crosses no circuit and is counted in deliver().
            if (!a->is_self ()) {
                Circuit *out = a->circuit ();
                if (orig) {
                    if (out) ++out->counters ().orig_sent;
                } else {
                    ++pkt.src->counters ().trans_recv;
                    if (out) ++out->counters ().trans_sent;
                }
            }
            a->send (pkt);
            return;
        }
    }

    // Undeliverable: unreachable, out of range, or too many visits.  Return to
    // sender if requested and not already returning; otherwise drop and count.
    if (pkt.rqr && !pkt.rts) {
        std::swap (pkt.dstnode, pkt.srcnode);
        pkt.rts = true;
        pkt.rqr = false;
        pkt.ie = false;
        forward (pkt);
        return;
    }
    events::EventId ev;
    if (aged) {
        ++aged_loss_;
        DN_TRACE ("dropping packet for {}: visit limit", pkt.dstnode.str ());
        ev = { 4, 0 };                      // aged packet loss
    } else if (oor) {
        ++oor_loss_;
        DN_TRACE ("dropping packet for {}: address out of range",
                  pkt.dstnode.str ());
        ev = { 4, 2 };                      // node out-of-range packet loss
    } else {
        ++unreach_loss_;
        DN_TRACE ("dropping packet for {}: unreachable", pkt.dstnode.str ());
        ev = { 4, 1 };                      // node unreachable packet loss
    }
    if (Node *n = node ()) {
        // The packet carries the circuit it arrived on, so the event can
        // name it; a packet we originated has none to name.
        events::Event e { ev, pkt.src
                                  ? nice::Entity::make_circuit (pkt.src->name ())
                                  : nice::Entity::make_none () };
        e.param (events::param::packet_header, packet_header (pkt));
        n->logevent (e);
    }
}

void L1Router::send (Bytes data, Nodeid dest, bool rqr)
{
    ShortData pkt;
    pkt.rqr     = rqr;
    pkt.dstnode = dest;
    pkt.srcnode = nodeid_;
    pkt.visit   = 0;
    pkt.payload = std::move (data);
    // Originating, so the visit count is not incremented and no event is
    // logged if the destination turns out to be unreachable.
    forward (pkt);
}

// -------------------------------------------------------------- L2Router

L2Router::L2Router (Element *parent, const Config &config)
    : L1Router (parent, config)
{
    const auto &rc = *config.routing ();
    maxarea_ = std::min (rc.maxarea, 63u);
    (void) rc;
    l2_.resize (maxarea_, rc.amaxhops, rc.amaxcost);

    // A second update process per circuit, for the area table.
    for (PtpCircuit *c : circuit_order_)
        area_updates_[c] = std::make_unique<Update> (c, this, ptp_t1_, 2);
    for (LanCircuit *c : lan_order_)
        area_updates_[c] = std::make_unique<Update> (c, this, bc_t1_, 2);
}

void L2Router::start ()
{
    L1Router::start ();

    // Our own area is reachable at no cost, through ourselves.
    auto self = std::make_unique<RouteInfo> (maxarea_);
    self->adj = selfadj_.get ();
    self->nodeid = nodeid_;
    self->hops[homearea ()] = 0;
    self->cost[homearea ()] = 0;
    l2_.columns[selfadj_.get ()] = std::move (self);

    compute_areas (1, maxarea_);
    for (auto &[c, u] : area_updates_) u->start ();
}

void L2Router::adj_up (const AdjacencyPtr &adj)
{
    if (adj->ntype () == L2ROUTER) {
        // Another area router: it gets a column in the area table, which
        // its L2Routing messages will fill in.
        auto col = std::make_unique<RouteInfo> (maxarea_);
        col->adj = adj.get ();
        col->nodeid = adj->nodeid ();
        l2_.columns[adj.get ()] = std::move (col);
    }
    // The level 1 side handles the rest, including declining to do level 1
    // work for an out of area neighbour.
    L1Router::adj_up (adj);
    if (adj->ntype () == L2ROUTER) set_asrm (1, maxarea_);
}

void L2Router::adj_down (const AdjacencyPtr &adj)
{
    bool was_l2 = adj->ntype () == L2ROUTER;
    Adjacency *raw = adj.get ();
    L1Router::adj_down (adj);
    if (was_l2) {
        l2_.columns.erase (raw);
        compute_areas (1, maxarea_);
    }
}

void L2Router::routing_message (const RoutingMessage &msg, Adjacency *from,
                                unsigned circuit_cost)
{
    auto *l2msg = dynamic_cast<const L2Routing *> (&msg);
    if (!l2msg) {
        L1Router::routing_message (msg, from, circuit_cost);
        return;
    }
    auto it = l2_.columns.find (from);
    if (it == l2_.columns.end () || !it->second) return;
    RouteInfo &col = *it->second;

    unsigned maxreach = 0;
    for (const RouteUpdate &u : l2msg->updates (circuit_cost)) {
        if (u.id < 1 || u.id > maxarea_) {
            // An area beyond our table, as for nodes above.
            if (u.id > maxarea_ && u.hops <= INFHOPS && u.cost < INFCOST)
                maxreach = std::max (maxreach, u.id);
            continue;
        }
        auto h = static_cast<std::uint8_t> (std::min (u.hops, INFHOPS));
        auto c = static_cast<std::uint16_t> (std::min (u.cost, INFCOST));
        if (col.hops[u.id] != h || col.cost[u.id] != c) {
            col.hops[u.id] = h;
            col.cost[u.id] = c;
            compute_areas (u.id, u.id);
        }
    }
    if (maxreach) {
        ++partial_update_loss_;
        DN_DEBUG ("area update from {} mentions area {}, beyond maxarea {}",
                  from->nodeid ().str (), maxreach, maxarea_);
        if (Node *n = node ()) {
            events::Event e { { 4, 3 },
                              from->circuit ()
                                  ? nice::Entity::make_circuit (
                                        from->circuit ()->name ())
                                  : nice::Entity::make_none () };
            e.param (events::param::highest_address,
                     nice::Value::du (maxreach, 2));
            n->logevent (e);
        }
    }
}

void L2Router::compute_areas (unsigned first, unsigned last)
{
    compute (l2_, first, last, nullptr,
             [this] (unsigned i) { set_asrm (i, i); },
             [this] (unsigned i, bool reachable) {
                 reach_event ({ 4, 17 }, nice::Entity::make_area (i),
                              reachable);
             });

    // Attached if any other area is reachable (DNA Routing 2.0.0 definition).
    bool attached = false;
    for (unsigned i = 1; i <= maxarea_; ++i)
        if (i != homearea () && l2_.oadj[i]) { attached = true; break; }

    if (attached == attached_) return;
    DN_DEBUG ("level 2 attached state changed to {}", attached);
    attached_ = attached;

    // Destination 0 in the level 1 table is "nearest level 2 router".
    // Attached area routers advertise it at zero cost.
    auto it = l1_.columns.find (selfadj_.get ());
    if (it != l1_.columns.end () && it->second) {
        RouteInfo &self = *it->second;
        self.hops[0] = attached_ ? 0 : static_cast<std::uint8_t> (INFHOPS);
        self.cost[0] = attached_ ? 0 : static_cast<std::uint16_t> (INFCOST);
    }
    set_srm (0, 0);
    compute_routes (0, 0);
}

Adjacency *L2Router::find_oadj (Nodeid dest, bool &out_of_range) const
{
    out_of_range = false;
    unsigned area = dest.area ();
    if (attached_ && area != homearea ()) {
        // We can reach other areas ourselves, so route by area.
        if (area >= l2_.oadj.size ()) {
            out_of_range = true;
            return nullptr;
        }
        return l2_.oadj[area];
    }
    // Otherwise fall back to the level 1 table, which sends out of area
    // traffic to the nearest area router at destination 0.
    return L1Router::find_oadj (dest, out_of_range);
}

bool L2Router::area_reachable (unsigned a) const
{ return a < l2_.oadj.size () && l2_.oadj[a] != nullptr; }

unsigned L2Router::area_hops (unsigned a) const
{ return a < l2_.minhops.size () ? l2_.minhops[a] : INFHOPS; }

unsigned L2Router::area_cost (unsigned a) const
{ return a < l2_.mincost.size () ? l2_.mincost[a] : INFCOST; }

std::uint16_t L2Router::advertised_area_entry (unsigned a) const
{
    if (a >= l2_.minhops.size ()) return route_entry (INFHOPS, INFCOST);
    return route_entry (l2_.minhops[a], l2_.mincost[a]);
}

void L2Router::set_asrm (unsigned first, unsigned last)
{
    for (auto &[c, u] : area_updates_) u->set_srm (first, last);
}

Update *L2Router::area_update_for (Circuit *c) const
{
    auto it = area_updates_.find (c);
    return it == area_updates_.end () ? nullptr : it->second.get ();
}

// ---------------------------------------------------------------- Update

Update::Update (Circuit *circuit, L1Router *router, double t1,
                unsigned level)
    : Element (circuit), circuit_ (circuit), router_ (router), t1_ (t1),
      level_ (level),
      // A level 1 process advertises node numbers, a level 2 one areas.
      srm_ (level == 2 ? 64 : router->maxnodes () + 1, false)
{
}

void Update::start ()
{
    running_ = true;
    lastfull_ = std::chrono::steady_clock::now ();
    if (node ()) node ()->timers ().start (this, t1_);
}

void Update::stop ()
{
    running_ = false;
    if (node ()) node ()->timers ().stop (this);
}

void Update::set_srm (unsigned id) { set_srm (id, id); }

void Update::set_srm (unsigned first, unsigned last)
{
    // Nothing to do if there is nobody on this circuit who wants them.
    if (!circuit_->wants_updates (level_)) return;

    last = std::min (last, static_cast<unsigned> (srm_.size ()) - 1);
    for (unsigned i = first; i <= last; ++i) srm_[i] = true;
    any_srm_ = true;

    // Schedule a send shortly, but not more often than T2: a burst of
    // table changes should produce one update, not one per change.
    if (!holdoff_ && running_) {
        holdoff_ = true;
        if (node ()) node ()->timers ().start (this, T2);
    }
}

void Update::timeout ()
{
    holdoff_ = false;
    if (!running_) return;
    if (!circuit_->wants_updates (level_)) {
        // Nobody to tell yet; try again at the periodic interval.
        if (node ()) node ()->timers ().start (this, t1_);
        return;
    }
    send_now ();
}

void Update::send_now ()
{
    // A triggered update carries only what changed; a periodic one carries
    // everything.
    const bool triggered = any_srm_;
    std::vector<Bytes> pkts = build (!triggered);
    for (const Bytes &p : pkts) circuit_->send_update (p);
    DN_TRACE ("sent {} {} routing update packet(s) on {}", pkts.size (),
              triggered ? "triggered" : "periodic", circuit_->name ());

    std::fill (srm_.begin (), srm_.end (), false);
    any_srm_ = false;

    // A full update restarts the t1 interval.  After a triggered update the
    // next is scheduled relative to the last full update, capped at t1, so
    // periodic updates are not postponed (Update.dispatch).
    double delta = t1_;
    auto now = std::chrono::steady_clock::now ();
    if (triggered) {
        std::chrono::duration<double> since = now - lastfull_;
        delta = std::min (since.count (), t1_);
    } else {
        lastfull_ = now;
    }
    next_interval_ = delta;
    last_complete_ = !triggered;
    if (node ()) node ()->timers ().start (this, delta);
}

std::vector<Bytes> Update::build (bool complete) const
{
    std::vector<Bytes> ret;
    // Keep each message inside the neighbour's block size, leaving room
    // for the header, the segment header and the checksum.
    std::size_t mtu = circuit_->update_blksize ();
    std::size_t room = (mtu > 16) ? mtu - 16 : 64;

    L1Routing l1msg;
    L2Routing l2msg;
    Ph4RoutingMessage &msg = (level_ == 2)
        ? static_cast<Ph4RoutingMessage &> (l2msg)
        : static_cast<Ph4RoutingMessage &> (l1msg);
    msg.srcnode = router_->nodeid ().value ();

    RouteSegment seg;
    bool have_seg = false;
    std::size_t words = 0;
    unsigned previd = 0;

    auto flush_segment = [&] {
        if (have_seg && !seg.entries.empty ()) msg.segments.push_back (seg);
        have_seg = false;
        seg = RouteSegment {};
    };
    auto flush_message = [&] {
        flush_segment ();
        if (!msg.segments.empty ()) ret.push_back (msg.encode_packet ());
        msg.segments.clear ();
        words = 0;
    };

    // Level 1 advertises node numbers from zero; level 2 advertises areas,
    // and there is no area zero.
    unsigned lowid = (level_ == 2) ? 1 : 0;
    unsigned highid = (level_ == 2)
        ? static_cast<unsigned> (srm_.size ()) - 1 : router_->maxnodes ();
    auto entry_for = [&] (unsigned i) -> std::uint16_t {
        if (level_ == 2) {
            auto *l2 = dynamic_cast<L2Router *> (router_);
            return l2 ? l2->advertised_area_entry (i)
                      : route_entry (INFHOPS, INFCOST);
        }
        return router_->advertised_entry (i);
    };

    for (unsigned i = lowid; i <= highid; ++i) {
        if (!complete && !srm_[i]) continue;
        // A gap in the destinations starts a new segment; entries within a
        // segment have to be consecutive.
        if (have_seg && i != previd + 1) flush_segment ();
        if (!have_seg) {
            seg.startid = static_cast<std::uint16_t> (i);
            have_seg = true;
            words += 2;                 // the segment's count and startid
        }
        seg.entries.push_back (entry_for (i));
        ++words;
        previd = i;
        if (words * 2 >= room) flush_message ();
    }
    flush_message ();
    return ret;
}

}   // namespace decnet::routing
