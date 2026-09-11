// decnet/nice/nml.h -- the network management listener, object 19.
//
// Port of decnet/modules/nml.py.  NCP on another node -- or on this one --
// connects to object 19 and sends NICE requests; this answers them.  The
// answers themselves come from the layers, each of which knows about its
// own entities: routing about circuits and reachability, NSP about links,
// the datalink layer about lines, MOP about the configurator module.
//
// A read request can be about one entity or about a plural one ("known
// circuits"), and one entity can produce several replies -- a circuit with
// three adjacencies reports each of them.  ReplyDict below is what collects
// that: replies keyed by entity, in groups, ordered the way NCP wants to
// print them.
//
// PORT: only read information and loop node are implemented.  Set and zero
// answer "privilege violation", because this listener is read only; loop
// circuit and loop line answer "unrecognized function".  See NOTDONE.md.

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

// The replies to one request, keyed by the entity each is about.
//
// Access creates: asking for the reply for circuit ETH-0 makes an empty one
// if there is not one already, which is what lets each layer add what it
// knows without any of them having to go first.  That is pydecnet's
// ReplyDict, whose __getitem__ does the same.
class ReplyDict {
public:
    ReplyDict (std::uint8_t entity_kind, Node *node) noexcept
        : kind_ (entity_kind), node_ (node) {}

    std::uint8_t kind () const noexcept { return kind_; }

    // The reply for one entity, created empty if it does not exist yet.
    NiceReply &node_entry (Nodeid id);
    NiceReply &named_entry (const std::string &name);
    NiceReply &area_entry (unsigned area);

    // Another reply about the same entity.  A circuit reports one reply
    // per adjacency, and they travel as a group: all but the last carry
    // "more to come for this entity".
    NiceReply &add_named (const std::string &name);

    // Is there already a reply for this node?  Distinct from node_entry,
    // which would create one.
    bool contains_node (Nodeid id) const;

    bool empty () const noexcept { return numeric_.empty () && named_.empty (); }

    // The replies, grouped by entity, in the order they should be sent.
    // For nodes that is the executor first, then the rest by address, as
    // NodeReplyDict.sorted does; for everything else it is by key.  A
    // request's own entity filters the result: a wildcard read of one area
    // must not answer about another.
    std::vector<std::vector<NiceReply *>> sorted (const NiceRequest &req) const;

private:
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
