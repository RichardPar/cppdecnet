// src/routing/nice.cc -- NICE reads for routing.
//
// Ports of nice_read, read_node, reach and node_char from routing.py,
// route_ptp.py and route_eth.py.  The router answers for nodes and areas;
// each circuit answers for itself and its neighbours.

#include "decnet/common/logging.h"
#include "decnet/datalink/bc.h"
#include "decnet/nice/nml.h"
#include "decnet/node.h"
#include "decnet/routing/l1router.h"
#include "decnet/routing/lan.h"
#include "decnet/routing/ptp.h"
#include "decnet/routing/routing.h"

#include <algorithm>

namespace decnet::routing {

using nice::Counter;
using nice::Entity;
using nice::NiceReply;
using nice::NiceRequest;
using nice::ReplyDict;
using nice::Value;

namespace {

// NICE node type code for a neighbour: routing type + 2 for Phase IV,
// separate values for Phase III.
unsigned nice_ntype (unsigned ntype, unsigned rphase)
{
    if (rphase == 4) return ntype + 2;
    if (rphase == 3) return ntype == ENDNODE ? nice::nice_endnode3
                                             : nice::nice_routing3;
    return nice::nice_phase2;
}

// A node parameter: the address, and the name when the database has one.
Value node_value (const nice::NiceNode &n)
{
    if (n.name.empty ())
        return Value::cm ({ Value::du (n.id.value (), 2) });
    return Value::cm ({ Value::du (n.id.value (), 2), Value::ai (n.name) });
}

// The routing layer's counters for a circuit, in the numbering and widths
// of the circuit counter table in nice_coding.py.  The datalink adds its
// own on top of these.
//
// 3900 and 3901 are PyDECnet's own numbers rather than architected ones:
// 3900 is how long the circuit has been up, which PyDECnet reports the same
// way, and 3901 is adjacency down, which PyDECnet keeps but shows only on
// its own web page.  See PORTING.md.
void circuit_counters (NiceReply &r, const CircuitCounters &c)
{
    r.params.set_counter (800, Counter { c.term_recv,  4, false, 0 });
    r.params.set_counter (801, Counter { c.orig_sent,  4, false, 0 });
    r.params.set_counter (810, Counter { c.trans_recv, 4, false, 0 });
    r.params.set_counter (811, Counter { c.trans_sent, 4, false, 0 });
    r.params.set_counter (820, Counter { c.cir_down,   1, false, 0 });
    r.params.set_counter (821, Counter { c.init_fail,  1, false, 0 });
    r.params.set_counter (900, Counter { c.peak_adj,   1, false, 0 });
    if (c.ever_up ())
        r.params.set_counter (3900, Counter { c.seconds_since_up (), 2,
                                              false, 0 });
    r.params.set_counter (3901, Counter { c.adj_down,  1, false, 0 });
}

}   // namespace

// ------------------------------------------------------------ BaseRouter

void BaseRouter::node_char (NiceReply &)
{
    // An endnode has no routing characteristics of its own beyond what
    // read_node already fills in.
}

void BaseRouter::reach (const NiceRequest &, ReplyDict &, const std::string *)
{
    // No routing table, nothing to report.
}

void BaseRouter::read_node (const NiceRequest &req, Nodeid id, ReplyDict &resp)
{
    if (!req.chars ()) return;
    // Characteristics apply only to the executor, and the caller has
    // already checked that this is it.
    NiceReply &r = resp.node_entry (id);
    Version v = tiver ();
    r.params.set (900, Value::cm ({ Value::du (v.v1), Value::du (v.v2),
                                    Value::du (v.v3) }));
    r.params.set (901, Value::c (nice_ntype (ntype (),
                                             phase () == Phase::ph4 ? 4 : 3)));
    r.params.set (932, Value::du (MTU, 2));
    node_char (r);
}

void BaseRouter::nice_read (const NiceRequest &req, ReplyDict &resp)
{
    switch (req.entity_type) {
    case Entity::node: {
        // A circuit qualifier narrows the answer to neighbours on one
        // circuit.  "Known circuits" is the same as no qualifier.
        const std::string *qual = nullptr;
        if (req.has_qual_circuit && !req.qual_circuit.empty ())
            qual = &req.qual_circuit;
        if (req.counters ()) return;    // NSP owns the node counters
        if (req.chars ()) {
            // Only the executor has node characteristics.
            if ((req.entity.one () && req.entity.id == nodeid_)
                || (req.entity.mult () && !req.entity.is_adjacent ()
                    && !req.entity.is_loop ()))
                read_node (req, nodeid_, resp);
            return;
        }
        // Summary or status.
        if (req.entity.one ()) {
            read_node (req, req.entity.id, resp);
            // An endnode has no routing table, so its one circuit is the
            // only thing that can say anything about a neighbour.
            for (PtpCircuit *c : circuit_order_) c->nice_read (req, resp);
            for (LanCircuit *c : lan_order_) c->nice_read (req, resp);
            return;
        }
        // Plural: start with the adjacencies, which every node type has.
        for (PtpCircuit *c : circuit_order_)
            if (!qual || c->name () == *qual) c->nice_read (req, resp);
        for (LanCircuit *c : lan_order_)
            if (!qual || c->name () == *qual) c->nice_read (req, resp);
        if (!req.entity.is_adjacent () && !req.entity.is_loop ())
            reach (req, resp, qual);
        return;
    }
    case Entity::circuit: {
        const Nodeid *qual = nullptr;
        Nodeid qnode;
        if (req.has_qual_node) {
            if (req.qual_node.code < 0) return;   // a plural qualifier
            qnode = req.qual_node.id;
            qual = &qnode;
        }
        if (req.entity.code > 0) {
            // One named circuit.
            if (PtpCircuit *c = circuit (req.entity.name)) {
                c->nice_read (req, resp, qual);
                return;
            }
            if (LanCircuit *c = lan_circuit (req.entity.name)) {
                c->nice_read (req, resp, qual);
                return;
            }
            return;     // no such circuit: the caller reports that
        }
        // Active or known circuits, which come to the same thing here:
        // every circuit we have is on.
        for (PtpCircuit *c : circuit_order_) c->nice_read (req, resp, qual);
        for (LanCircuit *c : lan_order_) c->nice_read (req, resp, qual);
        return;
    }
    default:
        return;
    }
}

// -------------------------------------------------------------- L1Router

void L1Router::nice_counters (NiceReply &r)
{
    r.params.set_counter (900, Counter { aged_loss_, 1, false, 0 });
    r.params.set_counter (901, Counter { unreach_loss_, 2, false, 0 });
    r.params.set_counter (902, Counter { oor_loss_, 1, false, 0 });
    r.params.set_counter (920, Counter { partial_update_loss_, 1, false, 0 });
}

void L1Router::node_char (NiceReply &r)
{
    r.params.set (920, Value::du (maxnodes_, 2));
    r.params.set (922, Value::du (l1_.maxcost, 2));
    r.params.set (923, Value::du (l1_.maxhops, 1));
    r.params.set (924, Value::du (maxvisits_, 1));
    r.params.set (910, Value::du (static_cast<std::uint64_t> (ptp_t1_), 2));
    r.params.set (912, Value::du (static_cast<std::uint64_t> (bc_t1_), 2));
}

void L1Router::read_node (const NiceRequest &req, Nodeid id, ReplyDict &resp)
{
    if (req.chars ()) {
        BaseRouter::read_node (req, id, resp);
        return;
    }
    if (!req.sumstat ()) return;

    // Which matrix covers this address.  Hops and cost are unknown for nodes
    // in other areas.
    bool in_area = id.area () == homearea ();
    Adjacency *a = nullptr;
    if (in_area) {
        unsigned tid = id.tid ();
        if (tid <= l1_.maxid) a = l1_.oadj[tid];
    } else {
        bool oor = false;
        a = find_oadj (id, oor);
    }
    if (a == selfadj_.get ()) return;   // ourselves, which is not a route

    NiceReply &r = resp.node_entry (id);
    if (!a) {
        r.params.set (0, Value::c (5));         // Unreachable
        return;
    }
    r.params.set (0, Value::c (4));             // Reachable
    r.params.set (822, Value::ai (a->circuit () ? a->circuit ()->name ()
                                                : std::string ()));
    if (req.stat ()) {
        if (in_area) {
            unsigned tid = id.tid ();
            r.params.set (821, Value::du (l1_.minhops[tid], 1));
            r.params.set (820, Value::du (l1_.mincost[tid], 2));
        }
        if (id == a->nodeid ()) {
            // The destination is the neighbour itself, so we know what
            // kind of node it is.
            r.params.set (810, Value::c (nice_ntype (a->ntype (), 4)));
        }
    } else {
        // Summary only, as RSX does: naming the next node makes the
        // tabular output far easier to read than hops and cost do.
        r.params.set (830, node_value (node ()->nicenode (a->nodeid ())));
    }
}

// Does the configuration give this address a name?  That is what makes an
// otherwise silent address one we know about rather than one we do not.
bool L1Router::named_node (Nodeid id) const
{
    const Nodeinfo *ni = const_cast<Node *> (node ())->find_node (id, false);
    return ni && !ni->name.empty ();
}

void L1Router::reach (const NiceRequest &req, ReplyDict &resp,
                      const std::string *qual)
{
    for (unsigned i = 1; i <= l1_.maxid; ++i) {
        Adjacency *a = l1_.oadj[i];
        if (a == selfadj_.get ()) continue;
        Nodeid id (homearea (), i);
        if (a) {
            if (qual && (!a->circuit () || a->circuit ()->name () != *qual))
                continue;
            NiceReply &r = resp.node_entry (id);
            r.params.set (0, Value::c (4));     // Reachable
            r.params.set (822, Value::ai (a->circuit () ? a->circuit ()->name ()
                                                        : std::string ()));
            if (req.stat ()) {
                r.params.set (821, Value::du (l1_.minhops[i], 1));
                r.params.set (820, Value::du (l1_.mincost[i], 2));
            } else {
                r.params.set (830, node_value (node ()->nicenode (a->nodeid ())));
            }
        } else if (resp.every_address () || resp.contains_node (id)
                   || named_node (id)) {
            // Report unreachable nodes only if named in the configuration or already
            // in the reply, unless all nodes were requested.  PyDECnet reports every
            // address up to maxnodes, which is over a thousand replies.
            resp.node_entry (id).params.set (0, Value::c (5));
        }
    }
}

void L1Router::nice_read (const NiceRequest &req, ReplyDict &resp)
{
    BaseRouter::nice_read (req, resp);
}

// -------------------------------------------------------------- L2Router

void L2Router::node_char (NiceReply &r)
{
    L1Router::node_char (r);
    r.params.set (925, Value::du (maxarea_, 1));
    r.params.set (928, Value::du (l2_.maxcost, 2));
    r.params.set (929, Value::du (l2_.maxhops, 1));
}

void L2Router::read_node (const NiceRequest &req, Nodeid id, ReplyDict &resp)
{
    L1Router::read_node (req, id, resp);
}

void L2Router::nice_read (const NiceRequest &req, ReplyDict &resp)
{
    if (req.entity_type != Entity::area) {
        L1Router::nice_read (req, resp);
        return;
    }
    // Areas.  There is no such thing as area characteristics or area
    // counters, so anything but summary and status asks for nothing.
    if (!req.sumstat ()) return;

    auto fill = [&] (unsigned i) {
        Adjacency *a = (i <= l2_.maxid) ? l2_.oadj[i] : nullptr;
        NiceReply &r = resp.area_entry (i);
        if (!a) {
            r.params.set (0, Value::c (5));     // Unreachable
            return;
        }
        r.params.set (0, Value::c (4));         // Reachable
        if (a == selfadj_.get ()) {
            // Our own area: we are the next hop.
            r.params.set (830, node_value (node ()->nicenode (nodeid ())));
        } else {
            r.params.set (822, Value::ai (a->circuit () ? a->circuit ()->name ()
                                                        : std::string ()));
            r.params.set (830, node_value (node ()->nicenode (a->nodeid ())));
        }
        if (req.stat ()) {
            r.params.set (821, Value::du (l2_.minhops[i], 1));
            r.params.set (820, Value::du (l2_.mincost[i], 2));
        }
    };

    if (req.entity.mult ()) {
        // The reachable ones only: an unreachable area is not news.
        for (unsigned i = 1; i <= maxarea_ && i <= l2_.maxid; ++i)
            if (l2_.oadj[i]) fill (i);
        return;
    }
    unsigned i = req.entity.area;
    if (i == 0 || i > maxarea_) return;
    fill (i);
}

// ------------------------------------------------------------ PtpCircuit

void PtpCircuit::nice_read (const NiceRequest &req, ReplyDict &resp,
                            const Nodeid *adj_qual)
{
    if (req.entity_type == Entity::node) {
        if (!req.sumstat () || req.entity.is_loop ()) return;
        if (!running ()) return;
        Nodeid neighbour = info_.id;
        if (req.entity.one () && neighbour != req.entity.id) return;
        NiceReply &r = resp.node_entry (neighbour);
        r.params.set (822, Value::ai (name_));
        if (req.stat ())
            r.params.set (810, Value::c (nice_ntype (info_.ntype,
                                                     info_.rphase)));
        else
            r.params.set (830, node_value (node ()->nicenode (neighbour)));
        return;
    }
    if (req.entity_type != Entity::circuit) return;

    // A qualified circuit read wants only the circuit whose neighbour is
    // the named node.
    if (adj_qual && (!running () || info_.id != *adj_qual)) return;

    NiceReply &r = resp.named_entry (name_);
    if (req.sumstat ()) {
        r.params.set (0, Value::c (0));         // On
        int sub = nice_substate ();
        if (sub >= 0) {
            r.params.set (1, Value::c (static_cast<unsigned> (sub)));
        } else {
            // Running: there is a neighbour to name.
            r.params.set (800, node_value (node ()->nicenode (info_.id)));
            if (req.stat ()) r.params.set (810, Value::du (info_.blksize, 2));
        }
    } else if (req.chars ()) {
        r.params.set (900, Value::du (cost_, 1));
        r.params.set (906, Value::du (static_cast<std::uint64_t> (t3_), 2));
        if (adj_) r.params.set (907,
                                Value::du (static_cast<std::uint64_t>
                                           (adj_->listen_time ()), 2));
    } else if (req.counters ()) {
        r.params.set_counter (0, Counter { node ()->seconds_since_zeroed (),
                                           2, false, 0 });
        circuit_counters (r, counters_);
    }
    if (port_) port_->nice_read_port (req, r);
}

// ------------------------------------------------------------ LanCircuit

void LanCircuit::nice_read (const NiceRequest &req, ReplyDict &resp,
                            const Nodeid *adj_qual)
{
    if (req.entity_type == Entity::node) {
        if (!req.sumstat () || req.entity.is_loop ()) return;
        // In node address order, so the listing is stable.
        std::vector<const LanAdjacency *> up;
        for (const auto &[key, a] : adjacencies_)
            if (a.state == AdjState::up && a.adj) up.push_back (&a);
        std::sort (up.begin (), up.end (),
                   [] (const LanAdjacency *x, const LanAdjacency *y)
                   { return x->adj->nodeid ().value () < y->adj->nodeid ().value (); });
        for (const LanAdjacency *a : up) {
            Nodeid neighbour = a->adj->nodeid ();
            if (req.entity.one () && req.entity.id != neighbour) continue;
            NiceReply &r = resp.node_entry (neighbour);
            r.params.set (822, Value::ai (name_));
            if (req.stat ()) {
                r.params.set (810, Value::c (nice_ntype (a->ntype, 4)));
                // Fill in the distance in case the routing table does not:
                // an adjacent node is one hop away at the circuit's cost.
                r.params.set (821, Value::du (1, 1));
                r.params.set (820, Value::du (cost_, 2));
            } else {
                r.params.set (830, node_value (node ()->nicenode (neighbour)));
            }
        }
        return;
    }
    if (req.entity_type != Entity::circuit) return;

    // One reply per adjacency, or one reply if none.  For summary, include the
    // adjacency only if there is exactly one and no name was given.
    //
    // Counters belong to the circuit rather than to any one neighbour, so a
    // counters read makes the single unadorned reply and skips this.
    bool all = req.stat () || req.chars ()
        || (!adj_qual && adjacencies_.size () == 1);
    std::vector<NiceReply *> made;
    for (const auto &[key, a] : adjacencies_) {
        if (req.counters ()) break;
        if (a.state != AdjState::up || !a.adj) continue;
        if (!all && a.ntype == ENDNODE) continue;
        Nodeid neighbour = a.adj->nodeid ();
        if (adj_qual && *adj_qual != neighbour) continue;
        NiceReply &r = resp.add_named (name_);
        r.params.set (800, node_value (node ()->nicenode (neighbour)));
        if (req.stat ()) r.params.set (810, Value::du (a.adj->blksize (), 2));
        else if (req.chars ())
            r.params.set (907, Value::du (static_cast<std::uint64_t>
                                          (a.listen_time), 2));
        made.push_back (&r);
    }
    NiceReply *first = nullptr;
    if (!made.empty ()) first = made.front ();
    else if (adj_qual) return;      // qualified and nothing matched
    else first = &resp.named_entry (name_);

    if (req.sumstat ()) {
        first->params.set (0, Value::c (0));    // On
    } else if (req.chars ()) {
        first->params.set (906, Value::du (static_cast<std::uint64_t> (t3_), 2));
        first->params.set (900, Value::du (cost_, 1));
        nice_char (*first);
    } else if (req.counters ()) {
        first->params.set_counter (0,
            Counter { node ()->seconds_since_zeroed (), 2, false, 0 });
        circuit_counters (*first, counters_);
    }
    if (port_) port_->nice_read_port (req, *first);
}

void RoutingLanCircuit::nice_char (NiceReply &r) const
{
    r.params.set (902, Value::du (prio_, 1));
    r.params.set (901, Value::du (maxrouters_, 1));
    // Designated router reported as a characteristic, as RSX and VMS do.
    if (isdr_)
        r.params.set (801, Value::cm ({ Value::du (parent_->nodeid ().value (), 2) }));
    else if (dr_.value ())
        r.params.set (801, Value::cm ({ Value::du (dr_.value (), 2) }));
}

}   // namespace decnet::routing
