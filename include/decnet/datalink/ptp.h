// decnet/datalink/ptp.h -- point to point datalink base class.
//
// Port of datalink.PtpDatalink and PtpPort.  This is the state machine that
// every point to point circuit runs: it owns a receive thread doing
// blocking I/O, and turns everything that thread sees into work items so
// the states run on the node thread.
//
// States, with pydecnet's labels:
//
//   s0           "Halted"        a Start item connects and starts the thread
//   connecting   "Connecting"    waiting for the thread to report Connected
//   running                      supplied by the concrete datalink
//   reconnecting "Reconnecting"  wait for the thread, then hold off and retry
//   shutdown     "Shutdown"      wait for the thread, then stay down
//
// The design note in doc/internals.txt is worth keeping in mind here: the
// routing layer has no timeout in its datalink-start state, because this
// layer guarantees it will keep retrying forever and will always report
// when it finally comes up.  That guarantee is what the reconnecting state
// with its backoff implements.

#ifndef DECNET_DATALINK_PTP_H
#define DECNET_DATALINK_PTP_H

#include "decnet/common/backoff.h"
#include "decnet/common/socket.h"
#include "decnet/common/statemachine.h"
#include "decnet/datalink/datalink.h"

#include <atomic>
#include <thread>

namespace decnet::datalink {

class PtpDatalink;

// The port for a point to point circuit.  Only one may exist at a time,
// since there is no multiplexing.  Port of datalink.PtpPort.
class PtpPort : public Port {
public:
    PtpPort (PtpDatalink *dl, Element *owner) noexcept;

    void send (Bytes msg) override;
    bool start_works () const noexcept override;
};

class PtpDatalink : public Datalink, public StateMachine<PtpDatalink> {
public:
    PtpDatalink (Element *owner, std::string name);
    ~PtpDatalink () override;

    static constexpr const char *class_name = "PtpDatalink";

    Port *create_port (Element *owner) override;
    const PtpCounters *counters () const noexcept override { return &counters_; }
    PtpCounters &mutable_counters () noexcept { return counters_; }

    // Transmit one frame.  Called on the node thread.
    virtual void send (Bytes msg) = 0;

    // Queue a Reconnect request.
    void reconnect (bool now = false);

    // Work items reach the state machine through here.
    void dispatch (Work &w) override { StateMachine<PtpDatalink>::dispatch (w); }

    // Common handling done in every state, before the state action.
    bool validate (Work &w);

    // ------------------------------------------------------------- states
    State s0 (Work &w);
    State connecting (Work &w);
    State reconnecting (Work &w);
    State shutdown_state (Work &w);

    // The running state, supplied by the concrete datalink.  It is a
    // virtual member of this class rather than of the subclass so that a
    // pointer to member of PtpDatalink still reaches the override -- which
    // is what lets one state machine base serve every datalink type.
    virtual State running (Work &w) = 0;

    std::string statename () const override;

    bool is_up () const noexcept { return is_up_; }

protected:
    // Report to the port owner.  Both are idempotent, as in pydecnet.
    void report_up ();
    void report_down ();

    // ------------------------------------------- concrete datalink hooks
    // Create sockets and start connecting.  Runs on the node thread.
    virtual void connect () = 0;

    // Close whatever connect() opened.  Runs on the node thread.
    virtual void disconnect () = 0;

    // Runs on the receive thread, after connect().  Return true once there
    // is a usable connection, false to give up.  Connectionless datalinks
    // return true immediately.
    virtual bool check_connection () = 0;

    // Runs on the receive thread once check_connection succeeded.  Receives
    // frames and posts them as Received work items until the connection
    // fails or a stop is requested.
    virtual void receive_loop () = 0;

    // Called on the node thread when the thread reports Connected; returns
    // the next state.  Multinet goes straight to running; a real datalink
    // would start its initialisation protocol here.
    virtual State connected () = 0;

    // ------------------------------------------- receive thread services
    // Read exactly n bytes, or throw std::runtime_error if the connection
    // is lost or a stop is requested.  Port of PtpDatalink.recvall.
    Bytes recvall (std::size_t n);

    bool stopping () const noexcept { return stopnow_.load (); }

    // The socket the receive thread works on.  Written on the node thread
    // during connect and disconnect, read on the receive thread; both
    // happen only while the other side is quiescent, which the state
    // machine guarantees.
    Socket socket_;

    PtpPort    *port_ = nullptr;
    PtpCounters counters_;
    Backoff     restart_timer_ { 2.0, 120.0 };

private:
    void start_thread ();
    void stop_thread (bool wait);
    void run ();                 // the receive thread body

    // Common actions for Stop and Reconnect, shared by every state.
    State handle_stop ();
    State handle_reconnect (const Reconnect &r);

    std::unique_ptr<PtpPort> owned_port_;
    std::thread              thread_;
    std::atomic<bool>        stopnow_ { false };
    std::atomic<bool>        thread_running_ { false };
    bool                     is_up_ = false;
    bool                     restart_now_ = false;
};

}   // namespace decnet::datalink

#endif  // DECNET_DATALINK_PTP_H
