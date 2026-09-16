// decnet/nice/nml.h -- the network management listener, object 19.
//
// Port of decnet/modules/nml.py.  Answers NICE requests using data from
// each layer's nice_read.  A request may cover several entities, and an
// entity may produce several replies (one per adjacency, for instance);
// ReplyDict collects them in the order NCP expects.
//
// PORT: only READ INFORMATION and LOOP NODE.  SET and ZERO are refused;
// LOOP CIRCUIT and LOOP LINE answer "unrecognized function".  See
// NOTDONE.md.

#ifndef DECNET_NICE_NML_H
#define DECNET_NICE_NML_H

#include "decnet/nice/packets.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace decnet {
class Node;
namespace session { class Application; }
}

namespace decnet::nice {

// The version this listener speaks, sent as the accept data.  Port of
// nml.MYVERSION.
inline constexpr std::uint8_t nice_version[3] = { 4, 0, 0 };

// Replies to one request, keyed by entity.  Lookup creates an empty reply
// if none exists, as ReplyDict.__getitem__ does.
class ReplyDict {
public:
    ReplyDict (std::uint8_t entity_kind, Node *node) noexcept
        : kind_ (entity_kind), node_ (node) {}

    std::uint8_t kind () const noexcept { return kind_; }

    // Report every address in the routing table, including unreachable
    // unnamed ones.  Off by default; see L1Router::reach.
    void want_every_address (bool on) noexcept { every_address_ = on; }
    bool every_address () const noexcept { return every_address_; }

    // The reply for one entity, created empty if it does not exist yet.
    NiceReply &node_entry (Nodeid id);
    NiceReply &named_entry (const std::string &name);
    NiceReply &area_entry (unsigned area);

    // Another reply for the same entity.  All but the last are flagged "more
    // to come".
    NiceReply &add_named (const std::string &name);

    // Is there already a reply for this node?  Distinct from node_entry,
    // which would create one.
    bool contains_node (Nodeid id) const;

    bool empty () const noexcept { return numeric_.empty () && named_.empty (); }

    // Replies grouped by entity in send order: for nodes the executor first,
    // then by address (NodeReplyDict.sorted); otherwise by key.  Filtered by
    // the request's entity.
    std::vector<std::vector<NiceReply *>> sorted (const NiceRequest &req) const;

private:
    bool every_address_ = false;

    struct Group {
        std::vector<std::unique_ptr<NiceReply>> items;
    };

    NiceReply &make (Group &g, bool reuse);

    std::uint8_t kind_;
    Node        *node_;
    // Node addresses and area numbers key the first; every named entity
    // keys the second.  Only one of them is ever in use.
    std::map<std::uint32_t, Group> numeric_;
    std::map<std::string, Group>   named_;
};

// The listener itself: an application for object 19.
std::unique_ptr<session::Application> make_nml (Node *node);

}   // namespace decnet::nice

#endif  // DECNET_NICE_NML_H
