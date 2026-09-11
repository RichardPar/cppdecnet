// decnet/routing/ptp.h -- the point to point routing circuit.
//
// Port of route_ptp.PtpCircuit: the routing sublayer for a non-Ethernet
// circuit.  The state machine is the one in the Phase IV routing spec,
// with the three deviations pydecnet documents:
//
//  1. Circuit up and down notification is synchronous, so the states that
//     exist only to deliver those notifications are omitted.
//  2. No timeout in the DS state.  Data links do not fail separately here
//     and the datalink guarantees it will report when it is ready, so a
//     timeout would only let the two layers' timeouts fight each other.
//     doc/internals.txt has the reasoning.
//  3. Multinet in UDP mode cannot report a remote restart, so the port
//     says so through start_works().
//
// States, with pydecnet's labels:
//
//   ha  "Halted"          -- a Start item opens the datalink
//   ds  "Datalink started"-- waiting for the datalink to come up
//   ri  "Routing init"    -- init sent, waiting for the neighbour's
//   rv  "Routing verify"  -- waiting for a verification message
//   ru  "Running"         -- the adjacency is up; hellos flow
//
// PORT: Phase II neighbours (Node Init/Verify, the intercept machinery and
// the ru2 state) are not ported.  Nor are the router substates: pydecnet
// splits Running into ru4l2/ru4l1/ru4e/ru3r/ru3e purely to control which
// packet types each accepts, which only matters once this node can be a
// router.  One running state covers the endnode case.

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

    // Send a data packet to the neighbour.  Returns false when the circuit
    // is not up, or when the neighbour is an endnode and the packet is not
    // addressed to it.  Port of PtpCircuit.send.
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

    // The NICE substate for the circuit's current state, or -1 when there
    // is none.  pydecnet attaches these to the state functions with
    // @setcode: ha and ds are "Synchronizing", ri and rv are "Starting",
    // and ru -- Running -- has no substate at all, which is what tells a
    // circuit read that there is a neighbour worth naming.
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

    // Answer the part of a NICE read this circuit knows about.  For a node
    // read that is the neighbour at the far end; for a circuit read it is
    // the circuit's own state.  Port of PtpCircuit.nice_read.
    void nice_read (const nice::NiceRequest &req, nice::ReplyDict &resp,
                    const Nodeid *adj_qual = nullptr);

private:
    // Send a packet straight to the datalink.
    void dlsend (const RoutingPacketBase &pkt);

    // Give up on the current neighbour and start over.  Returns the ds
    // state, as PtpCircuit.restart does.
    State restart (const char *why);
    // The same, but reporting an event first.  Which event and which
    // reason depends on what went wrong, and the caller is the only one
    // that knows.
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

    // The received packet.  validate() decodes it once, before the state
    // action runs, so each state works with a typed packet rather than
    // repeating the parse -- which is what PtpCircuit.validate does.
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
