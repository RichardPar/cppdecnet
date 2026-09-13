// src/routing/nice.cc -- what the routing layer tells network management.
//
// Port of the nice_read, read_node, reach and node_char methods of
// routing.py, route_ptp.py and route_eth.py.  They are gathered here rather
// than spread through those three files because they are one subject: what
// NCP prints for SHOW NODE, SHOW CIRCUIT and SHOW AREA, and each of them is
// only meaningful next to the others.
//
// The division of labour follows the Python's.  The router answers about
// nodes and areas, because it owns the routing table; each circuit answers
// about itself and about the neighbour at its far end, because that is
// where the adjacency lives.

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

// The NICE node type code for a neighbour, which is not the routing
// layer's own numbering: Phase IV adds two to it, and Phase III has its own
// pair of values.  Port of the identical arithmetic in read_node and in
// each circuit's nice_read.
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
            // Only the executor has node characteristics.  "Adjacent
            // nodes" and "loop nodes" exclude it by definition, so those
            // ask for nothing we have.
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

    // Which matrix answers for this address, and whether we know the
    // distance.  Out of area we do not: the level 2 route gives a next hop
    // but the hops and cost belong to the area, not the node.
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
            // Unreachable, and worth saying so only for an address we
            // actually know something about: one the configuration names,
            // or one something else in this reply has already mentioned.
            //
            // DIVERGENCE FROM THE PYTHON, deliberate.  Its reach() adds an
            // entry for every address from 1 to maxnodes when the request
            // says "known", and ours did the same.  With the default
            // maximum of 1023 that answers "show known nodes" with 1025
            // messages, of which all but a handful say only "node 1.457 is
            // unreachable" -- a thousand frames out and a thousand
            // acknowledgements back for nothing.  Against a PDP-11 that is
            // minutes, and the link times out part way through.  An address
            // nothing has ever been heard from and no configuration names
            // is not a node the system knows; it is a number.  The
            // monitoring pages, which had already grown their own filter
            // for this, ask for the full set explicitly.
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

    // One reply per adjacency, or one bare reply if there are none.  Take
    // every adjacency for status and characteristics; for a summary take
    // them only if there is exactly one and nothing was asked for by name.
    // That last rule is the Python's, and it exists to keep a summary of a
    // busy LAN down to one line per circuit.
    bool all = req.stat () || req.chars ()
        || (!adj_qual && adjacencies_.size () == 1);
    std::vector<NiceReply *> made;
    for (const auto &[key, a] : adjacencies_) {
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
    }
    if (port_) port_->nice_read_port (req, *first);
}

void RoutingLanCircuit::nice_char (NiceReply &r) const
{
    r.params.set (902, Value::du (prio_, 1));
    r.params.set (901, Value::du (maxrouters_, 1));
    // The specification calls the designated router a status item, but RSX
    // and VMS both report it as a characteristic, and following them keeps
    // the status display from looking strange.
    if (isdr_)
        r.params.set (801, Value::cm ({ Value::du (parent_->nodeid ().value (), 2) }));
    else if (dr_.value ())
        r.params.set (801, Value::cm ({ Value::du (dr_.value (), 2) }));
}

}   // namespace decnet::routing
