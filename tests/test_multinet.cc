// Port of tests/test_multinet.py.
//
// Device string parsing, plus two nodes exchanging traffic over TCP.
//
// No routing line, so the tests can use the circuit's port directly.

#include "harness.h"

#include "decnet/common/logging.h"
#include "decnet/config.h"
#include "decnet/datalink/multinet.h"
#include "decnet/node.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace decnet;
using namespace decnet::datalink;
using Mode = MultinetDevice::Mode;

namespace {

Bytes bytes_of (std::initializer_list<int> v)
{
    Bytes b;
    for (int x : v) b.push_back (static_cast<std::uint8_t> (x));
    return b;
}

// A stand-in for the routing layer: records what the datalink reports.
class TestOwner : public Element {
public:
    explicit TestOwner (Node *n) : Element (n) {}

    void dispatch (Work &w) override
    {
        std::lock_guard lock (mutex_);
        if (auto *s = dynamic_cast<DlStatus *> (&w)) {
            if (s->is_up ()) ++ups_; else ++downs_;
        } else if (auto *r = dynamic_cast<Received *> (&w)) {
            received_.push_back (r->packet ());
        }
        cond_.notify_all ();
    }

    // Wait until pred() holds or the deadline passes.  Returns whether it
    // held.
    template <typename P>
    bool wait_for (P pred, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock (mutex_);
        return cond_.wait_for (lock, timeout, [&] { return pred (); });
    }

    int ups () { std::lock_guard l (mutex_); return ups_; }
    int downs () { std::lock_guard l (mutex_); return downs_; }

    std::vector<Bytes> received ()
    { std::lock_guard l (mutex_); return received_; }

    // These are called with the lock held by wait_for's predicate.
    int ups_unlocked () const { return ups_; }
    std::size_t count_unlocked () const { return received_.size (); }

private:
    std::mutex              mutex_;
    std::condition_variable cond_;
    int                     ups_ = 0, downs_ = 0;
    std::vector<Bytes>      received_;
};

// Pick a port that is free right now.  Tests must not collide with
// whatever else is running on the machine.
std::uint16_t free_port ()
{
    SourceAddress any ("127.0.0.1", 0);
    Socket s = any.create_server ();
    if (!s) throw std::runtime_error ("cannot find a free port");
    sockaddr_storage sa {};
    socklen_t len = sizeof sa;
    ::getsockname (s.fd (), reinterpret_cast<sockaddr *> (&sa), &len);
    return ntohs (reinterpret_cast<sockaddr_in *> (&sa)->sin_port);
}

constexpr auto patience = std::chrono::seconds (10);

}   // namespace

// --------------------------------------------------------- device parsing

DN_TEST (multinet, device_udp_default_port)
{
    MultinetDevice d = MultinetDevice::parse ("localhost:");
    DN_ASSERT (d.mode == Mode::udp);
    DN_ASSERT_EQ (d.destination, std::string ("localhost"));
    DN_ASSERT_EQ (d.dest_port, 700);
    // Local and remote ports match unless told otherwise.
    DN_ASSERT_EQ (d.source_port, 700);
}

DN_TEST (multinet, device_udp_with_ports)
{
    MultinetDevice d = MultinetDevice::parse ("localhost:7000");
    DN_ASSERT (d.mode == Mode::udp);
    DN_ASSERT_EQ (d.dest_port, 7000);
    DN_ASSERT_EQ (d.source_port, 7000);

    // A trailing number is the local port.
    MultinetDevice e = MultinetDevice::parse ("localhost:7000:7001");
    DN_ASSERT (e.mode == Mode::udp);
    DN_ASSERT_EQ (e.dest_port, 7000);
    DN_ASSERT_EQ (e.source_port, 7001);
}

DN_TEST (multinet, device_connect_mode)
{
    MultinetDevice d = MultinetDevice::parse ("localhost:700:connect");
    DN_ASSERT (d.mode == Mode::connect);
    DN_ASSERT_EQ (d.destination, std::string ("localhost"));
    DN_ASSERT_EQ (d.dest_port, 700);
    DN_ASSERT_EQ (d.source_port, 0);
}

DN_TEST (multinet, device_listen_mode)
{
    // In listen mode the port given is the one we bind, and there is no
    // destination port at all.
    MultinetDevice d = MultinetDevice::parse ("localhost:12345:listen");
    DN_ASSERT (d.mode == Mode::listen);
    DN_ASSERT_EQ (d.source_port, 12345);
    DN_ASSERT_EQ (d.dest_port, 0);

    // An empty host in listen mode means "accept from anywhere".
    MultinetDevice e = MultinetDevice::parse (":12345:listen");
    DN_ASSERT (e.mode == Mode::listen);
    DN_ASSERT_EQ (e.destination, std::string (""));
    DN_ASSERT_EQ (e.source_port, 12345);
}

DN_TEST (multinet, device_errors)
{
    DN_ASSERT_THROWS (std::invalid_argument, MultinetDevice::parse ("localhost"));
    DN_ASSERT_THROWS (std::invalid_argument,
                      MultinetDevice::parse ("localhost:99999"));
    DN_ASSERT_THROWS (std::invalid_argument,
                      MultinetDevice::parse ("a:1:2:3"));
    DN_ASSERT_THROWS (std::invalid_argument,
                      MultinetDevice::parse ("localhost:700:bogus"));
    // A connect with no destination has nowhere to go.
    DN_ASSERT_THROWS (std::invalid_argument,
                      MultinetDevice::parse (":700:connect"));
}

// ------------------------------------------------------------- loopback

DN_TEST (multinet, tcp_loopback_carries_frames_both_ways)
{
    std::uint16_t port = free_port ();

    Config lcfg = Config::from_string (
        "node 1.1 LISTEN\n"
        "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
        + ":listen\n");
    Config ccfg = Config::from_string (
        "node 1.2 CONN\n"
        "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
        + ":connect\n");

    Node listener (lcfg);
    Node connector (ccfg);

    Datalink *ldl = listener.datalink ()->circuit ("mul-0");
    Datalink *cdl = connector.datalink ()->circuit ("mul-0");
    DN_ASSERT (ldl != nullptr);
    DN_ASSERT (cdl != nullptr);

    TestOwner lowner (&listener), cowner (&connector);
    Port *lport = ldl->create_port (&lowner);
    Port *cport = cdl->create_port (&cowner);

    listener.start ();
    connector.start ();

    // Opening the port is what starts a point to point datalink; the
    // datalink's own open() is deliberately a no-op.
    lport->open ();
    cport->open ();

    DN_ASSERT (lowner.wait_for ([&] { return lowner.ups_unlocked () > 0; },
                                patience));
    DN_ASSERT (cowner.wait_for ([&] { return cowner.ups_unlocked () > 0; },
                                patience));

    // Connector to listener.
    Bytes msg = bytes_of ({ 0x02, 0x36, 0x24, 0x01, 0x04, 0x00, 'h', 'i' });
    cport->send (msg);
    DN_ASSERT (lowner.wait_for ([&] { return lowner.count_unlocked () > 0; },
                                patience));
    DN_ASSERT_EQ (lowner.received ().at (0), msg);

    // And back the other way.
    Bytes reply = bytes_of ({ 0x05, 0x24, 0x36 });
    lport->send (reply);
    DN_ASSERT (cowner.wait_for ([&] { return cowner.count_unlocked () > 0; },
                                patience));
    DN_ASSERT_EQ (cowner.received ().at (0), reply);

    connector.stop ();
    listener.stop ();
}

DN_TEST (multinet, tcp_loopback_preserves_frame_boundaries)
{
    // The length header is the whole point: a stream of frames sent back to
    // back must arrive as the same frames, not as one run of bytes.
    std::uint16_t port = free_port ();

    Config lcfg = Config::from_string (
        "node 1.1 LISTEN\n"
        "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
        + ":listen\n");
    Config ccfg = Config::from_string (
        "node 1.2 CONN\n"
        "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
        + ":connect\n");

    Node listener (lcfg), connector (ccfg);
    TestOwner lowner (&listener), cowner (&connector);
    Port *lport = listener.datalink ()->circuit ("mul-0")->create_port (&lowner);
    Port *cport = connector.datalink ()->circuit ("mul-0")->create_port (&cowner);

    listener.start ();
    connector.start ();
    lport->open ();
    cport->open ();

    DN_ASSERT (cowner.wait_for ([&] { return cowner.ups_unlocked () > 0; },
                                patience));

    // Frames of several lengths, including an empty one.
    std::vector<Bytes> sent;
    for (std::size_t n : { 1u, 0u, 5u, 300u, 2u }) {
        Bytes b (n, static_cast<std::uint8_t> (n & 0xff));
        sent.push_back (b);
        cport->send (b);
    }

    DN_ASSERT (lowner.wait_for (
        [&] { return lowner.count_unlocked () >= 5; }, patience));

    std::vector<Bytes> got = lowner.received ();
    DN_ASSERT_EQ (got.size (), 5u);
    for (std::size_t i = 0; i < sent.size (); ++i)
        DN_ASSERT_EQ (got[i], sent[i]);

    connector.stop ();
    listener.stop ();
}

namespace {

// A Multinet connect circuit to a bare socket the test reads, or does not
// read, as it likes.  The bare end's receive buffer is small, so the
// connection fills soon.
struct BarePeer {
    std::uint16_t port = free_port ();
    Socket        server;
    Config        cfg;
    Node          node;
    TestOwner     owner;
    Port         *port_ = nullptr;
    Socket        peer;

    BarePeer ()
        : server (SourceAddress ("127.0.0.1", port).create_server ()),
          cfg (Config::from_string (
              "node 1.2 CONN\n"
              "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
              + ":connect\n")),
          node (cfg), owner (&node)
    {
        int small = 4096;
        ::setsockopt (server.fd (), SOL_SOCKET, SO_RCVBUF,
                      reinterpret_cast<const char *> (&small), sizeof small);
        port_ = node.datalink ()->circuit ("mul-0")->create_port (&owner);
        node.start ();
        port_->open ();
        peer = Socket (sock_accept (server.fd ()));
    }

    ~BarePeer () { node.stop (); }

    // Read until nothing more comes for a while.
    Bytes drain ()
    {
        Bytes stream;
        std::uint8_t buf[65536];
        for (;;) {
            PollResult pr = poll_socket (peer.fd (), true, false, 1000);
            if (!pr.readable) return stream;
            ssize_t n = sock_recv (peer.fd (), buf, sizeof buf);
            if (n <= 0) return stream;
            stream.insert (stream.end (), buf, buf + n);
        }
    }
};

Bytes numbered (unsigned i, std::size_t size)
{
    Bytes b (size);
    for (std::size_t j = 0; j < size; ++j)
        b[j] = static_cast<std::uint8_t> (i * 31 + j);
    return b;
}

// Split a Multinet stream into frames; false if it does not end on a frame.
bool frames_of (const Bytes &stream, std::vector<Bytes> &out)
{
    std::size_t at = 0;
    while (stream.size () - at >= 4) {
        std::size_t n = stream[at] | (stream[at + 1] << 8);
        if (stream.size () - at < 4 + n) return false;
        out.emplace_back (stream.begin () + static_cast<std::ptrdiff_t> (at + 4),
                          stream.begin () + static_cast<std::ptrdiff_t> (at + 4 + n));
        at += 4 + n;
    }
    return at == stream.size ();
}

}   // namespace

DN_TEST (multinet, tcp_peer_slow_to_read_loses_nothing)
{
    // A peer that stops reading for a while, as a busy or paused emulator
    // does, long enough for the connection to fill.  Every frame still
    // arrives, whole and in order, on the one connection; and sending
    // never waited for the peer.
    BarePeer b;
    DN_ASSERT (static_cast<bool> (b.peer));
    DN_ASSERT (b.owner.wait_for ([&] { return b.owner.ups_unlocked () > 0; },
                                 patience));

    constexpr unsigned frames = 180;
    constexpr std::size_t size = 1400;
    auto started = std::chrono::steady_clock::now ();
    for (unsigned i = 0; i < frames; ++i) b.port_->send (numbered (i, size));
    DN_ASSERT (std::chrono::steady_clock::now () - started
               < std::chrono::seconds (1));
    std::this_thread::sleep_for (std::chrono::milliseconds (300));

    std::vector<Bytes> got;
    DN_ASSERT (frames_of (b.drain (), got));
    DN_ASSERT_EQ (b.owner.downs (), 0);
    DN_ASSERT_EQ (got.size (), std::size_t (frames));
    for (unsigned i = 0; i < frames; ++i)
        DN_ASSERT (got[i] == numbered (i, size));
}

DN_TEST (multinet, tcp_peer_that_stops_reading_costs_frames_not_the_circuit)
{
    // Far more than is kept for a peer that has stopped reading: the
    // excess is dropped as whole frames.  The circuit stays up, and what
    // does arrive is whole frames, in order.
    BarePeer b;
    DN_ASSERT (b.owner.wait_for ([&] { return b.owner.ups_unlocked () > 0; },
                                 patience));

    constexpr unsigned frames = 4000;
    constexpr std::size_t size = 1400;
    for (unsigned i = 0; i < frames; ++i) b.port_->send (numbered (i, size));
    std::this_thread::sleep_for (std::chrono::milliseconds (300));

    std::vector<Bytes> got;
    DN_ASSERT (frames_of (b.drain (), got));
    DN_ASSERT_EQ (b.owner.downs (), 0);
    DN_ASSERT (!got.empty () && got.size () < frames);
    unsigned next = 0;
    for (const Bytes &f : got) {
        while (next < frames && f != numbered (next, size)) ++next;
        DN_ASSERT (next < frames);
        ++next;
    }
}

DN_TEST (multinet, counters_track_traffic)
{
    std::uint16_t port = free_port ();
    Config lcfg = Config::from_string (
        "node 1.1 L\n"
        "circuit m Multinet 127.0.0.1:" + std::to_string (port) + ":listen\n");
    Config ccfg = Config::from_string (
        "node 1.2 C\n"
        "circuit m Multinet 127.0.0.1:" + std::to_string (port) + ":connect\n");

    Node listener (lcfg), connector (ccfg);
    TestOwner lowner (&listener), cowner (&connector);
    Datalink *ldl = listener.datalink ()->circuit ("m");
    Datalink *cdl = connector.datalink ()->circuit ("m");
    Port *lport = ldl->create_port (&lowner);
    Port *cport = cdl->create_port (&cowner);

    listener.start ();
    connector.start ();
    lport->open ();
    cport->open ();
    DN_ASSERT (cowner.wait_for ([&] { return cowner.ups_unlocked () > 0; },
                                patience));

    cport->send (Bytes (10, 0xaa));
    DN_ASSERT (lowner.wait_for ([&] { return lowner.count_unlocked () > 0; },
                                patience));

    DN_ASSERT_EQ (cdl->counters ()->pkts_sent, 1u);
    DN_ASSERT_EQ (cdl->counters ()->bytes_sent, 10u);
    DN_ASSERT_EQ (ldl->counters ()->pkts_recv, 1u);
    DN_ASSERT_EQ (ldl->counters ()->bytes_recv, 10u);

    connector.stop ();
    listener.stop ();
}

DN_TEST (multinet, connect_to_nothing_stays_down_and_retries)
{
    // Nothing listening: the circuit stays down, does not crash, and keeps
    // retrying.
    std::uint16_t port = free_port ();
    Config ccfg = Config::from_string (
        "node 1.2 C\n"
        "circuit m Multinet 127.0.0.1:" + std::to_string (port) + ":connect\n");

    Node connector (ccfg);
    TestOwner owner (&connector);
    Port *p = connector.datalink ()->circuit ("m")->create_port (&owner);
    connector.start ();
    p->open ();

    // Give it long enough to fail and schedule a retry.
    std::this_thread::sleep_for (std::chrono::milliseconds (500));
    DN_ASSERT_EQ (owner.ups (), 0);

    connector.stop ();
}
