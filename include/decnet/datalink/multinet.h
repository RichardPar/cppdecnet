// decnet/datalink/multinet.h -- Multinet over TCP or UDP.
//
// Port of multinet.py.  As that module's docstring says, Multinet fails a
// number of the requirements the routing spec places on a point to point
// datalink -- there is no datalink level startup, so a remote restart is
// invisible.  The point to point sublayer works around that when the port
// reports start_works() false, which the UDP form does.
//
// The framing is four bytes of header then the payload.  Over TCP the first
// two header bytes are the payload length, little endian; over UDP they are
// a sequence number that the receiver ignores.  The remaining two bytes are
// zero either way.
//
// Device syntax, unchanged from pydecnet:
//
//     host[:port][:connect]    active end of a TCP connection
//     host[:port][:listen]     passive end of a TCP connection
//     host[:port][:localport]  UDP
//
// The port defaults to 700.  In listen mode the host may be empty, which
// accepts a connection from any address.

#ifndef DECNET_DATALINK_MULTINET_H
#define DECNET_DATALINK_MULTINET_H

#include "decnet/datalink/ptp.h"

#include <optional>

namespace decnet::datalink {

// The parsed --device argument.
struct MultinetDevice {
    enum class Mode { connect, listen, udp };

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
class TcpMultinet : public Multinet {
public:
    using Multinet::Multinet;

    void send (Bytes msg) override;

protected:
    void disconnect () override;
    void receive_loop () override;
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

}   // namespace decnet::datalink

#endif  // DECNET_DATALINK_MULTINET_H
