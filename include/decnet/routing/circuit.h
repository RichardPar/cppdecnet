// decnet/routing/circuit.h -- routing circuit interface.
//
// Common base for point to point and LAN circuits, so one Adjacency type
// works with both.

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

    // Send a data packet to a neighbour.  Point to point circuits ignore the
    // neighbour argument.
    virtual bool send_to (ShortData &pkt, const Adjacency &adj) = 0;

    // An adjacency's listen timer expired: the neighbour has gone quiet.
    virtual void adj_timeout (Adjacency *adj) = 0;

    // ------------------------------------------- the update process needs
    // Send an encoded routing message: to the neighbour on point to point, to
    // all routers on a LAN.
    virtual void send_update (const Bytes &frame) = 0;

    // Does anyone on this circuit want routing messages of this level?  Level
    // 1 goes to any router, level 2 to area routers.
    virtual bool wants_updates (unsigned level) const = 0;

    // The largest routing message this circuit should build.
    virtual std::uint16_t update_blksize () const = 0;

protected:
    std::string name_;
    unsigned    cost_ = 4;
};

}   // namespace decnet::routing

#endif  // DECNET_ROUTING_CIRCUIT_H
