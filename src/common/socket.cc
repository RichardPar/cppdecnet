#include "decnet/common/socket.h"

#include "decnet/common/logging.h"

#include <cerrno>
#include <cstring>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace decnet {

// -------------------------------------------------------------- platform

#ifdef _WIN32

void net_init ()
{
    static const bool started = [] {
        WSADATA wsa;
        return ::WSAStartup (MAKEWORD (2, 2), &wsa) == 0;
    } ();
    (void) started;
}

std::string sock_strerror (int err)
{
    char buf[256] = "";
    DWORD n = ::FormatMessageA (FORMAT_MESSAGE_FROM_SYSTEM
                                | FORMAT_MESSAGE_IGNORE_INSERTS,
                                nullptr, static_cast<DWORD> (err), 0,
                                buf, sizeof buf, nullptr);
    // System messages end in ".\r\n"; strerror's do not.
    while (n && (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == '.'))
        buf[--n] = '\0';
    if (!n) return "error " + std::to_string (err);
    return buf;
}

#else

void net_init () {}

std::string sock_strerror (int err)
{
    return std::strerror (err);
}

#endif

// ---------------------------------------------------------------- Socket

void Socket::close () noexcept
{
    if (fd_ >= 0) {
        sock_close (fd_);
        fd_ = -1;
    }
}

void Socket::shutdown () noexcept
{
#ifdef _WIN32
    // Unlike POSIX, shutdown does not wake a thread blocked in recv or
    // accept on this socket; cancelling its pending I/O does.  The retry
    // then fails, as it would after shutdown on POSIX.
    if (fd_ >= 0) {
        ::shutdown (static_cast<SOCKET> (fd_), SD_BOTH);
        ::CancelIoEx (reinterpret_cast<HANDLE> (static_cast<SOCKET> (fd_)),
                      nullptr);
    }
#else
    if (fd_ >= 0) ::shutdown (fd_, SHUT_RDWR);
#endif
}

void Socket::set_nonblocking (bool on) noexcept
{
    if (fd_ < 0) return;
#ifdef _WIN32
    u_long mode = on ? 1 : 0;
    ::ioctlsocket (static_cast<SOCKET> (fd_), FIONBIO, &mode);
#else
    int flags = ::fcntl (fd_, F_GETFL, 0);
    if (flags < 0) return;
    flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    ::fcntl (fd_, F_SETFL, flags);
#endif
}

namespace {

void set_int_option (int fd, int level, int name, int value) noexcept
{
#ifdef _WIN32
    ::setsockopt (static_cast<SOCKET> (fd), level, name,
                  reinterpret_cast<const char *> (&value), sizeof value);
#else
    ::setsockopt (fd, level, name, &value, sizeof value);
#endif
}

}   // namespace

void Socket::set_reuseaddr () noexcept
{
    // On Windows SO_REUSEADDR lets a second socket bind a port that is in
    // active use, which is not what this is for; Windows already allows
    // rebinding a port in TIME_WAIT without it.
#ifndef _WIN32
    if (fd_ >= 0) set_int_option (fd_, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
}

void Socket::set_nodelay () noexcept
{
    // Multinet frames are small and latency sensitive; Nagle would coalesce
    // them into round trip delays.
    if (fd_ >= 0) set_int_option (fd_, IPPROTO_TCP, TCP_NODELAY, 1);
}

int Socket::socket_error () const noexcept
{
    if (fd_ < 0) return EBADF;
    int err = 0;
    socklen_t len = sizeof err;
#ifdef _WIN32
    if (::getsockopt (static_cast<SOCKET> (fd_), SOL_SOCKET, SO_ERROR,
                      reinterpret_cast<char *> (&err), &len) < 0)
#else
    if (::getsockopt (fd_, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
#endif
        return sock_errno ();
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
    int n = sock_poll (&p, 1, ms);
    if (n < 0) {
        // EINTR is not an error: the caller loops and checks its stop flag.
        if (sock_interrupted (sock_errno ())) { r.timeout = true; return r; }
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
                       host, static_cast<socklen_t> (sizeof host),
                       serv, static_cast<socklen_t> (sizeof serv),
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
    net_init ();
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
        e.len    = static_cast<socklen_t> (a->ai_addrlen);
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
    return open_socket (family, type, protocol, type != SOCK_STREAM);
}

Socket SourceAddress::open_socket (int family, int type, int protocol,
                                   bool must_bind) const
{
    Socket s (sock_open (family, type, protocol));
    if (!s) return s;
    s.set_reuseaddr ();
    // An IPv6 socket takes IPv4 too only if IPV6_V6ONLY is off.  That is
    // the Linux default, but Windows and the BSDs default to on.
    if (family == AF_INET6) set_int_option (s.fd (), IPPROTO_IPV6, IPV6_V6ONLY, 0);

    // Nothing to bind to: an outbound connection with no source constraint.
    if (name_.empty () && port_ == 0 && !must_bind) return s;

    std::vector<Endpoint> addrs = lookup (name_, port_, true);
    for (const Endpoint &e : addrs) {
        if (e.family != family) continue;
        if (::bind (s.fd (), reinterpret_cast<const sockaddr *> (&e.addr),
                    e.len) == 0)
            return s;
        DN_TRACE ("bind to {} failed: {}", e.str (),
                  sock_strerror (sock_errno ()));
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
        Socket s = open_socket (family, SOCK_STREAM, 0, true);
        if (!s) continue;
        if (::listen (s.fd (), 1) < 0) {
            DN_TRACE ("listen failed: {}", sock_strerror (sock_errno ()));
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
        && !sock_in_progress (sock_errno ())) {
        DN_TRACE ("connect to {} failed: {}", e->str (),
                  sock_strerror (sock_errno ()));
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
#ifdef _WIN32
    ssize_t n = ::sendto (static_cast<SOCKET> (fd),
                          reinterpret_cast<const char *> (data.data ()),
                          static_cast<int> (data.size ()), 0,
                          reinterpret_cast<const sockaddr *> (&to.addr),
                          to.len);
#else
    ssize_t n = ::sendto (fd, data.data (), data.size (), MSG_NOSIGNAL,
                          reinterpret_cast<const sockaddr *> (&to.addr),
                          to.len);
#endif
    return n == static_cast<ssize_t> (data.size ());
}

ssize_t recv_datagram (int fd, std::uint8_t *buf, std::size_t len,
                       Endpoint &from)
{
    from.len = sizeof from.addr;
#ifdef _WIN32
    ssize_t n = ::recvfrom (static_cast<SOCKET> (fd),
                            reinterpret_cast<char *> (buf),
                            static_cast<int> (len), 0,
                            reinterpret_cast<sockaddr *> (&from.addr),
                            &from.len);
#else
    ssize_t n = ::recvfrom (fd, buf, len, 0,
                            reinterpret_cast<sockaddr *> (&from.addr),
                            &from.len);
#endif
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
