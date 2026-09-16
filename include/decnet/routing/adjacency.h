// decnet/routing/adjacency.h -- routing adjacencies.
//
// Port of adjacency.py.  An adjacency holds a neighbour's address, type,
// block size and version, and the listen timer that takes it down when the
// neighbour goes silent.
//
// Adjacencies are shared_ptr: the circuit, the routing table and pending
// work items all refer to them.

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
    // Only meaningful on a broadcast circuit, where it decides the
    // designated router election; zero everywhere else.
    unsigned      priority () const noexcept { return info_.priority; }

    // The cost of the circuit this adjacency is on; zero for the self
    // adjacency, which costs nothing to reach.
    unsigned circuit_cost () const noexcept;

    // Bring the adjacency up: start the listen timer and notify routing.
    // Phase II neighbours have no listen timer.
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

// This node as column 0 of the routing matrix, so packets for us resolve
// to an adjacency like any other.  Port of routing.SelfAdj.
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
