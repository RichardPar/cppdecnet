// decnet/routing/routing.h -- the routing layer.
//
// Port of routing.BaseRouter and routing.EndnodeRouting.  The router owns
// the circuits and the adjacency table, and decides what to do with a
// packet that arrives from one of them.
//
// Only the endnode lives here.  L1Router and L2Router, which add the
// routing tables, the forwarding decision and the routing message
// exchange, are in l1router.h.

#ifndef DECNET_ROUTING_ROUTING_H
#define DECNET_ROUTING_ROUTING_H

#include "decnet/common/element.h"
#include "decnet/routing/adjacency.h"
#include "decnet/routing/packets.h"

#include <map>
#include <memory>
#include <vector>

namespace decnet {
class Config;
namespace datalink { class DatalinkLayer; }
namespace nsp { class NSP; }
}

namespace decnet::routing {

class PtpCircuit;
class LanCircuit;

class BaseRouter : public Element {
public:
    BaseRouter (Element *parent, const Config &config);
    ~BaseRouter () override;

    // Virtual: a router does more at startup than an endnode -- it has to
    // establish its own column in the routing matrix before any route can
    // be computed.
    virtual void start ();
    virtual void stop ();

    Nodeid   nodeid () const noexcept { return nodeid_; }
    unsigned homearea () const noexcept { return nodeid_.area (); }
    unsigned tid () const noexcept { return nodeid_.tid (); }
    Phase    phase () const noexcept { return phase_; }
    const std::string &name () const noexcept { return name_; }

    // Our own type code, as it goes into an init message.
    virtual std::uint8_t ntype () const noexcept = 0;
    virtual Version tiver () const noexcept { return tiver_ph4; }

    unsigned maxnodes () const noexcept { return maxnodes_; }
    unsigned maxarea  () const noexcept { return maxarea_; }

    // Adjacency table maintenance, called by the circuits.  A router
    // overrides these to keep its routing table in step.
    virtual void adj_up (const AdjacencyPtr &adj);
    virtual void adj_down (const AdjacencyPtr &adj);
    std::size_t adjacency_count () const noexcept { return adjacencies_.size (); }
    AdjacencyPtr find_adjacency (Nodeid id) const;

    // A data packet arrived from a circuit and is ours to deal with: for
    // an endnode that means accept or drop, for a router it means route.
    virtual void forward (ShortData &pkt) = 0;

    // The packet is addressed to this node: hand it up.  Called through
    // the self adjacency, so that a packet for us resolves to an output
    // adjacency like any other.
    virtual void deliver (ShortData &pkt);

    // Originate a packet carrying NSP data.  Each node type reaches the
    // wire differently -- an endnode through its one circuit, a router
    // through its routing table -- so this is virtual.
    virtual void send_nsp (const Bytes &data, Nodeid dest, bool rqr = false) = 0;

    // Where a packet for this node goes.  NSP registers itself here.
    void set_nsp (nsp::NSP *n) noexcept { nsp_ = n; }

    // A routing message arrived.  An endnode never sees one.
    virtual void routing_message (const RoutingMessage &, Adjacency *,
                                  unsigned) {}

protected:
    // Create the routing circuits.  This is deliberately NOT called from
    // this class's constructor: building a circuit asks the router for its
    // node type, which is virtual, and during a base class constructor
    // that call would land on the pure virtual.  Each concrete router
    // calls this from its own constructor body, where the vtable is its
    // own.
    void init_circuits (const Config &config);

public:

    void dispatch (Work &) override {}

    const std::vector<PtpCircuit *> &circuits () const noexcept
    { return circuit_order_; }
    PtpCircuit *circuit (const std::string &name) const;

    // Broadcast circuits are kept separately: they are a different class
    // with no state machine, and most callers want one kind or the other.
    const std::vector<LanCircuit *> &lan_circuits () const noexcept
    { return lan_order_; }
    LanCircuit *lan_circuit (const std::string &name) const;

    // Where a packet addressed to this node goes.  Until NSP exists there
    // is nowhere to send it, so the router counts it and drops it; the
    // count is what the tests look at.
    std::uint64_t packets_for_us () const noexcept { return for_us_; }

protected:
    Nodeid      nodeid_;
    Phase       phase_ = Phase::ph4;
    std::string name_;
    unsigned    maxnodes_ = 1023;
    unsigned    maxarea_ = 63;
    std::uint64_t for_us_ = 0;
    nsp::NSP     *nsp_ = nullptr;

    std::map<std::string, std::unique_ptr<PtpCircuit>> circuits_;
    std::vector<PtpCircuit *>                          circuit_order_;
    std::map<std::string, std::unique_ptr<LanCircuit>> lan_circuits_;
    std::vector<LanCircuit *>                          lan_order_;
    std::map<std::uint16_t, AdjacencyPtr>              adjacencies_;
};

// Routing for a Phase IV endnode.  Port of routing.EndnodeRouting.
class EndnodeRouting : public BaseRouter {
public:
    EndnodeRouting (Element *parent, const Config &config);

    std::uint8_t ntype () const noexcept override { return ENDNODE; }

    // Send NSP data to a destination.  rqr asks for return to sender if the
    // packet cannot be delivered, which NSP sets on connect initiates.
    void send (Bytes data, Nodeid dest, bool rqr = false);
    void send_nsp (const Bytes &data, Nodeid dest, bool rqr = false) override
    { send (data, dest, rqr); }

    // A received data packet goes up to NSP if it is for us, and is
    // dropped otherwise: an endnode does not forward.
    void forward (ShortData &pkt) override;
};

// Build the routing layer the configuration calls for.
std::unique_ptr<BaseRouter> make_router (Element *parent, const Config &config);

}   // namespace decnet::routing

#endif  // DECNET_ROUTING_ROUTING_H
