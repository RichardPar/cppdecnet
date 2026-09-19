// decnet/routing/circuit.h -- routing circuit interface.
//
// Common base for point to point and LAN circuits, so one Adjacency type
// works with both.

#ifndef DECNET_ROUTING_CIRCUIT_H
#define DECNET_ROUTING_CIRCUIT_H

#include "decnet/common/element.h"
#include "decnet/routing/packets.h"

#include <chrono>
#include <string>

namespace decnet::routing {

class Adjacency;

// The routing layer's own counters for one circuit.  PyDECnet keeps these
// on the datalink object; here they belong to the layer that maintains
// them, and the datalink keeps its own traffic counters separately.
//
// "Terminating" is traffic for this node, "originating" traffic from it,
// and "transit" traffic passing through.  A packet both originating and
// terminating here crosses no circuit and is counted nowhere, as PyDECnet
// notes in SelfAdj.send.
struct CircuitCounters {
    std::uint64_t term_recv = 0, orig_sent = 0;
    std::uint64_t trans_recv = 0, trans_sent = 0;
    std::uint64_t cir_down = 0, init_fail = 0, adj_down = 0;
    std::uint64_t peak_adj = 0;

    // When the circuit last came up.  Unset until it does, which is what
    // distinguishes "never up" from "up just now".
    std::chrono::steady_clock::time_point last_up {};

    void up_now () noexcept { last_up = std::chrono::steady_clock::now (); }
    bool ever_up () const noexcept
    { return last_up != std::chrono::steady_clock::time_point {}; }

    // Seconds since the circuit last came up, saturating at the two byte
    // counter's ceiling.  Meaningless unless ever_up().
    unsigned seconds_since_up () const noexcept;
};

class Circuit : public Element {
public:
    Circuit (Element *parent, std::string name) noexcept
        : Element (parent), name_ (std::move (name)) {}

    const std::string &name () const noexcept { return name_; }

    // The routing counters for this circuit.  Every layer that can change
    // one reaches it through here.
    CircuitCounters &counters () noexcept { return counters_; }
    const CircuitCounters &counters () const noexcept { return counters_; }

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
    std::string     name_;
    unsigned        cost_ = 4;
    CircuitCounters counters_;
};

}   // namespace decnet::routing

#endif  // DECNET_ROUTING_CIRCUIT_H
