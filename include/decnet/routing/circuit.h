// decnet/routing/circuit.h -- what the routing layer needs from a circuit.
//
// The two circuit kinds find their neighbours in completely different
// ways: one runs a handshake, the other listens for multicast hellos.
// Once a neighbour is known the routing layer treats them the same, and
// none of what an Adjacency needs from its circuit depends on which kind
// it is.
//
// the Python gets this from duck typing.  Here it is a base class, which is
// what lets one Adjacency type serve both.

#ifndef DECNET_ROUTING_CIRCUIT_H
#define DECNET_ROUTING_CIRCUIT_H

#include "decnet/common/element.h"
#include "decnet/routing/packets.h"

#include <string>

namespace decnet::routing {

class Adjacency;

class Circuit : public Element {
public:
    Circuit (Element *parent, std::string name) noexcept
        : Element (parent), name_ (std::move (name)) {}

    const std::string &name () const noexcept { return name_; }

    // What the route computation charges for using this circuit.
    unsigned cost () const noexcept { return cost_; }

    // Send a data packet to one neighbour on this circuit.  A point to
    // point circuit has only one, so it ignores which; a LAN circuit
    // addresses the frame to that neighbour.
    virtual bool send_to (ShortData &pkt, const Adjacency &adj) = 0;

    // An adjacency's listen timer expired: the neighbour has gone quiet.
    virtual void adj_timeout (Adjacency *adj) = 0;

    // ------------------------------------------- the update process needs
    // Send an already encoded routing message.  A point to point circuit
    // sends it to its one neighbour; a LAN circuit multicasts it to all
    // routers.
    virtual void send_update (const Bytes &frame) = 0;

    // Is there anybody on this circuit who wants routing messages of this
    // level?  Level 1 messages go to any router, level 2 only to area
    // routers.
    virtual bool wants_updates (unsigned level) const = 0;

    // The largest routing message this circuit should build.
    virtual std::uint16_t update_blksize () const = 0;

protected:
    std::string name_;
    unsigned    cost_ = 4;
};

}   // namespace decnet::routing

#endif  // DECNET_ROUTING_CIRCUIT_H
