#include "decnet/datalink/multinet.h"

#include "decnet/common/logging.h"
#include "decnet/node.h"

#include "decnet/datalink/datalink.h"

#include <chrono>
#include <charconv>
#include <cstring>
#include <stdexcept>

namespace decnet::datalink {

namespace {

// The four byte Multinet header.
constexpr std::size_t hdrlen = 4;

// Largest accepted frame.  Limits allocation from a bad peer.
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
    // Equivalent to PyDECnet's (.*?):(\d*)(?:(:connect)|(:listen)|(:\d+))?$,
    // parsed from the right.
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
    else if (tail == "server") d.mode = Mode::server;
    else if (tail.empty ())    d.mode = Mode::udp;
    else {
        d.mode = Mode::udp;
        if (!to_port (tail, d.source_port))
            throw std::invalid_argument ("invalid Multinet device: " + device);
    }

    if (d.mode == Mode::listen || d.mode == Mode::server) {
        // In listen and server mode the port we were given is the one we
        // bind.  For a server the host, if any, is the address to bind:
        // the peers are whoever connects.
        d.source_port = d.dest_port;
        d.dest_port = 0;
    } else if (d.mode == Mode::udp) {
        // Local and remote ports are the same unless told otherwise.
        if (!d.source_port) d.source_port = d.dest_port;
    }

    if (d.mode == Mode::server && d.source_port == 0)
        throw std::invalid_argument ("Multinet server needs a port in "
                                     + device);
    if (d.mode != Mode::listen && d.mode != Mode::server
        && d.destination.empty ())
        throw std::invalid_argument ("Multinet needs a destination host in "
                                     + device);
    return d;
}

std::string MultinetDevice::str () const
{
    const char *m = mode == Mode::connect ? "connect"
                  : mode == Mode::listen  ? "listen"
                  : mode == Mode::server  ? "server" : "udp";
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
        // PyDECnet warns here, and it is right to: UDP Multinet violates
        // most of the point to point datalink requirements.
        DN_WARN ("Multinet UDP mode is not recommended: it violates the "
                 "DECnet architecture");
        return std::make_unique<UdpMultinet> (owner, name, dev, source_host);
    case MultinetDevice::Mode::server:
        // Not a circuit: the datalink layer makes a MultinetServer, which
        // makes the circuits.
        throw std::invalid_argument ("Multinet server mode is not a single "
                                     "circuit");
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
        // Pass the frame up via the state machine, on the node thread.
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

namespace {

// Frames waiting for the connection, at most.  A peer that has stopped
// reading for this long has frames dropped, whole, as a lossy line would;
// NSP sends again what mattered.
constexpr std::size_t max_backlog = 256 * 1024;

// Frames buffered, at most, before they are written there and then rather
// than when the node thread gets to it.  A burst no bigger than this costs
// one write.
constexpr std::size_t cork_limit = 32 * 1024;

Socket wake_socket ()
{
    Socket s = SourceAddress ("127.0.0.1", 0).bind_socket (AF_INET, SOCK_DGRAM);
    if (!s) return s;
    sockaddr_storage sa {};
    socklen_t len = sizeof sa;
    if (::getsockname (s.fd (), reinterpret_cast<sockaddr *> (&sa), &len) < 0
        || ::connect (s.fd (), reinterpret_cast<const sockaddr *> (&sa), len) < 0)
        return Socket {};
    s.set_nonblocking ();
    return s;
}

}   // namespace

TcpMultinet::TcpMultinet (Element *owner, std::string name, MultinetDevice dev,
                          std::string source_host)
    : Multinet (owner, std::move (name), std::move (dev), std::move (source_host)),
      wake_ (wake_socket ())
{
}

void TcpMultinet::disconnect ()
{
    {
        // What the connection takes now still goes; the rest was for this
        // connection, and a frame half written cannot be finished on the
        // next.
        std::lock_guard l (txm_);
        if (socket_ && !blocked_) flush ();
        txbuf_.clear ();
        txoff_ = 0;
        blocked_ = false;
    }
    if (socket_) {
        socket_.shutdown ();
        socket_.close ();
    }
}

bool TcpMultinet::flush ()
{
    while (txoff_ < txbuf_.size ()) {
        ssize_t n = sock_send (socket_.fd (), txbuf_.data () + txoff_,
                               txbuf_.size () - txoff_);
        if (n < 0) {
            int err = sock_errno ();
            if (sock_interrupted (err)) continue;
            if (!sock_would_block (err)) return false;
            n = 0;
        }
        if (n == 0) {
            // Full.  Keep only what is still to go.
            blocked_ = true;
            txbuf_.erase (txbuf_.begin (),
                          txbuf_.begin () + static_cast<std::ptrdiff_t> (txoff_));
            txoff_ = 0;
            return true;
        }
        txoff_ += static_cast<std::size_t> (n);
    }
    txbuf_.clear ();
    txoff_ = 0;
    blocked_ = false;
    return true;
}

void TcpMultinet::flush_posted ()
{
    bool ok = true, blocked;
    {
        std::lock_guard l (txm_);
        posted_ = false;
        // Blocked, the receive thread writes it as the connection drains.
        if (!blocked_ && txoff_ < txbuf_.size ()) ok = flush ();
        blocked = blocked_;
    }
    if (!ok) {
        DN_TRACE ("send error on {}, reconnecting", name_);
        reconnect ();
    } else if (blocked) {
        wake ();
    }
}

void TcpMultinet::wake ()
{
    if (!wake_) return;
    ssize_t ignored = sock_send (wake_.fd (), "w", 1);
    (void) ignored;
}

void TcpMultinet::receive_loop ()
{
    // The tail of what was read: not yet a whole frame.
    Bytes in;
    std::vector<std::uint8_t> buf (64 * 1024);
    std::vector<WorkPtr> frames;
    for (;;) {
        bool blocked;
        {
            std::lock_guard l (txm_);
            blocked = blocked_;
        }
        pollfd p[2] {};
        p[0].fd = socket_.fd ();
        p[0].events = static_cast<short> (POLLIN | (blocked ? POLLOUT : 0));
        std::size_t nfds = 1;
        if (wake_) {
            p[1].fd = wake_.fd ();
            p[1].events = POLLIN;
            nfds = 2;
        }
        // The wake socket, or failing that the timeout, brings it back to
        // a connection that has filled meanwhile.
        int n = sock_poll (p, nfds, poll_timeout_ms);
        if (stopping ()) return;
        if (n < 0) {
            if (sock_interrupted (sock_errno ())) continue;
            DN_TRACE ("receive loop on {} ending: poll failed", name_);
            return;
        }
        if (n == 0) continue;

        if (nfds == 2 && p[1].revents) {
            std::uint8_t junk[64];
            while (sock_recv (wake_.fd (), junk, sizeof junk) > 0) {}
        }
        if (p[0].revents & (POLLERR | POLLNVAL)) {
            DN_TRACE ("receive loop on {} ending: socket error", name_);
            return;
        }
        if (p[0].revents & POLLOUT) {
            std::lock_guard l (txm_);
            if (!flush ()) {
                DN_TRACE ("receive loop on {} ending: send failed", name_);
                return;
            }
        }
        if (!(p[0].revents & (POLLIN | POLLHUP))) continue;

        ssize_t got = sock_recv (socket_.fd (), buf.data (), buf.size ());
        if (got < 0) {
            int err = sock_errno ();
            if (sock_would_block (err) || sock_interrupted (err)) continue;
        }
        if (got <= 0) {
            DN_TRACE ("receive loop on {} ending: connection closed", name_);
            return;
        }

        // Frames come straight from what was just read, unless a frame was
        // left over from the last read to complete.
        const std::uint8_t *data = buf.data ();
        std::size_t len = static_cast<std::size_t> (got);
        if (!in.empty ()) {
            in.insert (in.end (), data, data + len);
            data = in.data ();
            len = in.size ();
        }

        // Pass up every whole frame, all in one go.  The length is 16 bits,
        // so never more than max_frame.
        std::size_t at = 0;
        while (len - at >= hdrlen) {
            std::size_t bc = static_cast<std::size_t> (data[at])
                           | (static_cast<std::size_t> (data[at + 1]) << 8);
            if (len - at < hdrlen + bc) break;
            const std::uint8_t *from = data + at + hdrlen;
            frames.push_back (std::make_unique<Received> (
                this, Bytes (from, from + bc)));
            at += hdrlen + bc;
        }
        if (node ()) node ()->add_work (frames);
        frames.clear ();

        if (data == in.data ())
            in.erase (in.begin (), in.begin () + static_cast<std::ptrdiff_t> (at));
        else
            in.assign (data + at, data + len);
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

    bool ok = true, blocked = false, post = false;
    {
        std::lock_guard l (txm_);
        std::size_t waiting = txbuf_.size () - txoff_;
        if (waiting + hdrlen + mlen > max_backlog) {
            DN_DEBUG ("send on {} dropped: {} bytes already waiting for the "
                      "peer to read", name_, waiting);
            return;
        }
        const std::uint8_t hdr[hdrlen] = {
            static_cast<std::uint8_t> (mlen & 0xff),
            static_cast<std::uint8_t> (mlen >> 8), 0, 0 };
        txbuf_.insert (txbuf_.end (), hdr, hdr + hdrlen);
        txbuf_.insert (txbuf_.end (), msg.begin (), msg.end ());

        if (blocked_) {
            // The receive thread writes it as the connection drains.
        } else if (waiting + hdrlen + mlen >= cork_limit) {
            ok = flush ();
            blocked = blocked_;
        } else if (!posted_) {
            // Written by the node thread once the work already queued is
            // done: whatever else is sent meanwhile goes in the same write.
            posted_ = true;
            post = true;
        }
    }
    if (!ok) {
        DN_TRACE ("send error on {}, reconnecting", name_);
        reconnect ();
        return;
    }
    counters_.bytes_sent += mlen;
    ++counters_.pkts_sent;
    if (blocked) wake ();
    if (post && node ())
        node ()->add_work (std::make_unique<CallbackWork> (
            [this] { flush_posted (); }));
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

    // Connect timeout.  Expiry leads to a reconnect with backoff.
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
                      sock_strerror (err));
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

        Socket conn (sock_accept (listener_.fd ()));
        if (!conn) continue;
        // Non-blocking, as the send path needs.  Windows hands the
        // listener's mode on to the new socket; POSIX does not.
        conn.set_nonblocking ();

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
    // UDP sequence number.  Receivers ignore it.
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

// ------------------------------------------------------ AcceptedMultinet

AcceptedMultinet::AcceptedMultinet (Element *owner, std::string name,
                                    MultinetDevice dev, Socket conn,
                                    std::string peer, RetireFn retire)
    : TcpMultinet (owner, std::move (name), std::move (dev)),
      peer_ (std::move (peer)), retire_ (std::move (retire))
{
    // The receive thread starts only when the port is opened, so this is
    // set well before anything reads it.
    socket_ = std::move (conn);
}

bool AcceptedMultinet::validate (Work &w)
{
    if (finished_) return false;
    if (dynamic_cast<Received *> (&w)) return true;

    // Every request to stop, restart or reconnect means the same thing
    // here, since there is nobody to reconnect to: close.  The routing
    // circuit asked, so it does not need telling the link went down.
    if (dynamic_cast<Stop *> (&w) || dynamic_cast<Restart *> (&w)
        || dynamic_cast<Reconnect *> (&w)) {
        owner_knows_ = true;
        close_now ();
        return false;
    }
    if (dynamic_cast<ThreadExit *> (&w)) {
        // The peer went away, or we asked the thread to stop.
        finish ();
        return false;
    }
    // Start, Connected and the rest go to the states as usual.
    return true;
}

void AcceptedMultinet::close_now ()
{
    if (closing_) return;
    closing_ = true;
    if (thread_started ())
        stop_thread (false);    // its ThreadExit brings us to finish ()
    else
        finish ();
}

void AcceptedMultinet::finish ()
{
    if (finished_) return;
    finished_ = true;
    closing_ = true;
    if (thread_started ()) stop_thread (true);
    disconnect ();
    if (!owner_knows_) report_down ();
    if (node ()) node ()->timers ().stop (this);
    DN_DEBUG ("Multinet {} connection from {} closed", name_, peer_);
    // After whatever this has queued for the routing circuit, so that it
    // sees the link go down before it is removed.
    if (node () && retire_)
        node ()->add_work (std::make_unique<CallbackWork> (
            [fn = retire_, n = name_] { fn (n); }));
}

// -------------------------------------------------------- MultinetServer

MultinetServer::MultinetServer (Element *owner, const CircuitConfig &c,
                                MultinetDevice dev, std::set<std::string> used)
    : Element (owner), config_ (c), dev_ (std::move (dev)),
      source_ (dev_.destination, dev_.source_port), used_ (std::move (used))
{
    for (const std::string &h : config_.allow)
        allow_.emplace_back (h, 0);
    DN_DEBUG ("Multinet server {} initialized: {}, at most {} connections{}",
              config_.name, dev_.str (), config_.max_connections,
              allow_.empty () ? std::string (", from anywhere")
                              : ", from " + std::to_string (allow_.size ())
                                    + " allowed host(s)");
}

MultinetServer::~MultinetServer ()
{
    stop ();
}

void MultinetServer::start ()
{
    if (thread_.joinable ()) return;
    stopping_.store (false);
    thread_ = std::thread ([this] { accept_loop (); });
}

void MultinetServer::stop ()
{
    stopping_.store (true);
    if (thread_.joinable ()) thread_.join ();
    listener_.close ();
}

bool MultinetServer::allowed (const Endpoint &peer)
{
    if (allow_.empty ()) return true;
    for (HostAddress &a : allow_) {
        a.resolve ();
        if (a.valid (peer)) return true;
    }
    return false;
}

void MultinetServer::accept_loop ()
{
    logging::set_thread_name ((node () ? node ()->name () + "." : "")
                              + config_.name);
    using clock = std::chrono::steady_clock;
    // Refused connections are held this long, at most this many at once.
    constexpr auto        hold_for = std::chrono::seconds (10);
    constexpr std::size_t max_held = 16;
    bool warned = false;
    while (!stopping_.load ()) {
        const auto now = clock::now ();
        std::erase_if (held_, [&] (const Held &h) { return h.until <= now; });

        // Full: leave callers in the kernel's queue until a slot is free.
        if (claimed_.load () >= config_.max_connections) {
            std::this_thread::sleep_for (std::chrono::milliseconds (100));
            continue;
        }

        if (!listener_) {
            listener_ = source_.create_server ();
            if (!listener_) {
                if (!warned)
                    DN_WARN ("Multinet server {} cannot listen on {}; "
                             "retrying", config_.name, source_.str ());
                warned = true;
                // Try again in a few seconds, noticing a stop promptly.
                for (int i = 0; i < 100 && !stopping_.load (); ++i)
                    std::this_thread::sleep_for (std::chrono::milliseconds (50));
                continue;
            }
            listener_.set_nonblocking ();
            warned = false;
            DN_INFO ("Multinet server {} listening on {}", config_.name,
                     source_.str ());
        }

        PollResult p = poll_socket (listener_.fd (), true, false,
                                    poll_timeout_ms);
        if (stopping_.load ()) break;
        if (p.error) {
            DN_DEBUG ("Multinet server {} listener failed, reopening",
                      config_.name);
            listener_.close ();
            continue;
        }
        if (p.timeout || !p.readable) continue;

        Socket conn (sock_accept (listener_.fd ()));
        if (!conn) continue;
        conn.set_nonblocking ();
        Endpoint peer = peer_of (conn.fd ());
        if (!allowed (peer)) {
            // Logged once a minute per host: the port changes every time.
            std::string host = peer.str ();
            if (auto c = host.rfind (':'); c != std::string::npos)
                host.erase (c);
            auto &last = logged_[host];
            if (last == clock::time_point {} || now - last >= std::chrono::minutes (1)) {
                DN_INFO ("Multinet server {} refused connection from {}: not "
                         "an allowed host", config_.name, peer.str ());
                last = now;
            }
            // Hold it open a while, so a caller that redials as soon as it
            // is dropped does so every ten seconds and not every moment.
            if (held_.size () < max_held)
                held_.push_back ({ std::move (conn), now + hold_for });
            continue;           // otherwise conn closes here
        }
        conn.set_nodelay ();
        claimed_.fetch_add (1);
        // Hand it to the node thread, which owns the circuit tables.  The
        // socket goes in a shared_ptr only because the work item must be
        // copyable.
        auto sp = std::make_shared<Socket> (std::move (conn));
        std::string desc = peer.str ();
        if (node ())
            node ()->add_work (std::make_unique<CallbackWork> (
                [this, sp, desc] { accepted (sp, desc); }));
    }
    listener_.close ();
    held_.clear ();
}

std::string MultinetServer::next_name () const
{
    auto *dll = static_cast<DatalinkLayer *> (parent ());
    for (unsigned n = 0; ; ++n) {
        std::string name = config_.name + "-" + std::to_string (n);
        if (used_.count (name) || live_.count (name)) continue;
        if (dll && dll->circuit (name)) continue;
        return name;
    }
}

void MultinetServer::accepted (const std::shared_ptr<Socket> &conn,
                               const std::string &peer)
{
    auto *dll = static_cast<DatalinkLayer *> (parent ());
    DynamicCircuitOwner *owner = dll ? dll->dynamic_owner () : nullptr;
    if (!owner) {
        DN_INFO ("Multinet server {} refused connection from {}: no "
                 "routing layer to take it", config_.name, peer);
        claimed_.fetch_sub (1);
        return;                 // the socket closes with the last reference
    }
    // The listening thread has already taken a slot for this connection,
    // so the count can only be over if something is amiss: refuse rather
    // than exceed --max.
    if (live_.size () >= config_.max_connections) {
        DN_WARN ("Multinet server {} refused connection from {}: already "
                 "{} connections (--max {})", config_.name, peer,
                 live_.size (), config_.max_connections);
        claimed_.fetch_sub (1);
        return;
    }

    std::string name = next_name ();
    auto dl = std::make_unique<AcceptedMultinet> (
        dll, name, dev_, std::move (*conn), peer,
        [this] (const std::string &n) { retire (n); });
    Datalink *raw = dl.get ();
    dll->add_circuit (std::move (dl));

    CircuitConfig c = config_;
    c.name = name;
    c.wait_for_init = true;
    if (!owner->add_dynamic_circuit (c, raw)) {
        // Nothing has been started, so nothing can refer to it yet.
        dll->remove_circuit (name);
        DN_WARN ("Multinet server {} could not make a circuit for {}",
                 config_.name, peer);
        claimed_.fetch_sub (1);
        return;
    }
    live_.insert (name);
    DN_INFO ("Multinet server {}: connection from {} is circuit {}",
             config_.name, peer, name);
}

void MultinetServer::retire (const std::string &name)
{
    auto *dll = static_cast<DatalinkLayer *> (parent ());
    if (!live_.erase (name)) return;
    claimed_.fetch_sub (1);
    if (!dll) return;
    std::unique_ptr<Datalink> dl = dll->remove_circuit (name);
    if (auto *am = dynamic_cast<AcceptedMultinet *> (dl.get ()))
        am->mark_retired ();
    if (DynamicCircuitOwner *owner = dll->dynamic_owner ())
        owner->retire_dynamic_circuit (name, std::move (dl));
    // With no routing layer left to hold it, dl goes here: nothing else
    // refers to it then.
    DN_INFO ("Multinet server {}: circuit {} removed", config_.name, name);
}

}   // namespace decnet::datalink
