// src/nsp/nice.cc -- NICE reads for NSP.
//
// Port of NSP.nice_read and NSP.read_node.  NSP answers node reads first,
// since it holds the node database; routing adds reachability afterwards.
//
// NSP owns the node counters, as it owns the node database.  The executor's
// extra counters are added by node.cc and by the router, which is where
// they are kept.

#include "decnet/common/logging.h"
#include "decnet/nice/nml.h"
#include "decnet/node.h"
#include "decnet/nsp/nsp.h"

namespace decnet::nsp {

using nice::Entity;
using nice::NiceReply;
using nice::NiceRequest;
using nice::ReplyDict;
using nice::Value;

namespace {

// The NSP version, as parameter 700 wants it: three decimal components.
Value version_value (Version v)
{
    return Value::cm ({ Value::du (v.v1), Value::du (v.v2),
                        Value::du (v.v3) });
}

void ctr (nice::NiceReply &r, std::uint16_t n, std::uint64_t v, unsigned bytes)
{
    r.params.set_counter (n, nice::Counter { v, bytes, false, 0 });
}

// The counters every node has, in the numbering and widths of the node
// counter table in nice_coding.py.
void node_counters (nice::NiceReply &r, const NodeCounters &c)
{
    ctr (r,   0, c.seconds_since_zeroed (), 2);
    ctr (r, 600, c.byt_rcv,   4);
    ctr (r, 601, c.byt_xmt,   4);
    ctr (r, 602, c.msg_rcv,   4);
    ctr (r, 603, c.msg_xmt,   4);
    ctr (r, 608, c.t_byt_rcv, 4);
    ctr (r, 609, c.t_byt_xmt, 4);
    ctr (r, 610, c.t_msg_rcv, 4);
    ctr (r, 611, c.t_msg_xmt, 4);
    ctr (r, 620, c.con_rcv,   2);
    ctr (r, 621, c.con_xmt,   2);
    ctr (r, 630, c.timeout,   2);
    ctr (r, 640, c.no_res_rcv, 2);
}

}   // namespace

unsigned NSP::links_to (Nodeid dest) const
{
    unsigned n = 0;
    for (const auto &[addr, conn] : by_addr_)
        if (conn && conn->dest () == dest) ++n;
    return n;
}

void NSP::read_node (const NiceRequest &req, Nodeid id, ReplyDict &resp,
                     unsigned links)
{
    // Asking for the entry is what creates it, as in PyDECnet.
    NiceReply &r = resp.node_entry (id);

    if (req.sumstat ()) {
        if (links) r.params.set (600, Value::du (links, 2));
        return;
    }
    if (req.counters ()) {
        // Every node has its own set, the executor included.
        if (const NodeCounters *c = counters_for (id)) node_counters (r, *c);
        return;
    }
    if (!req.chars ()) return;

    // Characteristics: nothing except for the executor.
    if (id != node ()->id ()) return;
    r.params.set (700, version_value (nspver_ph4));
    r.params.set (710, Value::du (maxconns_, 2));
}

void NSP::nice_read (const NiceRequest &req, ReplyDict &resp)
{
    // Nodes are all NSP knows about.
    if (req.entity_type != Entity::node) return;
    // And it knows nothing about which of them are adjacent: that is the
    // routing layer's question, on its circuits.
    if (req.entity.is_adjacent ()) return;

    Node *n = node ();
    if (!n) return;

    if (!req.entity.mult ()) {
        // One node, named or numbered.  node.cc has already turned a name
        // into an address and "the executor" into ours.
        read_node (req, req.entity.id, resp, links_to (req.entity.id));
        return;
    }

    // A plural request.  "Known" is every node in the database; "active"
    // and "significant" are the ones with something to say.
    for (const Nodeinfo *info : n->known_nodes ()) {
        Nodeid id = info->id;
        if (!req.entity.match (id)) continue;
        unsigned links = links_to (id);
        if (req.entity.is_known () || id == n->id ()) {
            read_node (req, id, resp, links);
        } else if (req.entity.is_loop ()) {
            // No loop nodes yet; nothing can match.
            continue;
        } else if (links) {
            // Active or significant: a node with a link to it qualifies
            // for both.
            read_node (req, id, resp, links);
        } else if (req.entity.is_significant () && req.counters ()
                   && info->counters.used ()) {
            // Significant counters: a node we have exchanged traffic with
            // has something to say even with no link open now.  Port of
            // the NSPNode.used test in NSP.nice_read.
            read_node (req, id, resp, links);
        }
    }
}

}   // namespace decnet::nsp
