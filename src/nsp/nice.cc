// src/nsp/nice.cc -- NICE reads for NSP.
//
// Port of NSP.nice_read and NSP.read_node.  NSP answers node reads first,
// since it holds the node database; routing adds reachability afterwards.
//
// PORT: per node counters are not reported, and "active nodes" means
// nodes with a link.  See NOTDONE.md.

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
        }
    }
}

}   // namespace decnet::nsp
