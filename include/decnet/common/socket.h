// decnet/common/socket.h -- sockets and host addresses.
//
// Port of the parts of host.py the datalinks need: resolve a host name and
// port, create connected, listening and UDP sockets, and check whether an
// inbound connection came from the address we expect.
//
// PORT: pydecnet resolves names on a background thread and re-resolves
// periodically, so a peer on a dynamic address is followed.  The interval
// is kept here, but the lookup runs inline on the datalink's receive
// thread, which is already allowed to block.  A background resolver only
// matters when many circuits share one name.

#ifndef DECNET_COMMON_SOCKET_H
#define DECNET_COMMON_SOCKET_H

#include "decnet/common/types.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <netdb.h>
#include <sys/socket.h>

namespace decnet {

// A file descriptor that closes itself.  Datalinks hand these between
// threads, so the operations that matter are movable, not copyable.
class Socket {
public:
    Socket () noexcept = default;
    explicit Socket (int fd) noexcept : fd_ (fd) {}
    ~Socket () { close (); }

    Socket (Socket &&o) noexcept : fd_ (o.fd_) { o.fd_ = -1; }
    Socket &operator= (Socket &&o) noexcept
    {
        if (this != &o) { close (); fd_ = o.fd_; o.fd_ = -1; }
        return *this;
    }

    Socket (const Socket &) = delete;
    Socket &operator= (const Socket &) = delete;

    int  fd () const noexcept { return fd_; }
    bool valid () const noexcept { return fd_ >= 0; }
    explicit operator bool () const noexcept { return valid (); }

    // Release ownership, for handing the fd to another Socket.
    int release () noexcept { int f = fd_; fd_ = -1; return f; }

    void close () noexcept;

    // Shut down both directions before closing, which is what the Multinet
    // disconnect path does so the peer sees the close promptly.
    void shutdown () noexcept;

    void set_nonblocking (bool on = true) noexcept;
    void set_reuseaddr () noexcept;
    void set_nodelay () noexcept;

    // The pending error from a non-blocking connect, once the socket is
    // writable.  Zero means the connection succeeded.
    int socket_error () const noexcept;

private:
    int fd_ = -1;
};

// Result of poll(): what happened on a socket.
struct PollResult {
    bool readable = false;
    bool writable = false;
    bool error    = false;      // POLLERR or POLLHUP
    bool timeout  = false;
};

// Wait for a socket, up to ms milliseconds.  A negative fd returns error.
PollResult poll_socket (int fd, bool want_read, bool want_write, int ms);

// The poll timeout the datalinks use, matching multinet's POLLTS: long
// enough to be cheap, short enough that a stop request is noticed promptly.
inline constexpr int poll_timeout_ms = 1000;

// One resolved endpoint.
struct Endpoint {
    sockaddr_storage addr {};
    socklen_t        len = 0;
    int              family = AF_UNSPEC;

    std::string str () const;
    // Compare the address only, ignoring the port: an inbound connection
    // comes from an ephemeral port, so that is all listen mode can check.
    bool same_host (const Endpoint &o) const noexcept;
};

// A remote host and port, re-resolved from time to time.  Port of
// host.HostAddress.
class HostAddress {
public:
    // An empty name means "any address", which listen mode uses to accept
    // a connection from anywhere.
    HostAddress (std::string name, std::uint16_t port,
                 std::chrono::seconds interval = std::chrono::seconds (3600));

    const std::string &name () const noexcept { return name_; }
    std::uint16_t port () const noexcept { return port_; }
    bool any () const noexcept { return name_.empty (); }

    // Resolve now if the name has never been looked up or the interval has
    // passed.  Returns false if the lookup failed; the previous addresses,
    // if any, are then kept, as host.py does.
    bool resolve ();

    // The address to use next.  Cycles through the resolved list so that a
    // host with several addresses is retried on a different one each time,
    // which is what HostAddress.__next__ does.
    const Endpoint *current () const noexcept;

    // Resolve if needed and return where to send; null if unresolvable.
    const Endpoint *destination ();
    void advance () noexcept;

    // Would we accept a connection from this peer?
    bool valid (const Endpoint &peer) const;

    std::string str () const;

private:
    std::string                          name_;
    std::uint16_t                        port_;
    std::chrono::seconds                 interval_;
    std::chrono::steady_clock::time_point next_lookup_ {};
    std::vector<Endpoint>                addrs_;
    std::size_t                          index_ = 0;
    bool                                 looked_up_ = false;
};

// The local address to bind to.  Port of host.SourceAddress.
class SourceAddress {
public:
    SourceAddress (std::string name, std::uint16_t port);

    std::uint16_t port () const noexcept { return port_; }
    const std::string &name () const noexcept { return name_; }

    // Bind a socket of the given family and type, returning it unbound-on
    // -failure (an invalid Socket).
    Socket bind_socket (int family, int type, int protocol = 0) const;

    // A listening TCP socket.  Port of SourceAddress.create_server.
    Socket create_server () const;

    std::string str () const;

private:
    std::string   name_;
    std::uint16_t port_;
};

// Start a non-blocking TCP connection to dest, bound to src if src has a
// port or a name.  The socket is returned immediately; the caller waits for
// it to become writable.  Port of HostAddress.create_connection.
Socket create_connection (HostAddress &dest, const SourceAddress &src);

// A bound UDP socket.  Port of HostAddress.create_udp.
//
// Deliberately NOT connected.  A connected UDP socket receives ICMP
// errors: send to a port nobody has bound yet -- which happens routinely
// when the peer has not started, or restarts -- and the next poll reports
// POLLERR.  A datalink that treated that as fatal would die because a
// datagram bounced, which is precisely what a datagram service does not
// promise.  So we bind only, send with send_datagram and check the sender
// on receive, as pydecnet does with sendto and recvfrom.
Socket create_udp (HostAddress &dest, const SourceAddress &src);

// Send one datagram.  Returns false on error, which callers generally
// ignore: on a datagram service a send that fails is a dropped packet.
bool send_datagram (int fd, ByteView data, const Endpoint &to);

// Receive one datagram, recording who sent it.  Returns the length, or -1.
ssize_t recv_datagram (int fd, std::uint8_t *buf, std::size_t len,
                       Endpoint &from);

// The peer address of a connected socket, for the listen-mode check.
Endpoint peer_of (int fd);

}   // namespace decnet

#endif  // DECNET_COMMON_SOCKET_H
