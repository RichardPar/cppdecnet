// src/datalink/ddcmp_link.cc -- the DDCMP datalink and its transports.
// Port of the _DDCMP transport subclasses in ddcmp.py.

#include "decnet/datalink/ddcmp.h"

#include "decnet/common/logging.h"
#include "decnet/common/work.h"
#include "decnet/node.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <termios.h>
#include <unistd.h>
#include <stdexcept>

namespace decnet::datalink {

using ddcmp::Message;
using ddcmp::HdrError;

namespace {

constexpr int poll_timeout_ms = 200;

std::vector<std::string> split (const std::string &s, char c)
{
    std::vector<std::string> out;
    std::string cur;
    std::istringstream in (s);
    while (std::getline (in, cur, c)) out.push_back (cur);
    return out;
}

unsigned to_port (const std::string &s, const char *what)
{
    try {
        unsigned long v = std::stoul (s);
        if (v > 65535) throw std::out_of_range (what);
        return static_cast<unsigned> (v);
    } catch (const std::exception &) {
        throw std::invalid_argument (std::string ("bad ") + what
                                     + " in DDCMP device: " + s);
    }
}

}   // namespace

// ---------------------------------------------------------------- device

DdcmpDevice DdcmpDevice::parse (const std::string &device)
{
    std::vector<std::string> f = split (device, ':');
    if (f.empty ())
        throw std::invalid_argument ("empty DDCMP device");

    DdcmpDevice d;
    std::string proto = f[0];
    for (char &c : proto) c = static_cast<char> (std::tolower (c));

    if (proto == "serial") {
        // serial:devname[:speed]
        if (f.size () < 2)
            throw std::invalid_argument ("serial DDCMP device needs a name");
        d.mode = Mode::serial;
        d.destination = f[1];
        if (f.size () > 2) d.speed = to_port (f[2], "speed");
        return d;
    }

    if (proto == "udp")         d.mode = Mode::udp;
    else if (proto == "tcp")    d.mode = Mode::tcp;
    else if (proto == "telnet") d.mode = Mode::telnet;
    else
        throw std::invalid_argument ("unknown DDCMP protocol: " + f[0]);

    // proto:lport:host:rport
    if (f.size () != 4)
        throw std::invalid_argument ("DDCMP device wants proto:lport:host:rport");
    d.source_port = static_cast<std::uint16_t> (to_port (f[1], "local port"));
    d.destination = f[2];
    d.dest_port = static_cast<std::uint16_t> (to_port (f[3], "remote port"));
    return d;
}

std::string DdcmpDevice::str () const
{
    std::string out;
    switch (mode) {
    case Mode::udp:    out = "udp";    break;
    case Mode::tcp:    out = "tcp";    break;
    case Mode::telnet: out = "telnet"; break;
    case Mode::serial:
        return "serial:" + destination + ":" + std::to_string (speed);
    }
    return out + ":" + std::to_string (source_port) + ":" + destination
         + ":" + std::to_string (dest_port);
}

// ------------------------------------------------------------- the link

Ddcmp::Ddcmp (Element *owner, std::string name, DdcmpDevice dev)
    : PtpDatalink (owner, std::move (name)),
      dev_ (std::move (dev)),
      source_ ("", dev_.source_port),
      dest_ (dev_.destination, dev_.dest_port)
{
    make_protocol ();
}

Ddcmp::~Ddcmp () = default;

void Ddcmp::make_protocol ()
{
    ddcmp::Protocol::Hooks h;
    h.send = [this] (const Message &m) { transmit (m); };
    h.deliver = [this] (Bytes b) {
        // A payload received in sequence.
        counters_.bytes_recv += b.size ();
        ++counters_.pkts_recv;
        if (port_ && node ())
            node ()->add_work (std::make_unique<Received> (port_->owner (),
                                                           std::move (b)));
    };
    h.up   = [this] { report_up (); };
    h.down = [this] { report_down (); };
    h.set_timer = [this] (double secs) {
        if (Node *n = node ()) {
            if (secs > 0) n->timers ().start (this, secs);
            else          n->timers ().stop (this);
        }
    };
    proto_ = std::make_unique<ddcmp::Protocol> (std::move (h));
}

std::unique_ptr<Datalink> Ddcmp::create (Element *owner,
                                         const std::string &name,
                                         const std::string &device)
{
    DdcmpDevice d = DdcmpDevice::parse (device);
    switch (d.mode) {
    case DdcmpDevice::Mode::udp:
        return std::make_unique<UdpDdcmp> (owner, name, d);
    case DdcmpDevice::Mode::tcp:
    case DdcmpDevice::Mode::telnet:
        return std::make_unique<TcpDdcmp> (owner, name, d);
    case DdcmpDevice::Mode::serial:
        return std::make_unique<SerialDdcmp> (owner, name, d);
    }
    // PORT: the synchronous framer, which is a board that does the framing
    // in hardware and hands over headers with the CRC already checked.
    throw std::invalid_argument ("DDCMP " + d.str () + ": unknown mode");
}

bool Ddcmp::validate (Work &w)
{
    if (dynamic_cast<Restart *> (&w)
        && in_state (DN_MY_STATE (PtpDatalink, running))) {
        // Restart the protocol, not the transport.  The engine reports down, then
        // up when the handshake completes.  Port of the Restart handling in
        // ddcmp.py.
        DN_DEBUG ("{} restarting the DDCMP protocol on request", name_);
        proto_->restart ();
        return false;
    }
    // Before the transport is up there is no protocol to restart, so the
    // base class's handling -- start the connection over -- is right.
    return PtpDatalink::validate (w);
}

Ddcmp::State Ddcmp::connected ()
{
    // Transport is up; the circuit is reported up when the DDCMP handshake
    // completes.
    DN_DEBUG ("{} transport connected, starting DDCMP", name_);
    proto_->connected ();
    return DN_MY_STATE (PtpDatalink, running);
}

void Ddcmp::timeout ()
{
    proto_->timeout ();
}

void Ddcmp::send (Bytes msg)
{
    counters_.bytes_sent += msg.size ();
    ++counters_.pkts_sent;
    proto_->send (std::move (msg));
}

Ddcmp::State Ddcmp::running (Work &w)
{
    if (auto *r = dynamic_cast<Received *> (&w)) {
        // The receive thread hands up whole messages, already framed.
        const Bytes &buf = r->packet ();
        Message m;
        HdrError e = ddcmp::decode_header (ByteView (buf.data (), buf.size ()),
                                           m);
        if (e != HdrError::none) {
            // Not a valid DDCMP message; drop it.
            DN_TRACE ("{}: undecodable DDCMP message, dropped", name_);
            return nullptr;
        }
        if (m.is_data ()) {
            // Payload and its CRC follow the header.
            std::size_t need = ddcmp::HDRLEN + m.count + 2;
            if (buf.size () < need) {
                m.crcok = false;
            } else {
                m.payload.assign (buf.begin () + ddcmp::HDRLEN,
                                  buf.begin () + ddcmp::HDRLEN + m.count);
                CRC16 c;
                for (std::size_t i = ddcmp::HDRLEN; i < need; ++i)
                    c.update (buf[i]);
                m.crcok = c.good ();
            }
            if (!m.crcok) {
                // The header was good, so the resp field is still worth
                // acting on; receive_error does that before it NAKs.
                proto_->receive_error (ddcmp::R_CRC, &m);
                return nullptr;
            }
        }
        proto_->receive (m);
        return nullptr;
    }
    return nullptr;
}

Bytes Ddcmp::read_framed_message (
    const std::function<Bytes (std::size_t)> &readn)
{
    // Advance a byte at a time until 8 bytes form a header with a valid CRC.
    Bytes hdr = readn (ddcmp::HDRLEN);
    ddcmp::Message m;
    while (ddcmp::decode_header (ByteView (hdr.data (), hdr.size ()), m)
           != ddcmp::HdrError::none) {
        hdr.erase (hdr.begin ());
        Bytes one = readn (1);
        hdr.insert (hdr.end (), one.begin (), one.end ());
    }

    if (m.is_data ()) {
        // The payload and its own CRC follow the header.
        Bytes rest = readn (m.count + 2u);
        hdr.insert (hdr.end (), rest.begin (), rest.end ());
    }
    return hdr;
}

// ------------------------------------------------------------------ UDP

UdpDdcmp::UdpDdcmp (Element *owner, std::string name, DdcmpDevice dev)
    : Ddcmp (owner, std::move (name), std::move (dev))
{
}

void UdpDdcmp::connect ()
{
    // Bound, never connected: a connected datagram socket reports ICMP
    // errors, and a bounced datagram would then kill the receive loop.
    socket_ = create_udp (dest_, source_);
}

void UdpDdcmp::disconnect () { socket_.close (); }

bool UdpDdcmp::check_connection () { return socket_.valid (); }

void UdpDdcmp::receive_loop ()
{
    std::uint8_t buf[2048];
    for (;;) {
        PollResult p = poll_socket (socket_.fd (), true, false, poll_timeout_ms);
        if (stopping ()) return;
        if (p.error) continue;          // not fatal on a datagram socket
        if (p.timeout || !p.readable) continue;

        Endpoint from;
        ssize_t n = recv_datagram (socket_.fd (), buf, sizeof buf, from);
        if (n < 0) continue;
        if (!dest_.valid (from)) continue;
        if (n < static_cast<ssize_t> (ddcmp::HDRLEN)) continue;

        Bytes msg (buf, buf + n);
        if (node ())
            node ()->add_work (std::make_unique<Received> (this,
                                                           std::move (msg)));
    }
}

void UdpDdcmp::transmit (const Message &m)
{
    if (!socket_) return;
    const Endpoint *to = dest_.destination ();
    if (!to) return;
    Bytes wire = m.encode ();
    send_datagram (socket_.fd (), ByteView (wire.data (), wire.size ()), *to);
}

// ------------------------------------------------------------------ TCP

TcpDdcmp::TcpDdcmp (Element *owner, std::string name, DdcmpDevice dev)
    : Ddcmp (owner, std::move (name), std::move (dev))
{
    telnet_ = dev_.mode == DdcmpDevice::Mode::telnet;
}

void TcpDdcmp::connect ()
{
    // Listen and connect at the same time and use the first connection, as
    // SIMH's sim_tmxr does.
    listener_ = source_.create_server ();
    if (!listener_)
        DN_DEBUG ("{}: cannot listen on {}", name_, source_.str ());

    if (!dev_.destination.empty ()) {
        SourceAddress any ("", 0);
        connecting_ = create_connection (dest_, any);
    }
}

void TcpDdcmp::disconnect ()
{
    listener_.close ();
    connecting_.close ();
    socket_.close ();
}

bool TcpDdcmp::check_connection ()
{
    for (;;) {
        if (stopping ()) return false;

        // The outbound attempt finishing shows up as writable; an inbound
        // one as readable on the listener.
        if (connecting_) {
            PollResult p = poll_socket (connecting_.fd (), false, true, 50);
            if (p.error) {
                connecting_.close ();
            } else if (p.writable) {
                int err = connecting_.socket_error ();
                if (err) {
                    DN_TRACE ("{} connect failed: {}", name_,
                              std::strerror (err));
                    connecting_.close ();
                } else {
                    DN_DEBUG ("{} connected outbound to {}", name_,
                              dest_.str ());
                    socket_ = std::move (connecting_);
                    listener_.close ();
                    socket_.set_nodelay ();
                    return true;
                }
            }
        }

        if (listener_) {
            PollResult p = poll_socket (listener_.fd (), true, false, 50);
            if (p.error) {
                listener_.close ();
            } else if (p.readable) {
                Socket conn (::accept (listener_.fd (), nullptr, nullptr));
                if (conn) {
                    Endpoint peer = peer_of (conn.fd ());
                    if (!dest_.any () && !dest_.valid (peer)) {
                        // Someone else dialled us.  A point to point link
                        // has exactly one peer, so this is not it.
                        DN_TRACE ("{}: connection from unexpected address {}",
                                  name_, peer.str ());
                        continue;       // conn closes here
                    }
                    DN_DEBUG ("{} accepted inbound connection", name_);
                    socket_ = std::move (conn);
                    listener_.close ();
                    connecting_.close ();
                    socket_.set_nodelay ();
                    return true;
                }
            }
        }

        if (!listener_ && !connecting_) return false;   // nothing left to wait on
    }
}

Bytes TcpDdcmp::unescape_read (std::size_t n)
{
    if (!telnet_) return recvall (n);

    // Telnet doubles the all-ones byte.  Read until n real bytes have been
    // recovered, collapsing each pair as it appears.
    Bytes out;
    out.reserve (n);
    while (out.size () < n) {
        Bytes b = recvall (n - out.size ());
        std::size_t ff = 0;
        for (std::uint8_t c : b) if (c == ddcmp::DEL) ++ff;
        if (ff & 1) {
            // A pair was split across this read; take its other half.
            Bytes more = recvall (1);
            b.insert (b.end (), more.begin (), more.end ());
        }
        for (std::size_t i = 0; i < b.size (); ++i) {
            out.push_back (b[i]);
            if (b[i] == ddcmp::DEL && i + 1 < b.size () && b[i + 1] == ddcmp::DEL)
                ++i;                    // skip the doubled one
        }
    }
    out.resize (n);
    return out;
}

Bytes TcpDdcmp::escape (const Bytes &b)
{
    Bytes out;
    out.reserve (b.size ());
    for (std::uint8_t c : b) {
        out.push_back (c);
        if (c == ddcmp::DEL) out.push_back (c);
    }
    return out;
}

void TcpDdcmp::receive_loop ()
{
    for (;;) {
        if (stopping ()) return;
        try {
            Bytes msg = read_framed_message (
                [this] (std::size_t n) { return unescape_read (n); });
            if (node ())
                node ()->add_work (std::make_unique<Received> (this,
                                                               std::move (msg)));
        } catch (const std::exception &) {
            return;                     // connection gone, or stop requested
        }
    }
}

void TcpDdcmp::transmit (const Message &m)
{
    if (!socket_) return;
    Bytes wire = m.encode ();
    if (telnet_) wire = escape (wire);

    std::size_t sent = 0;
    while (sent < wire.size ()) {
        ssize_t n = ::send (socket_.fd (), wire.data () + sent,
                            wire.size () - sent, MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += static_cast<std::size_t> (n);
    }
}

// --------------------------------------------------------------- serial

namespace {

// Baud rates termios supports.  Unknown speeds are an error.
speed_t termios_speed (unsigned baud)
{
    switch (baud) {
    case 300:    return B300;
    case 600:    return B600;
    case 1200:   return B1200;
    case 2400:   return B2400;
    case 4800:   return B4800;
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    default:     return 0;
    }
}

}   // namespace

SerialDdcmp::SerialDdcmp (Element *owner, std::string name, DdcmpDevice dev)
    : Ddcmp (owner, std::move (name), std::move (dev))
{
    if (!termios_speed (dev_.speed))
        throw std::invalid_argument ("DDCMP serial: unsupported speed "
                                     + std::to_string (dev_.speed));
}

void SerialDdcmp::connect ()
{
    fd_ = ::open (dev_.destination.c_str (), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
        DN_ERROR ("{}: cannot open {}: {}", name_, dev_.destination,
                  std::strerror (errno));
        return;
    }

    termios t {};
    if (::tcgetattr (fd_, &t) != 0) {
        DN_ERROR ("{}: {} is not a terminal: {}", name_, dev_.destination,
                  std::strerror (errno));
        ::close (fd_);
        fd_ = -1;
        return;
    }

    // Raw 8N1, no flow control.
    ::cfmakeraw (&t);
    t.c_cflag |= CLOCAL | CREAD;        // ignore modem lines, enable receive
    t.c_cflag &= ~static_cast<tcflag_t> (CSTOPB | PARENB | CRTSCTS);
    t.c_cflag = (t.c_cflag & ~static_cast<tcflag_t> (CSIZE)) | CS8;
    t.c_iflag &= ~static_cast<tcflag_t> (IXON | IXOFF | IXANY);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;

    speed_t sp = termios_speed (dev_.speed);
    ::cfsetispeed (&t, sp);
    ::cfsetospeed (&t, sp);

    if (::tcsetattr (fd_, TCSANOW, &t) != 0) {
        DN_ERROR ("{}: cannot configure {}: {}", name_, dev_.destination,
                  std::strerror (errno));
        ::close (fd_);
        fd_ = -1;
        return;
    }
    ::tcflush (fd_, TCIOFLUSH);
    DN_DEBUG ("{}: opened {} at {} baud", name_, dev_.destination, dev_.speed);
}

void SerialDdcmp::disconnect ()
{
    if (fd_ >= 0) { ::close (fd_); fd_ = -1; }
}

bool SerialDdcmp::check_connection ()
{
    // Nothing to establish on a serial line.
    return fd_ >= 0;
}

Bytes SerialDdcmp::read_line (std::size_t n)
{
    Bytes out;
    out.reserve (n);
    while (out.size () < n) {
        if (stopping ()) throw std::runtime_error ("stop requested");
        PollResult p = poll_socket (fd_, true, false, poll_timeout_ms);
        if (p.error) throw std::runtime_error ("serial line error");
        if (p.timeout || !p.readable) continue;

        std::uint8_t buf[512];
        std::size_t want = std::min (n - out.size (), sizeof buf);
        ssize_t got = ::read (fd_, buf, want);
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            throw std::runtime_error ("serial read failed");
        }
        if (got == 0) continue;         // nothing yet; a tty is not a socket
        out.insert (out.end (), buf, buf + got);
    }
    return out;
}

void SerialDdcmp::receive_loop ()
{
    for (;;) {
        if (stopping ()) return;
        try {
            Bytes msg = read_framed_message (
                [this] (std::size_t n) { return read_line (n); });
            if (node ())
                node ()->add_work (std::make_unique<Received> (this,
                                                               std::move (msg)));
        } catch (const std::exception &) {
            return;
        }
    }
}

void SerialDdcmp::transmit (const Message &m)
{
    if (fd_ < 0) return;
    Bytes wire = m.encode ();
    // One all-ones byte after the trailer.  No leading SYN bytes on an
    // asynchronous line, per the spec.
    wire.push_back (ddcmp::DEL);

    std::size_t sent = 0;
    while (sent < wire.size ()) {
        ssize_t n = ::write (fd_, wire.data () + sent, wire.size () - sent);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return;
        }
        sent += static_cast<std::size_t> (n);
    }
}

}   // namespace decnet::datalink
