// src/datalink/ddcmp_link.cc -- the DDCMP datalink and its transports.
// Port of the _DDCMP transport subclasses in ddcmp.py.

#include "decnet/datalink/ddcmp.h"

#include "decnet/common/logging.h"
#include "decnet/common/work.h"
#include "decnet/node.h"

#include <sstream>
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
        // A payload the far end acknowledged and put in order.  Everything
        // above this point sees a reliable message stream, which is the
        // whole point of DDCMP.
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
    default:
        // PORT: tcp, telnet and serial follow.  The protocol engine is
        // transport independent, so each is a matter of moving bytes:
        // a stream needs ddcmp::find_header on receive, which is written
        // and tested, and telnet additionally escapes the all-ones byte.
        throw std::invalid_argument ("DDCMP " + d.str ()
                                     + ": only udp is implemented so far");
    }
}

Ddcmp::State Ddcmp::connected ()
{
    // The transport is up; now the protocol has its own handshake to do.
    // The circuit is not reported up until that finishes, which is what
    // the engine's "up" hook does.
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
            // A datagram that is not a DDCMP message at all.  On a stream
            // this would mean "keep looking"; on a datagram there is
            // nothing to look through, so it is simply dropped.
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

// ------------------------------------------------------------------ UDP

UdpDdcmp::UdpDdcmp (Element *owner, std::string name, DdcmpDevice dev)
    : Ddcmp (owner, std::move (name), std::move (dev))
{
}

void UdpDdcmp::connect ()
{
    // Bound, never connected: a connected datagram socket reports ICMP
    // errors, and a bounced datagram would then kill the receive loop.
    // See BUGS.md -- this is the same hazard Multinet's UDP mode had.
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

}   // namespace decnet::datalink
