#include "decnet/datalink/multinet.h"

#include "decnet/common/logging.h"
#include "decnet/node.h"

#include <charconv>
#include <cstring>
#include <stdexcept>

#include <sys/socket.h>

namespace decnet::datalink {

namespace {

// The four byte Multinet header.
constexpr std::size_t hdrlen = 4;

// The largest frame we will accept.  Multinet carries routing layer
// packets, which are far smaller; this only bounds what a confused or
// hostile peer can make us allocate.
constexpr std::size_t max_frame = 65535;

bool to_port (std::string_view s, std::uint16_t &out)
{
    unsigned v = 0;
    auto [p, ec] = std::from_chars (s.data (), s.data () + s.size (), v);
    if (ec != std::errc () || p != s.data () + s.size () || v > 65535)
        return false;
    out = static_cast<std::uint16_t> (v);
    return true;
}

}   // namespace

// -------------------------------------------------------- device parsing

MultinetDevice MultinetDevice::parse (const std::string &device)
{
    // the Python's regex is (.*?):(\d*)(?:(:connect)|(:listen)|(:\d+))?$ --
    // a non-greedy host, then a port, then an optional mode or local port.
    // Splitting from the right is the same thing and is easier to read.
    MultinetDevice d;
    std::vector<std::string> parts;
    std::string cur;
    for (char c : device) {
        if (c == ':') { parts.push_back (cur); cur.clear (); }
        else            cur += c;
    }
    parts.push_back (cur);

    if (parts.size () < 2 || parts.size () > 3)
        throw std::invalid_argument ("invalid Multinet device: " + device);

    d.destination = parts[0];

    std::string port_part = parts[1];
    std::string tail      = parts.size () == 3 ? parts[2] : std::string ();

    if (port_part.empty ()) d.dest_port = 700;
    else if (!to_port (port_part, d.dest_port))
        throw std::invalid_argument ("invalid Multinet port in: " + device);

    if (tail == "connect")     d.mode = Mode::connect;
    else if (tail == "listen") d.mode = Mode::listen;
    else if (tail.empty ())    d.mode = Mode::udp;
    else {
        d.mode = Mode::udp;
        if (!to_port (tail, d.source_port))
            throw std::invalid_argument ("invalid Multinet device: " + device);
    }

    if (d.mode == Mode::listen) {
        // In listen mode the port we were given is the one we bind.
        d.source_port = d.dest_port;
        d.dest_port = 0;
    } else if (d.mode == Mode::udp) {
        // Local and remote ports are the same unless told otherwise.
        if (!d.source_port) d.source_port = d.dest_port;
    }

    if (d.mode != Mode::listen && d.destination.empty ())
        throw std::invalid_argument ("Multinet needs a destination host in "
                                     + device);
    return d;
}

std::string MultinetDevice::str () const
{
    const char *m = mode == Mode::connect ? "connect"
                  : mode == Mode::listen  ? "listen" : "udp";
    return std::string (m) + " dest " + (destination.empty () ? "*" : destination)
        + ":" + std::to_string (dest_port)
        + " source :" + std::to_string (source_port);
}

// -------------------------------------------------------------- Multinet

Multinet::Multinet (Element *owner, std::string name, MultinetDevice dev,
                    std::string source_host)
    : PtpDatalink (owner, std::move (name)),
      dev_ (std::move (dev)),
      source_ (std::move (source_host), dev_.source_port),
      dest_ (dev_.destination, dev_.dest_port)
{
    DN_DEBUG ("Multinet datalink {} initialized: {}", name_, dev_.str ());
}

std::unique_ptr<Datalink> Multinet::create (Element *owner,
                                            const std::string &name,
                                            const std::string &device,
                                            const std::string &source_host)
{
    MultinetDevice dev = MultinetDevice::parse (device);
    switch (dev.mode) {
    case MultinetDevice::Mode::connect:
        return std::make_unique<ConnectMultinet> (owner, name, dev, source_host);
    case MultinetDevice::Mode::listen:
        return std::make_unique<ListenMultinet> (owner, name, dev, source_host);
    case MultinetDevice::Mode::udp:
        // the Python warns here, and it is right to: UDP Multinet violates
        // most of the point to point datalink requirements.
        DN_WARN ("Multinet UDP mode is not recommended: it violates the "
                 "DECnet architecture");
        return std::make_unique<UdpMultinet> (owner, name, dev, source_host);
    }
    return nullptr;
}

PtpDatalink::State Multinet::connected ()
{
    // There is no datalink protocol to start, so the connection being made
    // is the circuit coming up.
    report_up ();
    return DN_MY_STATE (PtpDatalink, running);
}

PtpDatalink::State Multinet::running (Work &w)
{
    if (auto *r = dynamic_cast<Received *> (&w)) {
        // Pass the frame up.  It comes through the state machine rather
        // than straight from the receive thread so that everything the
        // layer above sees is serialised on the node thread.
        if (port_) {
            counters_.bytes_recv += r->packet ().size ();
            ++counters_.pkts_recv;
            node ()->add_work (
                std::make_unique<Received> (port_->owner (),
                                            std::move (r->packet ())));
        } else {
            DN_TRACE ("message discarded, no port open on {}", name_);
        }
        return nullptr;
    }
    if (dynamic_cast<ThreadExit *> (&w)) {
        reconnect ();
        return nullptr;
    }
    return nullptr;
}

// ----------------------------------------------------------- TcpMultinet

void TcpMultinet::disconnect ()
{
    if (socket_) {
        socket_.shutdown ();
        socket_.close ();
    }
}

void TcpMultinet::receive_loop ()
{
    for (;;) {
        Bytes msg;
        try {
            Bytes hdr = recvall (hdrlen);
            std::size_t bc = static_cast<std::size_t> (hdr[0])
                           | (static_cast<std::size_t> (hdr[1]) << 8);
            if (bc > max_frame) {
                DN_TRACE ("oversized Multinet frame ({}) on {}", bc, name_);
                return;
            }
            msg = recvall (bc);
        } catch (const std::exception &e) {
            DN_TRACE ("receive loop on {} ending: {}", name_, e.what ());
            return;
        }
        if (node ())
            node ()->add_work (std::make_unique<Received> (this,
                                                           std::move (msg)));
    }
}

void TcpMultinet::send (Bytes msg)
{
    if (!socket_ || !in_state (DN_MY_STATE (PtpDatalink, running))) {
        DN_TRACE ("send on {} dropped: not running", name_);
        return;
    }
    std::size_t mlen = msg.size ();
    if (mlen > max_frame) {
        DN_DEBUG ("send on {} dropped: frame too long ({})", name_, mlen);
        return;
    }
    counters_.bytes_sent += mlen;
    ++counters_.pkts_sent;

    Bytes frame;
    frame.reserve (hdrlen + mlen);
    frame.push_back (static_cast<std::uint8_t> (mlen & 0xff));
    frame.push_back (static_cast<std::uint8_t> (mlen >> 8));
    frame.push_back (0);
    frame.push_back (0);
    frame.insert (frame.end (), msg.begin (), msg.end ());

    // One write for the header and payload together: splitting them would
    // let a peer see a torn frame if the connection drops between the two.
    std::size_t off = 0;
    while (off < frame.size ()) {
        ssize_t n = ::send (socket_.fd (), frame.data () + off,
                            frame.size () - off, MSG_NOSIGNAL);
        if (n <= 0) {
            DN_TRACE ("send error on {}, reconnecting", name_);
            reconnect ();
            return;
        }
        off += static_cast<std::size_t> (n);
    }
}

// ------------------------------------------------------- ConnectMultinet

void ConnectMultinet::connect ()
{
    socket_ = create_connection (dest_, source_);
    if (socket_)
        DN_TRACE ("Multinet {} connect to {} in progress", name_, dest_.str ());
    else
        DN_TRACE ("Multinet {} connect to {} rejected", name_, dest_.str ());

    // Try the next address next time, if the name gave us several.
    dest_.advance ();

    // Bound the wait for the connection to complete.  A failed connect is
    // not retried here; the timer expiry turns into a reconnect, so a host
    // that is down is retried at a decreasing rate rather than in a spin.
    if (node ()) node ()->timers ().jstart (this, conn_timer_.next ());
}

bool ConnectMultinet::check_connection ()
{
    if (!socket_) return false;
    for (;;) {
        PollResult p = poll_socket (socket_.fd (), false, true, poll_timeout_ms);
        if (stopping ()) return false;
        if (p.error) return false;
        if (p.timeout) continue;
        if (!p.writable) continue;
        // Writable means the connect finished, successfully or not.
        int err = socket_.socket_error ();
        if (err) {
            DN_TRACE ("Multinet {} connect failed: {}", name_,
                      std::strerror (err));
            return false;
        }
        DN_TRACE ("Multinet {} connected", name_);
        return true;
    }
}

// -------------------------------------------------------- ListenMultinet

void ListenMultinet::connect ()
{
    listener_ = source_.create_server ();
    if (!listener_) {
        DN_TRACE ("Multinet {} bind {} failed", name_, source_.str ());
        // Use the connect timer as a retry holdoff.
        if (node ()) node ()->timers ().jstart (this, conn_timer_.next ());
        return;
    }
    listener_.set_nonblocking ();
    DN_TRACE ("Multinet {} listening on {}", name_, source_.str ());
}

bool ListenMultinet::check_connection ()
{
    if (!listener_) return false;
    // Make sure we know what addresses the peer name maps to before we
    // start checking against it.
    dest_.resolve ();

    for (;;) {
        PollResult p = poll_socket (listener_.fd (), true, false,
                                    poll_timeout_ms);
        if (stopping ()) return false;
        if (p.error) return false;
        if (p.timeout || !p.readable) continue;

        Socket conn (::accept (listener_.fd (), nullptr, nullptr));
        if (!conn) continue;

        Endpoint peer = peer_of (conn.fd ());
        if (!dest_.valid (peer)) {
            DN_TRACE ("Multinet {} connection from unexpected address {}",
                      name_, peer.str ());
            continue;           // conn closes here
        }
        DN_TRACE ("Multinet {} connected from {}", name_, peer.str ());
        conn.set_nodelay ();
        // Stop listening; the data socket is what matters now.
        listener_.close ();
        socket_ = std::move (conn);
        return true;
    }
}

// ----------------------------------------------------------- UdpMultinet

Port *UdpMultinet::create_port (Element *owner)
{
    Port *p = PtpDatalink::create_port (owner);
    return p;
}

void UdpMultinet::connect ()
{
    socket_ = create_udp (dest_, source_);
    if (socket_)
        DN_TRACE ("Multinet {} (UDP) bound to {}", name_, source_.str ());
    else
        DN_TRACE ("Multinet {} (UDP) bind to {} failed", name_,
                  source_.str ());
}

void UdpMultinet::disconnect ()
{
    socket_.close ();
}

bool UdpMultinet::check_connection ()
{
    // Connectionless: there is nothing to wait for.
    return socket_.valid ();
}

void UdpMultinet::receive_loop ()
{
    std::uint8_t buf[1500];
    for (;;) {
        PollResult p = poll_socket (socket_.fd (), true, false, poll_timeout_ms);
        if (stopping ()) return;
        if (p.error) {
            // Transient, as on any datagram socket; see the Ethernet
            // receive loop for why this is not fatal.
            DN_TRACE ("transient error on {}, continuing", name_);
            continue;
        }
        if (p.timeout || !p.readable) continue;

        Endpoint from;
        ssize_t n = recv_datagram (socket_.fd (), buf, sizeof buf, from);
        if (n < 0) continue;
        if (!dest_.valid (from)) continue;
        // A datagram must have the four byte header and something after it.
        if (n <= static_cast<ssize_t> (hdrlen)) continue;
        Bytes msg (buf + hdrlen, buf + n);
        if (node ())
            node ()->add_work (std::make_unique<Received> (this,
                                                           std::move (msg)));
    }
}

void UdpMultinet::send (Bytes msg)
{
    if (!socket_ || !in_state (DN_MY_STATE (PtpDatalink, running))) {
        DN_TRACE ("send on {} dropped: not running", name_);
        return;
    }
    counters_.bytes_sent += msg.size ();
    ++counters_.pkts_sent;

    Bytes frame;
    frame.reserve (hdrlen + msg.size ());
    // Over UDP the first two bytes are a sequence number, which the
    // receiver ignores -- the Linux implementation does not look at it
    // either -- but we count it up anyway to match what a peer expects.
    frame.push_back (static_cast<std::uint8_t> (seq_ & 0xff));
    frame.push_back (static_cast<std::uint8_t> (seq_ >> 8));
    frame.push_back (0);
    frame.push_back (0);
    ++seq_;
    frame.insert (frame.end (), msg.begin (), msg.end ());

    const Endpoint *to = dest_.destination ();
    if (!to
        || !send_datagram (socket_.fd (),
                           ByteView (frame.data (), frame.size ()), *to))
        DN_TRACE ("send error on {}", name_);
}

}   // namespace decnet::datalink
