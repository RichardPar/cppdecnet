// decnet/routing/lan.h -- the routing sublayer for broadcast circuits.
//
// Port of route_eth.py.  A LAN circuit has no handshake and no state
// machine: every node multicasts a hello periodically and neighbours are
// learned by listening.  Two consequences make up most of this file.
//
// Two-way visibility has to be proved.  Hearing a router's hello only
// shows it can reach us.  Each router lists in its hello every router it
// can hear, so seeing our own address in a neighbour's list is what
// promotes that adjacency from INIT to UP.
//
// Somebody has to be the designated router.  Endnodes send everything they
// cannot address directly to one router, so the routers elect one by
// priority with ties broken by address.  The winner waits DRDELAY before
// acting on it, so a brief disagreement at startup does not leave two
// nodes both behaving as DR.
//
// PORT: Phase III and Phase II nodes on a LAN are not handled; neither is
// the router-priority/maximum-routers table overflow event.

#ifndef DECNET_ROUTING_LAN_H
#define DECNET_ROUTING_LAN_H

#include "decnet/common/timers.h"
#include "decnet/events/events.h"
#include "decnet/datalink/bc.h"
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

// The previous hop cache an endnode keeps: which MAC address last
// delivered traffic from a given source node.  Sending back that way
// avoids bouncing every reply off the designated router.  Port of
// route_eth.NiCacheEntry.
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

protected:
    // Strip any padding, decode, and hand the packet to the subclass.
    // Returns null for anything to be ignored.
    std::unique_ptr<RoutingPacketBase> decode (const Bytes &frame) const;

    virtual void send_hello () = 0;
    virtual void handle (RoutingPacketBase &pkt, Macaddr src) = 0;

    // Bring an adjacency up and register it with the routing layer, or
    // take one down.  Shared by both circuit kinds.
    // Raise a class 4 event about this circuit and one neighbour on it.
    void lanevent (events::EventId ev, Nodeid neighbour, int reason = -1);

    void adjacency_up (std::uint16_t key, const AdjacencyInfo &info);
    void adjacency_down (std::uint16_t key);

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

    std::size_t cache_size () const noexcept { return cache_.size (); }

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

    std::uint8_t priority () const noexcept { return prio_; }
    bool is_dr () const noexcept { return isdr_; }
    // Who we currently believe is the designated router.
    Nodeid designated_router () const noexcept { return dr_; }

    // Is this neighbour confirmed two-way?
    bool two_way (Nodeid id) const;

protected:
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
