// decnet/datalink/multinet.h -- Multinet over TCP or UDP.
//
// Port of multinet.py.  Multinet has no datalink startup, so a remote
// restart is not visible.  The UDP form reports start_works() false.
//
// Four byte header then payload.  Over TCP the first two bytes are the
// payload length, little endian; over UDP they are a sequence number the
// receiver ignores.  The other two bytes are zero.
//
// Device syntax:
//
//     host[:port][:connect]    active end of a TCP connection
//     host[:port][:listen]     passive end of a TCP connection
//     host[:port][:localport]  UDP
//     [host]:port:server       passive end for any number of peers: each
//                              connection gets a circuit of its own
//
// The port defaults to 700.  In listen mode an empty host accepts any
// address.

#ifndef DECNET_DATALINK_MULTINET_H
#define DECNET_DATALINK_MULTINET_H

#include "decnet/datalink/ptp.h"

#include "decnet/config.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

namespace decnet::datalink {

// The parsed --device argument.
struct MultinetDevice {
    enum class Mode { connect, listen, udp, server };

    Mode          mode = Mode::udp;
    std::string   destination;      // empty means any, for listen
    std::uint16_t dest_port = 0;
    std::uint16_t source_port = 0;

    // Throws std::invalid_argument on a device string we cannot read.
    static MultinetDevice parse (const std::string &device);

    std::string str () const;
};

// Common behaviour for all three modes.
class Multinet : public PtpDatalink {
public:
    Multinet (Element *owner, std::string name, MultinetDevice dev,
              std::string source_host = "");

    static constexpr const char *class_name = "Multinet";

    const MultinetDevice &device () const noexcept { return dev_; }

    // Factory: builds the right subclass for the mode.
    static std::unique_ptr<Datalink> create (Element *owner,
                                             const std::string &name,
                                             const std::string &device,
                                             const std::string &source_host = "");

protected:
    // Multinet has no datalink startup to run, so a connection being made
    // is the same thing as the circuit being up.
    State connected () override;
    State running (Work &w) override;

    MultinetDevice dev_;
    SourceAddress  source_;
    HostAddress    dest_;
    Backoff        conn_timer_ { 5.0, 120.0 };
};

// The TCP forms share their framing, their send path and their teardown.
//
// The socket is non-blocking.  send() adds the frame to one buffer and has
// the node thread write it once the work in hand is done, so a burst of
// frames goes out in one write; a full buffer is written at once.
// Whatever the connection does not take waits for the receive thread,
// which writes it as the connection drains.  So a peer slow to read
// neither holds up the node thread nor costs the circuit, and nothing goes
// on the wire but whole frames.
class TcpMultinet : public Multinet {
public:
    TcpMultinet (Element *owner, std::string name, MultinetDevice dev,
                 std::string source_host = "");

    void send (Bytes msg) override;

protected:
    void disconnect () override;
    void receive_loop () override;

private:
    // Write what is buffered until the connection is full.  False on an
    // error that ends the connection.  Called with txm_ held.
    bool flush ();
    // The node thread's write of what send() buffered.
    void flush_posted ();
    // Have the receive thread look at the buffer now.
    void wake ();

    std::mutex  txm_;
    Bytes       txbuf_;                 // whole frames, header and all
    std::size_t txoff_ = 0;             // of txbuf_, already written
    bool        posted_ = false;        // a flush_posted is queued
    bool        blocked_ = false;       // the connection took no more
    // A UDP socket connected to itself: a byte sent to it ends the receive
    // thread's poll.  Works with poll and WSAPoll alike.
    Socket            wake_;
};

// Active end: connect out, retry on a backoff.
class ConnectMultinet : public TcpMultinet {
public:
    using TcpMultinet::TcpMultinet;

protected:
    void connect () override;
    bool check_connection () override;
};

// Passive end: listen, accept one connection from the expected peer.
class ListenMultinet : public TcpMultinet {
public:
    using TcpMultinet::TcpMultinet;

protected:
    void connect () override;
    bool check_connection () override;

private:
    // The listening socket, distinct from the data socket the base class
    // holds once a connection has been accepted.
    Socket listener_;
};

// UDP: connectionless, so there is nothing to wait for and nothing that
// can report a remote restart.
class UdpMultinet : public Multinet {
public:
    using Multinet::Multinet;

    void send (Bytes msg) override;
    Port *create_port (Element *owner) override;

protected:
    void connect () override;
    void disconnect () override;
    bool check_connection () override;
    void receive_loop () override;

private:
    std::uint16_t seq_ = 0;
};

// One connection accepted by a MultinetServer.  The connection is made
// before this exists, so there is nothing to connect and nothing to
// reconnect: when it ends, for whatever reason, the circuit goes away.
class AcceptedMultinet : public TcpMultinet {
public:
    // Called on the node thread, with this datalink's name, once the
    // connection is closed and the receive thread has finished.
    using RetireFn = std::function<void (const std::string &)>;

    AcceptedMultinet (Element *owner, std::string name, MultinetDevice dev,
                      Socket conn, std::string peer, RetireFn retire);

    bool validate (Work &w) override;
    void dispatch (Work &w) override
    {
        if (retired_) return;           // stale work for a removed circuit
        PtpDatalink::dispatch (w);
    }

    std::string peer_description () const override { return peer_; }

    // Set once the datalink layer has let go of it: from here on every
    // work item still addressed to it is dropped.
    void mark_retired () noexcept { retired_ = true; }

protected:
    void connect () override {}
    bool check_connection () override { return socket_.valid (); }

private:
    // Start closing: ask the receive thread to finish, or finish now if it
    // never started.
    void close_now ();
    // The thread is done: close the connection and hand back the circuit.
    void finish ();

    std::string peer_;
    RetireFn    retire_;
    bool        closing_ = false;       // close_now has run
    bool        finished_ = false;      // finish has run
    bool        owner_knows_ = false;   // routing asked for the close
    bool        retired_ = false;
};

// A Multinet TCP listener for any number of peers.  It is not a circuit:
// each connection it accepts becomes a new circuit, named after it, "MUL"
// giving MUL-0, MUL-1 and so on, skipping names already in use.  Whether
// the node at the far end is welcome is for the routing layer to decide
// once its routing init says who it is.
class MultinetServer : public Element {
public:
    // c is the configuration line; its options apply to every circuit made.
    // used holds the names of the configured circuits, never to be reused.
    MultinetServer (Element *owner, const CircuitConfig &c,
                    MultinetDevice dev, std::set<std::string> used);
    ~MultinetServer () override;

    static constexpr const char *class_name = "MultinetServer";

    // Begin listening, on a thread of its own.
    void start ();
    // Stop listening and wait for the thread.  Circuits already made are
    // left to the routing layer to stop.
    void stop ();

    const std::string &name () const noexcept { return config_.name; }
    std::size_t live () const noexcept { return live_.size (); }

    void dispatch (Work &) override {}

private:
    void accept_loop ();                // the listening thread
    bool allowed (const Endpoint &peer);
    // Node thread: make a circuit for a new connection.
    void accepted (const std::shared_ptr<Socket> &conn,
                   const std::string &peer);
    // Node thread: the connection of circuit name has ended.
    void retire (const std::string &name);
    std::string next_name () const;

    CircuitConfig            config_;
    MultinetDevice           dev_;
    SourceAddress            source_;
    std::vector<HostAddress> allow_;
    std::set<std::string>    used_;     // configured circuit names
    std::set<std::string>    live_;     // circuits made and not yet removed

    Socket                   listener_; // the listening thread's own
    std::thread              thread_;
    std::atomic<bool>        stopping_ { false };
};

}   // namespace decnet::datalink

#endif  // DECNET_DATALINK_MULTINET_H
