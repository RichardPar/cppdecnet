// decnet/datalink/ptp.h -- point to point datalink base class.
//
// Port of datalink.PtpDatalink and PtpPort.  A receive thread does
// blocking I/O and posts work items; the state machine runs on the node
// thread.
//
// States:
//
//   s0           "Halted"        Start connects and starts the thread
//   connecting   "Connecting"    waiting for the thread to report Connected
//   running                      supplied by the concrete datalink
//   reconnecting "Reconnecting"  wait for the thread, then hold off and retry
//   shutdown     "Shutdown"      wait for the thread, then stay down
//
// The routing layer has no timeout while waiting for the datalink, so this
// layer must keep retrying and report when it comes up.

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

    // Common handling for every state, before the state action.  Virtual so
    // DDCMP can restart its protocol instead of its connection.
    virtual bool validate (Work &w);

    // ------------------------------------------------------------- states
    State s0 (Work &w);
    State connecting (Work &w);
    State reconnecting (Work &w);
    State shutdown_state (Work &w);

    // The running state, supplied by the concrete datalink.
    virtual State running (Work &w) = 0;

    std::string statename () const override;

    bool is_up () const noexcept { return is_up_; }

protected:
    // Report to the port owner.  Both are idempotent, as in PyDECnet.
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

    // Socket used by the receive thread.  Written on the node thread only
    // while the receive thread is not running.
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
