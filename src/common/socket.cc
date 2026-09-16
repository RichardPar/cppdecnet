#include "decnet/common/socket.h"

#include "decnet/common/logging.h"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <unistd.h>

namespace decnet {

// ---------------------------------------------------------------- Socket

void Socket::close () noexcept
{
    if (fd_ >= 0) {
        ::close (fd_);
        fd_ = -1;
    }
}

void Socket::shutdown () noexcept
{
    if (fd_ >= 0) ::shutdown (fd_, SHUT_RDWR);
}

void Socket::set_nonblocking (bool on) noexcept
{
    if (fd_ < 0) return;
    int flags = ::fcntl (fd_, F_GETFL, 0);
    if (flags < 0) return;
    flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    ::fcntl (fd_, F_SETFL, flags);
}

void Socket::set_reuseaddr () noexcept
{
    int one = 1;
    if (fd_ >= 0)
        ::setsockopt (fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
}

void Socket::set_nodelay () noexcept
{
    // Multinet frames are small and latency sensitive; Nagle would coalesce
    // them into round trip delays.
    int one = 1;
    if (fd_ >= 0)
        ::setsockopt (fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

int Socket::socket_error () const noexcept
{
    if (fd_ < 0) return EBADF;
    int err = 0;
    socklen_t len = sizeof err;
    if (::getsockopt (fd_, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
        return errno;
    return err;
}

// ------------------------------------------------------------------ poll

PollResult poll_socket (int fd, bool want_read, bool want_write, int ms)
{
    PollResult r;
    if (fd < 0) { r.error = true; return r; }

    pollfd p {};
    p.fd = fd;
    p.events = static_cast<short> ((want_read ? POLLIN : 0)
                                 | (want_write ? POLLOUT : 0));
    int n = ::poll (&p, 1, ms);
    if (n < 0) {
        // EINTR is not an error: the caller loops and checks its stop flag.
        if (errno == EINTR) { r.timeout = true; return r; }
        r.error = true;
        return r;
    }
    if (n == 0) { r.timeout = true; return r; }
    r.readable = (p.revents & POLLIN)  != 0;
    r.writable = (p.revents & POLLOUT) != 0;
    r.error    = (p.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
    return r;
}

// -------------------------------------------------------------- Endpoint

std::string Endpoint::str () const
{
    char host[NI_MAXHOST] = "?";
    char serv[NI_MAXSERV] = "?";
    if (len)
        ::getnameinfo (reinterpret_cast<const sockaddr *> (&addr), len,
                       host, sizeof host, serv, sizeof serv,
                       NI_NUMERICHOST | NI_NUMERICSERV);
    return std::string (host) + ":" + serv;
}

namespace {

// Normalise an address for comparison.  IPv4-mapped IPv6 addresses
// (::ffff:a.b.c.d) are converted to IPv4.
struct AddrBytes {
    const unsigned char *data = nullptr;
    std::size_t          len = 0;
};

AddrBytes addr_bytes (const sockaddr_storage &ss, int family)
{
    if (family == AF_INET) {
        auto *a = reinterpret_cast<const sockaddr_in *> (&ss);
        return { reinterpret_cast<const unsigned char *> (&a->sin_addr), 4 };
    }
    if (family == AF_INET6) {
        auto *a = reinterpret_cast<const sockaddr_in6 *> (&ss);
        auto *b = reinterpret_cast<const unsigned char *> (&a->sin6_addr);
        if (IN6_IS_ADDR_V4MAPPED (&a->sin6_addr)) return { b + 12, 4 };
        return { b, 16 };
    }
    return {};
}

}   // namespace

bool Endpoint::same_host (const Endpoint &o) const noexcept
{
    AddrBytes a = addr_bytes (addr, family);
    AddrBytes b = addr_bytes (o.addr, o.family);
    if (!a.data || !b.data || a.len != b.len) return false;
    return std::memcmp (a.data, b.data, a.len) == 0;
}

namespace {

// getaddrinfo into our Endpoint list.  An empty name means "unspecified",
// which for a bind gives the wildcard address.
std::vector<Endpoint> lookup (const std::string &name, std::uint16_t port,
                              bool passive)
{
    std::vector<Endpoint> out;
    addrinfo hints {};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = 0;
    hints.ai_flags    = passive ? AI_PASSIVE : 0;

    std::string service = std::to_string (port);
    addrinfo *res = nullptr;
    int rc = ::getaddrinfo (name.empty () ? nullptr : name.c_str (),
                            service.c_str (), &hints, &res);
    if (rc != 0) {
        DN_TRACE ("lookup of {} failed: {}", name.empty () ? "*" : name,
                  ::gai_strerror (rc));
        return out;
    }
    for (addrinfo *a = res; a; a = a->ai_next) {
        if (a->ai_family != AF_INET && a->ai_family != AF_INET6) continue;
        Endpoint e;
        std::memcpy (&e.addr, a->ai_addr, a->ai_addrlen);
        e.len    = a->ai_addrlen;
        e.family = a->ai_family;
        // getaddrinfo returns one entry per socket type; keep each address
        // once.
        bool dup = false;
        for (const Endpoint &x : out)
            if (x.len == e.len
                && std::memcmp (&x.addr, &e.addr, e.len) == 0) { dup = true; break; }
        if (!dup) out.push_back (e);
    }
    ::freeaddrinfo (res);
    return out;
}

}   // namespace

// ----------------------------------------------------------- HostAddress

HostAddress::HostAddress (std::string name, std::uint16_t port,
                          std::chrono::seconds interval)
    : name_ (std::move (name)), port_ (port), interval_ (interval)
{
}

bool HostAddress::resolve ()
{
    if (any ()) return true;
    auto now = std::chrono::steady_clock::now ();
    if (looked_up_ && now < next_lookup_) return true;

    std::vector<Endpoint> found = lookup (name_, port_, false);
    next_lookup_ = now + interval_;
    if (found.empty ()) {
        // Keep whatever we had: a transient DNS failure should not take a
        // working circuit down.  host.py behaves the same way.
        DN_TRACE ("keeping previous addresses for {}", name_);
        return looked_up_;
    }
    addrs_ = std::move (found);
    if (index_ >= addrs_.size ()) index_ = 0;
    looked_up_ = true;
    return true;
}

const Endpoint *HostAddress::current () const noexcept
{
    if (addrs_.empty ()) return nullptr;
    return &addrs_[index_];
}

void HostAddress::advance () noexcept
{
    if (!addrs_.empty ()) index_ = (index_ + 1) % addrs_.size ();
}

bool HostAddress::valid (const Endpoint &peer) const
{
    if (any ()) return true;
    for (const Endpoint &e : addrs_)
        if (e.same_host (peer)) return true;
    return false;
}

std::string HostAddress::str () const
{
    return (any () ? std::string ("*") : name_) + ":" + std::to_string (port_);
}

// --------------------------------------------------------- SourceAddress

SourceAddress::SourceAddress (std::string name, std::uint16_t port)
    : name_ (std::move (name)), port_ (port)
{
}

Socket SourceAddress::bind_socket (int family, int type, int protocol) const
{
    Socket s (::socket (family, type, protocol));
    if (!s) return s;
    s.set_reuseaddr ();

    // Nothing to bind to: an outbound connection with no source constraint.
    if (name_.empty () && port_ == 0) return s;

    std::vector<Endpoint> addrs = lookup (name_, port_, true);
    for (const Endpoint &e : addrs) {
        if (e.family != family) continue;
        if (::bind (s.fd (), reinterpret_cast<const sockaddr *> (&e.addr),
                    e.len) == 0)
            return s;
        DN_TRACE ("bind to {} failed: {}", e.str (), std::strerror (errno));
    }
    if (addrs.empty ())
        DN_TRACE ("no local address for {}", str ());
    return Socket {};
}

Socket SourceAddress::create_server () const
{
    // Prefer IPv6, which accepts IPv4 too on a dual stack host; fall back
    // to IPv4 where IPv6 is unavailable.
    for (int family : { AF_INET6, AF_INET }) {
        Socket s = bind_socket (family, SOCK_STREAM);
        if (!s) continue;
        if (::listen (s.fd (), 1) < 0) {
            DN_TRACE ("listen failed: {}", std::strerror (errno));
            continue;
        }
        return s;
    }
    return Socket {};
}

std::string SourceAddress::str () const
{
    return (name_.empty () ? std::string ("*") : name_) + ":"
        + std::to_string (port_);
}

// ------------------------------------------------------- socket factories

Socket create_connection (HostAddress &dest, const SourceAddress &src)
{
    if (!dest.resolve ()) return Socket {};
    const Endpoint *e = dest.current ();
    if (!e) return Socket {};

    Socket s = src.bind_socket (e->family, SOCK_STREAM);
    if (!s) return s;
    s.set_nonblocking ();
    s.set_nodelay ();

    if (::connect (s.fd (), reinterpret_cast<const sockaddr *> (&e->addr),
                   e->len) < 0
        && errno != EINPROGRESS) {
        DN_TRACE ("connect to {} failed: {}", e->str (), std::strerror (errno));
        return Socket {};
    }
    return s;
}

Socket create_udp (HostAddress &dest, const SourceAddress &src)
{
    if (!dest.resolve ()) return Socket {};
    const Endpoint *e = dest.current ();
    int family = e ? e->family : AF_INET;
    // Bound, not connected -- see the comment in the header.
    return src.bind_socket (family, SOCK_DGRAM);
}

bool send_datagram (int fd, ByteView data, const Endpoint &to)
{
    if (fd < 0 || !to.len) return false;
    ssize_t n = ::sendto (fd, data.data (), data.size (), MSG_NOSIGNAL,
                          reinterpret_cast<const sockaddr *> (&to.addr),
                          to.len);
    return n == static_cast<ssize_t> (data.size ());
}

ssize_t recv_datagram (int fd, std::uint8_t *buf, std::size_t len,
                       Endpoint &from)
{
    from.len = sizeof from.addr;
    ssize_t n = ::recvfrom (fd, buf, len, 0,
                            reinterpret_cast<sockaddr *> (&from.addr),
                            &from.len);
    if (n < 0) { from.len = 0; return n; }
    from.family = reinterpret_cast<sockaddr *> (&from.addr)->sa_family;
    return n;
}

const Endpoint *HostAddress::destination ()
{
    if (!resolve ()) return nullptr;
    return current ();
}

Endpoint peer_of (int fd)
{
    Endpoint e;
    e.len = sizeof e.addr;
    if (::getpeername (fd, reinterpret_cast<sockaddr *> (&e.addr), &e.len) < 0) {
        e.len = 0;
        return e;
    }
    e.family = reinterpret_cast<sockaddr *> (&e.addr)->sa_family;
    return e;
}

}   // namespace decnet
