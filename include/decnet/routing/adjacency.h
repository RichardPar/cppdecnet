// decnet/routing/adjacency.h -- routing layer adjacencies.
//
// Port of adjacency.py.  An adjacency is what the routing layer knows
// about one neighbour: its address, type, block size and version, plus the
// listen timer that takes it down if the neighbour stops talking.
//
// Ownership is where this differs from the Python.  An adjacency is
// referenced by its circuit, by the routing layer's table and by in-flight
// work items at once, so it is held by shared_ptr.  This is the case where
// Python's collector was doing real work for us.

#ifndef DECNET_ROUTING_ADJACENCY_H
#define DECNET_ROUTING_ADJACENCY_H

#include "decnet/common/element.h"
#include "decnet/common/timers.h"
#include "decnet/routing/circuit.h"
#include "decnet/routing/packets.h"

#include <memory>

namespace decnet::routing {

class BaseRouter;

// What a circuit learned about its neighbour, gathered from the init
// message.  Port of the attributes Adjacency reads off its "info" argument.
struct AdjacencyInfo {
    Nodeid        id;
    std::uint8_t  ntype = UNKNOWN;
    std::uint16_t blksize = MTU;
    Version       tiver;
    unsigned      timer = 0;       // the neighbour's hello timer, if it sent one
    unsigned      priority = 0;    // broadcast circuits only
    unsigned      rphase = 4;
};

class Adjacency : public Element, public Timer,
                  public std::enable_shared_from_this<Adjacency> {
public:
    Adjacency (Circuit *circuit, const AdjacencyInfo &info,
               double circuit_t3);

    Circuit *circuit () const noexcept { return circuit_; }

    // The neighbour's LAN address, derived from its node number the way
    // Phase IV prescribes.  Port of Adjacency.macid.
    Macaddr macid () const noexcept { return Macaddr::from_nodeid (info_.id); }

    Nodeid        nodeid () const noexcept { return info_.id; }
    std::uint8_t  ntype  () const noexcept { return info_.ntype; }
    std::uint16_t blksize () const noexcept { return info_.blksize; }
    Version       tiver  () const noexcept { return info_.tiver; }
    double        listen_time () const noexcept { return t4_; }

    // The cost of the circuit this adjacency is on; zero for the self
    // adjacency, which costs nothing to reach.
    unsigned circuit_cost () const noexcept;

    // Bring the adjacency up: start the listen timer and tell the routing
    // layer.  Phase II neighbours are exempt from the timer, since they are
    // not required to send anything periodically.
    void up ();
    void down ();

    // Restart the listen timer; called for every packet received.
    void alive ();

    // Send a packet to this neighbour.
    virtual void send (const RoutingPacketBase &pkt);

    // True for the pseudo-adjacency that stands for this node itself.
    virtual bool is_self () const noexcept { return false; }

    void dispatch (Work &) override {}

    // The listen timer expired: the neighbour has gone quiet.
    void timeout () override;
    Element *timer_owner () noexcept override { return this; }

protected:
    Circuit      *circuit_;
    AdjacencyInfo info_;
    double        t4_;             // listen timeout
    bool          up_ = false;
};

// The routing architecture shows this node itself as column 0 of the
// routing matrix, so that a packet addressed to us resolves to an output
// adjacency like any other destination.  Port of routing.SelfAdj.
class SelfAdjacency : public Adjacency {
public:
    SelfAdjacency (BaseRouter *router, Nodeid id, std::uint8_t ntype);

    bool is_self () const noexcept override { return true; }

    // "Sending" to ourselves means delivering up to the layer above.
    void send (const RoutingPacketBase &pkt) override;

    void timeout () override {}     // never times out

private:
    BaseRouter *router_;
};

using AdjacencyPtr = std::shared_ptr<Adjacency>;

}   // namespace decnet::routing

#endif  // DECNET_ROUTING_ADJACENCY_H
