// decnet/routing/lan.h -- routing on broadcast circuits.
//
// Port of route_eth.py.  Nodes send periodic multicast hellos and
// neighbours are learned by listening.
//
// Router adjacencies go from INIT to UP when a router sees its own address
// in the neighbour's hello router list.
//
// Routers elect a designated router by priority, then address.  The winner
// waits DRDELAY before acting as DR.
//
// PORT: Phase II and III nodes on a LAN, and the router table overflow
// event.

#ifndef DECNET_ROUTING_LAN_H
#define DECNET_ROUTING_LAN_H

#include "decnet/common/timers.h"
#include "decnet/events/events.h"
#include "decnet/datalink/bc.h"
#include "decnet/nice/nml.h"
#include "decnet/routing/adjacency.h"
#include "decnet/routing/circuit.h"
#include "decnet/routing/packets.h"

#include <map>

namespace decnet {
struct CircuitConfig;
}

namespace decnet::routing {

class BaseRouter;

// An adjacency on a LAN is either heard-but-unconfirmed or confirmed
// two-way.  Port of the INIT and UP states route_eth assigns.
enum class AdjState { init, up };

// What a LAN circuit knows about one neighbour.
struct LanAdjacency {
    AdjacencyPtr adj;
    Macaddr      macaddr;
    AdjState     state = AdjState::init;
    std::uint8_t prio = 0;
    std::uint8_t ntype = ENDNODE;
    double       listen_time = 0;
};

// Endnode previous hop cache: the MAC address that last delivered traffic
// from a source node.  Port of route_eth.NiCacheEntry.
struct CacheEntry {
    Macaddr                               prevhop;
    std::chrono::steady_clock::time_point expires;
};

// Common behaviour for a broadcast routing circuit.
class LanCircuit : public Circuit, public Timer {
public:
    LanCircuit (BaseRouter *parent, std::string name,
                datalink::BcDatalink *dl, const CircuitConfig &config);
    ~LanCircuit () override;

    double t3 () const noexcept { return t3_; }

    virtual void start ();
    virtual void stop ();

    // Send a data packet to a specific next hop.  A LAN has no notion of
    // "unreachable", so this always succeeds.
    void send_to_mac (ShortData &pkt, Macaddr nexthop);

    // Designated router and our election priority.  An endnode reports its
    // chosen router; a router reports the election winner, possibly itself.
    virtual Nodeid designated_router () const noexcept { return Nodeid (); }

    // Zero for an endnode, which does not stand in the election.
    virtual std::uint8_t priority () const noexcept { return 0; }

    // The Circuit spelling: address the frame to that neighbour.
    bool send_to (ShortData &pkt, const Adjacency &adj) override;

    // A neighbour stopped sending hellos.  Drop it.
    void adj_timeout (Adjacency *adj) override;

    // Routing messages on a LAN are multicast to every router.
    void send_update (const Bytes &frame) override;
    bool wants_updates (unsigned level) const override;
    std::uint16_t update_blksize () const override { return ETHMTU; }

    void dispatch (Work &w) override;
    void timeout () override;
    Element *timer_owner () noexcept override { return this; }

    // How many neighbours are currently confirmed up.
    std::size_t adjacency_count () const;

    // What this circuit knows about its neighbours, for network management
    // and the monitoring pages.
    const std::map<std::uint16_t, LanAdjacency> &adjacencies () const noexcept
    { return adjacencies_; }

    unsigned cost () const noexcept { return cost_; }

    // Answer the part of a NICE read this circuit knows about.  Port of
    // LanCircuit.nice_read in route_eth.py.
    void nice_read (const nice::NiceRequest &req, nice::ReplyDict &resp,
                    const Nodeid *adj_qual = nullptr);

    // What a router adds to a circuit characteristics reply and an endnode
    // does not: the priority, and who the designated router is.
    virtual void nice_char (nice::NiceReply &r) const {}

protected:
    // Strip any padding, decode, and hand the packet to the subclass.
    // Returns null for anything to be ignored.
    std::unique_ptr<RoutingPacketBase> decode (const Bytes &frame) const;

    virtual void send_hello () = 0;
    virtual void handle (RoutingPacketBase &pkt, Macaddr src) = 0;

    // Bring an adjacency up and register it with routing, or take it down.
    // Raise a class 4 event for this circuit and a neighbour.
    void lanevent (events::EventId ev, Nodeid neighbour, int reason = -1);

    void adjacency_up (std::uint16_t key, const AdjacencyInfo &info);
    // Virtual: a router re-runs the designated router election.
    virtual void adjacency_down (std::uint16_t key);

    BaseRouter           *parent_;
    datalink::BcDatalink *datalink_;
    datalink::BcPort     *port_ = nullptr;
    double                t3_;
    unsigned              cost_ = 4;
    std::map<std::uint16_t, LanAdjacency> adjacencies_;
};

// A LAN circuit on an endnode.  Port of route_eth.EndnodeLanCircuit.
class EndnodeLanCircuit : public LanCircuit {
public:
    EndnodeLanCircuit (BaseRouter *parent, std::string name,
                       datalink::BcDatalink *dl, const CircuitConfig &config);

    // Originate a packet: use the cache if we have an entry, else the
    // designated router, else address the destination directly.
    bool send (ShortData &pkt, bool tryhard = false);

    // The router this endnode is using, if it has heard one.
    bool have_dr () const noexcept { return dr_.has_value (); }
    Nodeid dr () const noexcept
    { return dr_ ? dr_->first : Nodeid (); }
    Nodeid designated_router () const noexcept override { return dr (); }

    std::size_t cache_size () const noexcept { return cache_.size (); }

    // The designated router stopped sending hellos.  As well as dropping
    // the adjacency, forget the router itself.
    void adj_timeout (Adjacency *adj) override;

protected:
    void send_hello () override;
    void handle (RoutingPacketBase &pkt, Macaddr src) override;

private:
    void expire_cache ();

    std::optional<std::pair<Nodeid, Macaddr>> dr_;
    std::map<std::uint16_t, CacheEntry>       cache_;
};

// A LAN circuit on a router.  Port of route_eth.RoutingLanCircuit.
class RoutingLanCircuit : public LanCircuit {
public:
    RoutingLanCircuit (BaseRouter *parent, std::string name,
                       datalink::BcDatalink *dl, const CircuitConfig &config);

    void start () override;
    void stop () override;

    std::uint8_t priority () const noexcept override { return prio_; }
    bool is_dr () const noexcept { return isdr_; }
    // Who we currently believe is the designated router.
    Nodeid designated_router () const noexcept override { return dr_; }

    // Is this neighbour confirmed two-way?
    bool two_way (Nodeid id) const;

    void nice_char (nice::NiceReply &r) const override;

protected:
    // Losing a neighbour changes the election, and losing the designated
    // router means there is not one.
    void adjacency_down (std::uint16_t key) override;

    void send_hello () override;
    void handle (RoutingPacketBase &pkt, Macaddr src) override;

private:
    // Who should be DR, given what we can hear?  Returns true if it is us.
    bool best_dr (Nodeid &who) const;
    void calc_dr ();
    void become_dr ();

    // Build the router list for our hello: every router we can hear, and
    // whether we have confirmed two-way with it.
    Bytes build_elist (bool empty = false) const;

    std::uint8_t  prio_ = 64;
    unsigned      maxrouters_ = 16;
    bool          isdr_ = false;
    Nodeid        dr_;
    CallbackTimer drtimer_;
    bool          drtimer_running_ = false;
};

}   // namespace decnet::routing

#endif  // DECNET_ROUTING_LAN_H
