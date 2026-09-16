// decnet/routing/ptp.h -- point to point routing circuit.
//
// Port of route_ptp.PtpCircuit.  The Phase IV routing spec state machine
// with PyDECnet's three deviations:
//
//  1. Circuit up/down notification is synchronous, so the notification
//     states are omitted.
//  2. No timeout in ds; the datalink reports when it is ready.
//  3. Multinet over UDP cannot report a remote restart (start_works()).
//
// States:
//
//   ha  "Halted"           Start opens the datalink
//   ds  "Datalink started" waiting for the datalink
//   ri  "Routing init"     init sent, waiting for the neighbour's
//   rv  "Routing verify"   waiting for verification
//   ru  "Running"          adjacency up
//
// PORT: Phase II neighbours (Node Init/Verify, intercept, ru2) and the
// router running substates (ru4l2, ru4l1, ru4e, ru3r, ru3e), which only
// restrict accepted packet types.

#ifndef DECNET_ROUTING_PTP_H
#define DECNET_ROUTING_PTP_H

#include "decnet/common/statemachine.h"
#include "decnet/events/events.h"
#include "decnet/datalink/datalink.h"
#include "decnet/routing/adjacency.h"
#include "decnet/routing/circuit.h"
#include "decnet/routing/packets.h"

namespace decnet::routing {

class BaseRouter;

// Requests from the control sublayer, as work items so they serialise with
// the state machine.  Ports of route_ptp.Start/Stop/CircuitDown.
class CircuitStart : public Work {
public:
    explicit CircuitStart (Element *o) noexcept : Work (o) {}
    const char *kind () const noexcept override { return "CircuitStart"; }
};

class CircuitStop : public Work {
public:
    explicit CircuitStop (Element *o) noexcept : Work (o) {}
    const char *kind () const noexcept override { return "CircuitStop"; }
};

class CircuitDown : public Work {
public:
    explicit CircuitDown (Element *o) noexcept : Work (o) {}
    const char *kind () const noexcept override { return "CircuitDown"; }
};

class PtpCircuit : public Circuit, public StateMachine<PtpCircuit> {
public:
    PtpCircuit (BaseRouter *parent, std::string name,
                datalink::Datalink *dl, const CircuitConfig &config);

    static constexpr const char *class_name = "PtpCircuit";

    void start ();
    void stop ();

    // Send a data packet to the neighbour.  Returns false if the circuit is
    // down, or the neighbour is an endnode and the packet is not for it.
    // Port of PtpCircuit.send.
    bool send (ShortData &pkt);

    // Send an already encoded packet, which is what the update process
    // hands us: it builds routing messages itself.
    void send_raw (const Bytes &frame);

    // There is only one neighbour, so which adjacency is immaterial.
    bool send_to (ShortData &pkt, const Adjacency &) override
    { return send (pkt); }

    void send_update (const Bytes &frame) override { send_raw (frame); }

    bool wants_updates (unsigned level) const override
    {
        unsigned nt = info_.ntype;
        if (nt != L1ROUTER && nt != L2ROUTER) return false;
        // Level 1 data does not go out of area, and only an area router
        // wants level 2 data.
        if (level == 2) return nt == L2ROUTER;
        return true;
    }

    std::uint16_t update_blksize () const override { return info_.blksize; }

    // What the circuit knows about the far end.  Zero id means not yet up.
    Nodeid neighbour () const noexcept { return info_.id; }
    unsigned neighbour_type () const noexcept { return info_.ntype; }
    unsigned neighbour_phase () const noexcept { return info_.rphase; }
    std::uint16_t blksize () const noexcept { return info_.blksize; }
    bool running () const noexcept;

    // NICE substate for the current state, or -1.  ha and ds are
    // "Synchronizing", ri and rv "Starting", ru has none.
    int nice_substate () const noexcept;

    const AdjacencyPtr &adjacency () const noexcept { return adj_; }

    void dispatch (Work &w) override { StateMachine<PtpCircuit>::dispatch (w); }
    bool validate (Work &w);

    // ------------------------------------------------------------- states
    State s0 (Work &w);            // "ha", the initial state
    State ds (Work &w);
    State ri (Work &w);
    State rv (Work &w);
    State ru (Work &w);

    std::string statename () const override;

    void timeout () override;
    Element *timer_owner () noexcept override { return this; }

    // Called by the adjacency when its listen timer expires.
    void adj_timeout (Adjacency *adj) override;

    double t3 () const noexcept { return t3_; }

    // NICE read: the neighbour for node reads, circuit state for circuit
    // reads.  Port of PtpCircuit.nice_read.
    void nice_read (const nice::NiceRequest &req, nice::ReplyDict &resp,
                    const Nodeid *adj_qual = nullptr);

private:
    // Send a packet straight to the datalink.
    void dlsend (const RoutingPacketBase &pkt);

    // Give up on the current neighbour and start over.  Returns the ds
    // state, as PtpCircuit.restart does.
    State restart (const char *why);
    // Same, reporting an event first.
    State restart (const char *why, events::EventId ev, int reason);

    // Raise a class 4 event about this circuit, naming the neighbour if we
    // have one.  reason < 0 leaves the reason parameter out.
    void routeevent (events::EventId ev, int reason = -1,
                     Bytes packet_beginning = { });

    // Clear everything learned about the neighbour.  Port of r_neigh.
    void clear_neighbour ();

    // Build the init message for our own phase and node type.
    void build_initmsg ();

    // The adjacency is complete: bring it up and start the hello timer.
    void up ();
    void down ();

    void send_hello ();

    // Is the source address in a received control packet the one we agreed
    // on?  Port of checksrc.
    bool check_src (Nodeid src) const noexcept;

    // Put our own address into a packet in the form the neighbour expects.
    void set_src (Nodeid &field) const noexcept;

    // The received packet, decoded once by validate() before the state action.
    std::unique_ptr<RoutingPacketBase> decoded_;
    RoutingPacketBase                 *packet_ = nullptr;

    BaseRouter         *parent_;
    datalink::Port     *port_;
    double              t3_;
    Bytes               verify_;        // the value we must send, if asked
    Bytes               expect_verify_; // the value we require, if we asked
    bool                request_verify_ = false;

    AdjacencyInfo       info_;
    AdjacencyPtr        adj_;

    PtpInit             initmsg_;
    PtpHello            hellomsg_;
};

}   // namespace decnet::routing

#endif  // DECNET_ROUTING_PTP_H
