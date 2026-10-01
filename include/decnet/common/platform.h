// decnet/common/platform.h -- the few places POSIX and Windows differ.
//
// The code is written against POSIX sockets and libc.  On Windows this
// header supplies Winsock and thin wrappers so the same source compiles
// with MSVC; on POSIX hosts the wrappers are the plain system calls.
//
// Socket handles stay `int` everywhere.  A Winsock SOCKET is a kernel
// handle, which Windows guarantees fits in 32 bits, and INVALID_SOCKET
// converts to -1, so the existing `fd >= 0` checks hold.

#ifndef DECNET_COMMON_PLATFORM_H
#define DECNET_COMMON_PLATFORM_H

#include <cerrno>
#include <cstddef>
#include <ctime>
#include <string>

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#include <BaseTsd.h>

using ssize_t = SSIZE_T;

// Winsock never raises SIGPIPE, so there is nothing to suppress.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif

// Empty 16-bit leftovers from <windows.h> that collide with ordinary names.
#undef far
#undef near

// The reentrant time conversions, with POSIX argument order.
inline std::tm *localtime_r (const std::time_t *t, std::tm *out)
{
    return ::localtime_s (out, t) == 0 ? out : nullptr;
}

inline std::tm *gmtime_r (const std::time_t *t, std::tm *out)
{
    return ::gmtime_s (out, t) == 0 ? out : nullptr;
}

#else   // POSIX

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#endif

namespace decnet {

// Start the socket library.  Idempotent; Socket and the socket factories
// call it, so only code that calls ::socket directly needs to.
void net_init ();

// The error from the last failed socket call, and its text.
inline int sock_errno () noexcept
{
#ifdef _WIN32
    return ::WSAGetLastError ();
#else
    return errno;
#endif
}

std::string sock_strerror (int err);

// Error classes the I/O loops care about.
inline bool sock_would_block (int err) noexcept
{
#ifdef _WIN32
    return err == WSAEWOULDBLOCK;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

inline bool sock_interrupted (int err) noexcept
{
#ifdef _WIN32
    return err == WSAEINTR;
#else
    return err == EINTR;
#endif
}

// A non-blocking connect that has started but not finished.
inline bool sock_in_progress (int err) noexcept
{
#ifdef _WIN32
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
    return err == EINPROGRESS;
#endif
}

inline int sock_close (int fd) noexcept
{
#ifdef _WIN32
    return ::closesocket (static_cast<SOCKET> (fd));
#else
    return ::close (fd);
#endif
}

// send and recv on a stream socket.  send never raises SIGPIPE.
inline ssize_t sock_send (int fd, const void *buf, std::size_t len) noexcept
{
#ifdef _WIN32
    return ::send (static_cast<SOCKET> (fd), static_cast<const char *> (buf),
                   static_cast<int> (len), 0);
#else
    return ::send (fd, buf, len, MSG_NOSIGNAL);
#endif
}

inline ssize_t sock_recv (int fd, void *buf, std::size_t len) noexcept
{
#ifdef _WIN32
    return ::recv (static_cast<SOCKET> (fd), static_cast<char *> (buf),
                   static_cast<int> (len), 0);
#else
    return ::recv (fd, buf, len, 0);
#endif
}

// Accept a connection; returns -1 on failure.
inline int sock_accept (int fd) noexcept
{
#ifdef _WIN32
    SOCKET s = ::accept (static_cast<SOCKET> (fd), nullptr, nullptr);
    return s == INVALID_SOCKET ? -1 : static_cast<int> (s);
#else
    return ::accept (fd, nullptr, nullptr);
#endif
}

// A new socket; returns -1 on failure.
inline int sock_open (int family, int type, int protocol = 0)
{
#ifdef _WIN32
    net_init ();
    SOCKET s = ::socket (family, type, protocol);
    return s == INVALID_SOCKET ? -1 : static_cast<int> (s);
#else
    return ::socket (family, type, protocol);
#endif
}

// poll(2) over sockets.  On Windows this is WSAPoll, which only works on
// sockets -- not pipes or devices.
inline int sock_poll (pollfd *fds, std::size_t n, int ms) noexcept
{
#ifdef _WIN32
    return ::WSAPoll (fds, static_cast<ULONG> (n), ms);
#else
    return ::poll (fds, static_cast<nfds_t> (n), ms);
#endif
}

}   // namespace decnet

#endif  // DECNET_COMMON_PLATFORM_H
