// decnet/routing/routing.h -- the routing layer.
//
// Port of routing.BaseRouter and routing.EndnodeRouting.  Owns the
// circuits and adjacency table.  L1Router and L2Router are in l1router.h.

#ifndef DECNET_ROUTING_ROUTING_H
#define DECNET_ROUTING_ROUTING_H

#include "decnet/common/element.h"
#include "decnet/nice/nml.h"
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

    // Virtual: routers set up their routing matrix column.
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

    // Deliver a packet addressed to this node.  Called via the self adjacency.
    virtual void deliver (ShortData &pkt);

    // Send an NSP packet.  Endnodes use their circuit, routers the routing
    // table.
    virtual void send_nsp (const Bytes &data, Nodeid dest, bool rqr = false) = 0;

    // Where a packet for this node goes.  NSP registers itself here.
    void set_nsp (nsp::NSP *n) noexcept { nsp_ = n; }

    // NICE read for node reachability, circuits and areas.  Port of
    // BaseRouter.nice_read.
    virtual void nice_read (const nice::NiceRequest &req, nice::ReplyDict &resp);

    // Add the executor counters this router keeps to its node reply.  A
    // node with no routing table has none, which is why this is virtual and
    // empty here.  Port of the router-only half of ExecCounters.
    virtual void nice_counters (nice::NiceReply &) {}

    // A routing message arrived.  An endnode never sees one.
    virtual void routing_message (const RoutingMessage &, Adjacency *,
                                  unsigned) {}

protected:
    // Fill in what this node reports about one node address: whether it is
    // reachable and by what route.  Port of BaseRouter.read_node.
    virtual void read_node (const nice::NiceRequest &req, Nodeid id,
                            nice::ReplyDict &resp);

    // Add this node type's characteristics to an executor reply.  Each
    // router subclass adds its own.  Port of BaseRouter.node_char.
    virtual void node_char (nice::NiceReply &r);

    // Report every reachable node, for a plural request.  An endnode has no
    // routing table, so the base does nothing.  Port of BaseRouter.reach.
    virtual void reach (const nice::NiceRequest &req, nice::ReplyDict &resp,
                        const std::string *circuit_qual);

    // Create the routing circuits.  Called from each concrete router's
    // constructor, not from here, because circuits query the virtual node
    // type.
    void init_circuits (const Config &config);

public:

    void dispatch (Work &) override {}

    // Every adjacency this node has, keyed as the routing layer keys them.
    // Exposed for network management and the monitoring pages.
    const std::map<std::uint16_t, AdjacencyPtr> &adjacencies () const noexcept
    { return adjacencies_; }

    const std::vector<PtpCircuit *> &circuits () const noexcept
    { return circuit_order_; }
    PtpCircuit *circuit (const std::string &name) const;

    // Broadcast circuits are kept separately: they are a different class
    // with no state machine, and most callers want one kind or the other.
    const std::vector<LanCircuit *> &lan_circuits () const noexcept
    { return lan_order_; }
    LanCircuit *lan_circuit (const std::string &name) const;

    // Packets addressed to this node with nowhere to deliver them are counted
    // and dropped.
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
