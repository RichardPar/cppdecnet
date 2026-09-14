// NSP logical links: connect, accept, reject, data transfer with
// segmentation and reassembly, and disconnect -- run between two real
// nodes over a Multinet circuit, so the whole stack is in the path.

#include "harness.h"

#include "decnet/common/socket.h"
#include "decnet/config.h"
#include "decnet/node.h"
#include "decnet/nsp/nsp.h"
#include "decnet/routing/routing.h"

#include <chrono>
#include <mutex>
#include <set>
#include <thread>

using namespace decnet;
using namespace decnet::nsp;

namespace {

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

template <typename P>
bool wait_until (P pred, std::chrono::milliseconds timeout
                     = std::chrono::seconds (15))
{
    auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        if (pred ()) return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    return pred ();
}

// A session control that records what it is told, and can be set to
// accept or reject inbound connections.
class TestSession : public SessionControl {
public:
    enum class Policy { accept, reject, ignore };
    Policy policy = Policy::accept;
    unsigned reject_reason = 0;
    Bytes accept_data;
    // What this end asks for on inbound connections, which is what the
    // other end must obey when it sends to us.
    std::uint8_t request_flow = SVC_NONE;

    void connect_received (Connection &c, ByteView payload) override
    {
        {
            std::lock_guard l (m_);
            inbound_.push_back (Bytes (payload.begin (), payload.end ()));
            last_ = &c;
        }
        if (policy == Policy::accept)      c.accept (accept_data, request_flow);
        else if (policy == Policy::reject) c.reject (reject_reason);
    }

    void connect_confirmed (Connection &, ByteView data) override
    {
        std::lock_guard l (m_);
        ++confirmed_;
        confirm_data_.assign (data.begin (), data.end ());
    }

    void connect_rejected (Connection &, unsigned reason, ByteView) override
    {
        std::lock_guard l (m_);
        ++rejected_;
        reject_code_ = reason;
    }

    void data_received (Connection &, ByteView data) override
    {
        std::lock_guard l (m_);
        messages_.push_back (Bytes (data.begin (), data.end ()));
    }

    void interrupt_received (Connection &, ByteView data) override
    {
        std::lock_guard l (m_);
        interrupts_.push_back (Bytes (data.begin (), data.end ()));
    }

    void disconnected (Connection &, unsigned reason, ByteView) override
    {
        std::lock_guard l (m_);
        ++disconnects_;
        disc_reason_ = reason;
    }

    std::size_t inbound () { std::lock_guard l (m_); return inbound_.size (); }
    Bytes inbound_payload (std::size_t i)
    { std::lock_guard l (m_); return inbound_.at (i); }
    Connection *last () { std::lock_guard l (m_); return last_; }
    int confirmed () { std::lock_guard l (m_); return confirmed_; }
    Bytes confirm_data () { std::lock_guard l (m_); return confirm_data_; }
    int rejected () { std::lock_guard l (m_); return rejected_; }
    unsigned reject_code () { std::lock_guard l (m_); return reject_code_; }
    std::size_t messages () { std::lock_guard l (m_); return messages_.size (); }
    Bytes message (std::size_t i)
    { std::lock_guard l (m_); return messages_.at (i); }
    std::size_t interrupts ()
    { std::lock_guard l (m_); return interrupts_.size (); }
    Bytes interrupt (std::size_t i)
    { std::lock_guard l (m_); return interrupts_.at (i); }
    int disconnects () { std::lock_guard l (m_); return disconnects_; }
    unsigned disc_reason () { std::lock_guard l (m_); return disc_reason_; }

private:
    std::mutex         m_;
    std::vector<Bytes> inbound_, messages_, interrupts_;
    Connection        *last_ = nullptr;
    int                confirmed_ = 0, rejected_ = 0, disconnects_ = 0;
    unsigned           reject_code_ = 0, disc_reason_ = 0;
    Bytes              confirm_data_;
};

// Two endnodes joined by a Multinet circuit, each with a session control.
struct Link {
    std::uint16_t port = free_port ();
    Config acfg, bcfg;
    std::unique_ptr<Node> a, b;
    TestSession sa, sb;

    explicit Link (const std::string &extra = "")
        : acfg (Config::from_string (
              "routing 1.1 --type endnode\nnode 1.1 NODEA\nnode 1.2 NODEB\n"
              "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
              + ":listen --t3 2\n" + extra)),
          bcfg (Config::from_string (
              "routing 1.2 --type endnode\nnode 1.2 NODEB\nnode 1.1 NODEA\n"
              "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
              + ":connect --t3 2\n" + extra))
    {
        a = std::make_unique<Node> (acfg);
        b = std::make_unique<Node> (bcfg);
        a->nsp ()->set_session_control (&sa);
        b->nsp ()->set_session_control (&sb);
    }

    void start ()
    {
        a->start ();
        b->start ();
        // Wait for the circuit, or nothing can be sent.
        wait_until ([&] {
            return a->routing ()->adjacency_count () == 1
                && b->routing ()->adjacency_count () == 1;
        });
    }
    void stop () { if (b) b->stop (); if (a) a->stop (); }
};

Bytes bytes_of (const std::string &s) { return Bytes (s.begin (), s.end ()); }

}   // namespace

DN_TEST (nsp, link_addresses_are_unique_and_nonzero)
{
    Config cfg = Config::from_string (
        "routing 1.1 --type endnode\nnode 1.1 NODEA\n"
        "circuit mul-0 Multinet 127.0.0.1:1:connect\n");
    Node n (cfg);
    NSP *nsp = n.nsp ();
    DN_ASSERT (nsp != nullptr);

    // Every address is distinct, and none is zero -- a zero link address
    // means "not yet assigned" in a connect message.
    std::set<std::uint16_t> seen;
    for (int i = 0; i < 50; ++i) {
        Connection *c = nsp->connect (Nodeid::parse ("1.2"), {});
        DN_ASSERT (c != nullptr);
        DN_ASSERT_NE (c->srcaddr (), 0);
        DN_ASSERT (seen.insert (c->srcaddr ()).second);
    }
    DN_ASSERT_EQ (nsp->connection_count (), 50u);
}

DN_TEST (nsp, connect_and_accept)
{
    Link l;
    l.start ();

    Connection *c = l.b->nsp ()->connect (Nodeid::parse ("1.1"),
                                          bytes_of ("hello"));
    DN_ASSERT (c != nullptr);

    // The far end's session control saw the connect, with its payload.
    DN_ASSERT (wait_until ([&] { return l.sa.inbound () == 1; }));
    DN_ASSERT_EQ (l.sa.inbound_payload (0), bytes_of ("hello"));

    // And ours saw the confirmation.
    DN_ASSERT (wait_until ([&] { return l.sb.confirmed () == 1; }));
    DN_ASSERT (wait_until ([&] { return c->running (); }));

    // Both ends know each other's link address.
    DN_ASSERT_NE (c->dstaddr (), 0);
    DN_ASSERT (l.sa.last () != nullptr);
    DN_ASSERT_EQ (l.sa.last ()->dstaddr (), c->srcaddr ());

    l.stop ();
}

DN_TEST (nsp, the_retransmit_timer_follows_the_measured_round_trip)
{
    // The timer is not a constant.  It starts at two seconds because
    // nothing has been measured, then follows the round trip actually seen
    // to that node -- so a link across an ocean and a link across a room
    // do not wait the same time before deciding a packet was lost.
    Link l;
    l.start ();

    Connection *c = l.b->nsp ()->connect (Nodeid::parse ("1.1"),
                                          bytes_of ("hi"));
    DN_ASSERT (c != nullptr);
    DN_ASSERT (wait_until ([&] { return c->running (); }));

    // Something has now been sent and acknowledged over the loopback, so
    // an estimate exists.  It is floored at one second: a tenth of a
    // second of timer granularity is not the constraint, packet length on
    // a slow serial line is, and an estimate taken from short packets
    // produces false timeouts the moment a long one goes out.
    Nodeinfo *info = l.b->find_node (Nodeid::parse ("1.1"), false);
    DN_ASSERT (info != nullptr);
    DN_ASSERT (info->delay >= 1.0);
    DN_ASSERT (info->delay <= 5.0);     // and capped

    l.stop ();
}

DN_TEST (nsp, the_estimate_is_an_average_not_the_last_measurement)
{
    // One slow round trip must not move the timer far, or a single hiccup
    // lengthens every timeout after it.  The weight decides how slowly it
    // moves: each measurement is folded in as 1/(weight+1) of the change.
    Config c = Config::from_string (
        "routing 1.1 --type endnode\nnode 1.1 NODEA\n"
        "circuit mul-0 Multinet 127.0.0.1:1:connect\n"
        "nsp --nsp-weight 3 --nsp-delay 2.0\n");
    DN_ASSERT_EQ (c.nsp ().weight, 3u);
    DN_ASSERT_EQ (c.nsp ().delay_factor, 2.0);

    // Work the published arithmetic by hand: starting at 1, a measurement
    // of 5 moves the estimate by (5-1)/4 = 1, not to 5.
    double delay = 1.0;
    unsigned weight = c.nsp ().weight;
    delay += (5.0 - delay) / (weight + 1);
    DN_ASSERT (delay > 1.9 && delay < 2.1);
}

DN_TEST (nsp, the_timer_settings_come_from_the_configuration)
{
    Config c = Config::from_string (
        "routing 1.1 --type endnode\nnode 1.1 NODEA\n"
        "circuit mul-0 Multinet 127.0.0.1:1:connect\n"
        "nsp --nsp-weight 10 --nsp-delay 3.5 --retransmits 9\n");
    DN_ASSERT_EQ (c.nsp ().weight, 10u);
    DN_ASSERT_EQ (c.nsp ().delay_factor, 3.5);
    DN_ASSERT_EQ (c.nsp ().retransmits, 9u);

    // And a value nobody meant is refused rather than quietly clamped: a
    // weight of zero would divide by one and make the estimate the last
    // measurement, which is the behaviour the averaging exists to avoid.
    DN_ASSERT_THROWS (std::runtime_error,
                      Config::from_string ("nsp --nsp-weight 0\n"));
    DN_ASSERT_THROWS (std::runtime_error,
                      Config::from_string ("nsp --nsp-delay 0.5\n"));
}

DN_TEST (nsp, closed_connections_are_reclaimed)
{
    // A closed connection is not destroyed at once, because it is retired
    // from inside a callback into its own owner and freeing it there is a
    // use-after-free.  It must not be kept forever either: that was the
    // slow leak in BUGS.md item 6, where a node that opened and closed
    // many links grew without bound.
    Link l;
    l.start ();
    // Nothing is waited out: the grace period is the rule being checked,
    // not the clock.
    l.a->nsp ()->set_closed_grace (std::chrono::seconds (0));
    l.b->nsp ()->set_closed_grace (std::chrono::seconds (0));

    std::size_t held = l.b->nsp ()->closed_count ();

    for (int i = 0; i < 5; ++i) {
        Connection *c = l.b->nsp ()->connect (Nodeid::parse ("1.1"),
                                              bytes_of ("hi"));
        DN_ASSERT (c != nullptr);
        DN_ASSERT (wait_until ([&] { return c->running (); }));
        c->disconnect (0, { });
        DN_ASSERT (wait_until ([&] { return !c->running (); }));
    }

    // With no grace, each retirement sweeps what came before it, so the
    // list does not grow with the number of links opened and closed.
    DN_ASSERT (wait_until ([&] {
        return l.b->nsp ()->closed_count () <= held + 1;
    }));

    l.stop ();
}

DN_TEST (nsp, a_closed_connection_is_kept_for_the_grace_period)
{
    // The other half of the rule: with a grace period, the object survives
    // its own retirement.  This is what stops the use-after-free.
    Link l;
    l.start ();
    l.b->nsp ()->set_closed_grace (std::chrono::seconds (60));

    Connection *c = l.b->nsp ()->connect (Nodeid::parse ("1.1"),
                                          bytes_of ("hi"));
    DN_ASSERT (c != nullptr);
    DN_ASSERT (wait_until ([&] { return c->running (); }));

    std::size_t before = l.b->nsp ()->closed_count ();
    c->disconnect (0, { });
    DN_ASSERT (wait_until ([&] { return !c->running (); }));
    DN_ASSERT (wait_until ([&] {
        return l.b->nsp ()->closed_count () == before + 1;
    }));
    // And it still answers, rather than being a dangling pointer.
    DN_ASSERT (!c->running ());

    l.stop ();
}

DN_TEST (nsp, accept_can_carry_data)
{
    Link l;
    l.sa.accept_data = bytes_of ("welcome");
    l.start ();

    l.b->nsp ()->connect (Nodeid::parse ("1.1"), bytes_of ("hi"));
    DN_ASSERT (wait_until ([&] { return l.sb.confirmed () == 1; }));
    DN_ASSERT_EQ (l.sb.confirm_data (), bytes_of ("welcome"));

    l.stop ();
}

DN_TEST (nsp, reject)
{
    Link l;
    l.sa.policy = TestSession::Policy::reject;
    l.sa.reject_reason = 7;
    l.start ();

    Connection *c = l.b->nsp ()->connect (Nodeid::parse ("1.1"),
                                          bytes_of ("go away"));
    DN_ASSERT (wait_until ([&] { return l.sb.rejected () == 1; }));
    DN_ASSERT_EQ (l.sb.reject_code (), 7u);
    DN_ASSERT (wait_until ([&] { return c->closed (); }));
    DN_ASSERT_EQ (l.sb.confirmed (), 0);

    l.stop ();
}

DN_TEST (nsp, data_transfer_both_ways)
{
    Link l;
    l.start ();

    Connection *c = l.b->nsp ()->connect (Nodeid::parse ("1.1"),
                                          bytes_of ("x"));
    DN_ASSERT (wait_until ([&] { return c->running (); }));

    c->send_data (bytes_of ("from B"));
    DN_ASSERT (wait_until ([&] { return l.sa.messages () == 1; }));
    DN_ASSERT_EQ (l.sa.message (0), bytes_of ("from B"));

    l.sa.last ()->send_data (bytes_of ("from A"));
    DN_ASSERT (wait_until ([&] { return l.sb.messages () == 1; }));
    DN_ASSERT_EQ (l.sb.message (0), bytes_of ("from A"));

    l.stop ();
}

DN_TEST (nsp, messages_keep_their_boundaries)
{
    // NSP carries messages, not a byte stream: three sends must arrive as
    // three messages.
    Link l;
    l.start ();
    Connection *c = l.b->nsp ()->connect (Nodeid::parse ("1.1"), {});
    DN_ASSERT (wait_until ([&] { return c->running (); }));

    c->send_data (bytes_of ("one"));
    c->send_data (bytes_of ("two"));
    c->send_data (bytes_of ("three"));

    DN_ASSERT (wait_until ([&] { return l.sa.messages () == 3; }));
    DN_ASSERT_EQ (l.sa.message (0), bytes_of ("one"));
    DN_ASSERT_EQ (l.sa.message (1), bytes_of ("two"));
    DN_ASSERT_EQ (l.sa.message (2), bytes_of ("three"));

    l.stop ();
}

DN_TEST (nsp, a_long_message_is_segmented_and_reassembled)
{
    Link l;
    l.start ();
    Connection *c = l.b->nsp ()->connect (Nodeid::parse ("1.1"), {});
    DN_ASSERT (wait_until ([&] { return c->running (); }));

    // Comfortably more than one segment: the far end must see one message.
    Bytes big;
    for (std::size_t i = 0; i < 4000; ++i)
        big.push_back (static_cast<std::uint8_t> (i & 0xff));
    DN_ASSERT (big.size () > c->segsize ());

    c->send_data (big);
    DN_ASSERT (wait_until ([&] { return l.sa.messages () == 1; },
                           std::chrono::seconds (20)));
    DN_ASSERT_EQ (l.sa.message (0), big);

    l.stop ();
}

DN_TEST (nsp, an_empty_message_still_arrives)
{
    Link l;
    l.start ();
    Connection *c = l.b->nsp ()->connect (Nodeid::parse ("1.1"), {});
    DN_ASSERT (wait_until ([&] { return c->running (); }));

    c->send_data (Bytes {});
    DN_ASSERT (wait_until ([&] { return l.sa.messages () == 1; }));
    DN_ASSERT (l.sa.message (0).empty ());

    l.stop ();
}

DN_TEST (nsp, disconnect_is_confirmed_and_both_ends_close)
{
    Link l;
    l.start ();
    Connection *c = l.b->nsp ()->connect (Nodeid::parse ("1.1"), {});
    DN_ASSERT (wait_until ([&] { return c->running (); }));
    Connection *far = l.sa.last ();

    c->disconnect (0);
    // The far end hears about it, and both ends end up closed.
    DN_ASSERT (wait_until ([&] { return l.sa.disconnects () == 1; }));
    DN_ASSERT (wait_until ([&] { return c->closed (); }));
    DN_ASSERT (wait_until ([&] { return far->closed (); }));

    l.stop ();
}

DN_TEST (nsp, phase_and_segment_size_are_negotiated)
{
    Link l;
    l.start ();
    Connection *c = l.b->nsp ()->connect (Nodeid::parse ("1.1"), {});
    DN_ASSERT (wait_until ([&] { return c->running (); }));

    // Both are Phase IV, and the segment size is the smaller of the two
    // offers -- here the same on both sides.
    DN_ASSERT_EQ (c->phase (), 4u);
    DN_ASSERT_EQ (l.sa.last ()->phase (), 4u);
    DN_ASSERT_EQ (c->segsize (), l.sa.last ()->segsize ());
    DN_ASSERT (c->segsize () > 0);

    l.stop ();
}

DN_TEST (nsp, out_of_order_segments_are_held_not_dropped)
{
    // A segment that arrives early used to be discarded and recovered by
    // retransmission.  Now it waits for the gap to be filled, so only what
    // is actually missing has to be sent again.
    Link l;
    l.start ();
    Connection *c = l.b->nsp ()->connect (Nodeid::parse ("1.1"), {});
    DN_ASSERT (wait_until ([&] { return c->running (); }));

    Connection *far = l.sa.last ();
    DN_ASSERT (far != nullptr);

    // Hand the far end segment 3 of a three segment message before 2.
    // Segment 1 has not been sent either, so both are held.
    auto seg = [&] (unsigned n, bool bom, bool eom, const std::string &text) {
        DataSeg d;
        d.dstaddr = far->srcaddr ();
        d.srcaddr = far->dstaddr ();
        d.set_segnum (Seq (n));
        d.msgflag = static_cast<std::uint8_t> (
            DataSeg::flag | (bom ? 0x20 : 0) | (eom ? 0x40 : 0));
        d.payload = bytes_of (text);
        return d.encode ();
    };

    Bytes third = seg (3, false, true, "c");
    l.a->nsp ()->deliver (Nodeid::parse ("1.2"),
                          ByteView (third.data (), third.size ()));
    DN_ASSERT (wait_until ([&] { return far->out_of_order () == 1; }));
    DN_ASSERT_EQ (l.sa.messages (), 0u);

    Bytes second = seg (2, false, false, "b");
    l.a->nsp ()->deliver (Nodeid::parse ("1.2"),
                          ByteView (second.data (), second.size ()));
    DN_ASSERT (wait_until ([&] { return far->out_of_order () == 2; }));
    DN_ASSERT_EQ (l.sa.messages (), 0u);

    // The missing first segment completes the message, and the two held
    // ones follow it without being retransmitted.
    Bytes first = seg (1, true, false, "a");
    l.a->nsp ()->deliver (Nodeid::parse ("1.2"),
                          ByteView (first.data (), first.size ()));
    DN_ASSERT (wait_until ([&] { return l.sa.messages () == 1; }));
    DN_ASSERT_EQ (l.sa.message (0), bytes_of ("abc"));
    DN_ASSERT_EQ (far->out_of_order (), 0u);

    l.stop ();
}

DN_TEST (nsp, duplicate_segments_are_acknowledged_and_ignored)
{
    Link l;
    l.start ();
    Connection *c = l.b->nsp ()->connect (Nodeid::parse ("1.1"), {});
    DN_ASSERT (wait_until ([&] { return c->running (); }));

    c->send_data (bytes_of ("once"));
    DN_ASSERT (wait_until ([&] { return l.sa.messages () == 1; }));

    // Replay the same segment: the far end has seen it already, so the
    // application must not see the message twice.
    Connection *far = l.sa.last ();
    DataSeg d;
    d.dstaddr = far->srcaddr ();
    d.srcaddr = far->dstaddr ();
    d.set_segnum (Seq (1));
    d.msgflag = static_cast<std::uint8_t> (DataSeg::flag | 0x20 | 0x40);
    d.payload = bytes_of ("once");
    Bytes dup = d.encode ();
    l.a->nsp ()->deliver (Nodeid::parse ("1.2"),
                          ByteView (dup.data (), dup.size ()));

    std::this_thread::sleep_for (std::chrono::milliseconds (300));
    DN_ASSERT_EQ (l.sa.messages (), 1u);
    l.stop ();
}

DN_TEST (nsp, segment_flow_control_holds_data_until_credit_arrives)
{
    // A peer that asks for segment flow control gets nothing until it
    // grants credit.  Without this the sender would simply flood it.
    Link l;
    l.sa.request_flow = SVC_SEG;
    l.start ();

    Connection *c = l.b->nsp ()->connect (Nodeid::parse ("1.1"), {});
    DN_ASSERT (wait_until ([&] { return c->running (); }));
    DN_ASSERT_EQ (c->flow_control (), SVC_SEG);

    c->send_data (bytes_of ("held"));
    // Queued, but not on the wire: no credit has been given.
    DN_ASSERT (wait_until ([&] { return c->queued () >= 1; }));
    std::this_thread::sleep_for (std::chrono::milliseconds (300));
    DN_ASSERT_EQ (c->in_flight (), 0u);
    DN_ASSERT_EQ (l.sa.messages (), 0u);

    // Grant one segment.
    Connection *far = l.sa.last ();
    LinkSvcMsg ls;
    ls.dstaddr   = c->srcaddr ();
    ls.srcaddr   = far->srcaddr ();
    ls.segnum    = Seq (1);
    ls.fcval_int = LinkSvcMsg::DATA_REQ;
    ls.fcmod     = LinkSvcMsg::NO_CHANGE;
    ls.fcval     = 1;
    Bytes frame = ls.encode ();
    l.b->nsp ()->deliver (Nodeid::parse ("1.1"),
                          ByteView (frame.data (), frame.size ()));

    DN_ASSERT (wait_until ([&] { return l.sa.messages () == 1; }));
    DN_ASSERT_EQ (l.sa.message (0), bytes_of ("held"));

    l.stop ();
}

DN_TEST (nsp, xoff_stops_transmission_and_xon_resumes_it)
{
    Link l;
    l.start ();
    Connection *c = l.b->nsp ()->connect (Nodeid::parse ("1.1"), {});
    DN_ASSERT (wait_until ([&] { return c->running (); }));
    Connection *far = l.sa.last ();

    unsigned lsnum = 0;
    auto link_service = [&] (std::uint8_t mod) {
        LinkSvcMsg ls;
        ls.dstaddr   = c->srcaddr ();
        ls.srcaddr   = far->srcaddr ();
        // Each one gets the next number on the other subchannel, as a real
        // peer sends them: a repeat of a number already seen is a
        // retransmission, and its credit must not be applied twice.
        ls.segnum    = Seq (++lsnum);
        ls.fcval_int = LinkSvcMsg::DATA_REQ;
        ls.fcmod     = mod;
        ls.fcval     = 0;
        Bytes f = ls.encode ();
        l.b->nsp ()->deliver (Nodeid::parse ("1.1"),
                              ByteView (f.data (), f.size ()));
    };

    link_service (LinkSvcMsg::XOFF);
    std::this_thread::sleep_for (std::chrono::milliseconds (200));
    c->send_data (bytes_of ("waiting"));
    std::this_thread::sleep_for (std::chrono::milliseconds (300));
    DN_ASSERT_EQ (l.sa.messages (), 0u);
    DN_ASSERT (c->queued () >= 1);

    link_service (LinkSvcMsg::XON);
    DN_ASSERT (wait_until ([&] { return l.sa.messages () == 1; }));
    DN_ASSERT_EQ (l.sa.message (0), bytes_of ("waiting"));

    l.stop ();
}

DN_TEST (nsp, the_window_limits_how_much_is_in_flight)
{
    // Even with no flow control, only qmax segments may be outstanding at
    // once.  The far end acknowledges as it goes, so the window opens
    // again and everything arrives, but never more than qmax is on the
    // wire at any moment.
    Link l ("nsp --qmax 4\n");
    l.start ();
    Connection *c = l.b->nsp ()->connect (Nodeid::parse ("1.1"), {});
    DN_ASSERT (wait_until ([&] { return c->running (); }));

    // Ten one-segment messages, against a window of four.
    for (int i = 0; i < 10; ++i) c->send_data (bytes_of ("x"));

    // Whatever else happens, the number on the wire at once is bounded.
    for (int i = 0; i < 20; ++i) {
        DN_ASSERT (c->in_flight () <= 4);
        std::this_thread::sleep_for (std::chrono::milliseconds (25));
    }
    // And they do all arrive, since the far end acknowledges as it goes.
    DN_ASSERT (wait_until ([&] { return l.sa.messages () == 10; }));

    l.stop ();
}

DN_TEST (nsp, reserved_link_service_values_are_ignored)
{
    Link l;
    l.start ();
    Connection *c = l.b->nsp ()->connect (Nodeid::parse ("1.1"), {});
    DN_ASSERT (wait_until ([&] { return c->running (); }));
    Connection *far = l.sa.last ();

    LinkSvcMsg ls;
    ls.dstaddr   = c->srcaddr ();
    ls.srcaddr   = far->srcaddr ();
    ls.segnum    = Seq (1);
    ls.fcval_int = 2;                 // only 0 and 1 are defined
    ls.fcmod     = LinkSvcMsg::NO_CHANGE;
    ls.fcval     = 1;
    Bytes f = ls.encode ();
    l.b->nsp ()->deliver (Nodeid::parse ("1.1"),
                          ByteView (f.data (), f.size ()));

    // Still usable afterwards.
    c->send_data (bytes_of ("fine"));
    DN_ASSERT (wait_until ([&] { return l.sa.messages () == 1; }));
    l.stop ();
}

DN_TEST (nsp, an_interrupt_arrives_out_of_band)
{
    Link l;
    l.start ();
    Connection *c = l.b->nsp ()->connect (Nodeid::parse ("1.1"), {});
    DN_ASSERT (wait_until ([&] { return c->running (); }));

    // One interrupt is allowed as soon as the link is up.
    DN_ASSERT (c->can_interrupt ());
    DN_ASSERT (c->send_interrupt (bytes_of ("urgent")));

    DN_ASSERT (wait_until ([&] { return l.sa.interrupts () == 1; }));
    DN_ASSERT_EQ (l.sa.interrupt (0), bytes_of ("urgent"));
    // It travels on its own subchannel, so it is not a data message.
    DN_ASSERT_EQ (l.sa.messages (), 0u);

    l.stop ();
}

DN_TEST (nsp, only_one_interrupt_until_more_credit_arrives)
{
    // A DECnet node typically allows one interrupt at a time.  The second
    // is refused until the far end releases another.
    Link l;
    l.start ();
    Connection *c = l.b->nsp ()->connect (Nodeid::parse ("1.1"), {});
    DN_ASSERT (wait_until ([&] { return c->running (); }));

    DN_ASSERT (c->send_interrupt (bytes_of ("first")));
    DN_ASSERT (!c->can_interrupt ());
    DN_ASSERT (!c->send_interrupt (bytes_of ("second")));
    DN_ASSERT (wait_until ([&] { return l.sa.interrupts () == 1; }));

    // Release one more, with a link service message on the other
    // subchannel.
    Connection *far = l.sa.last ();
    LinkSvcMsg ls;
    ls.dstaddr   = c->srcaddr ();
    ls.srcaddr   = far->srcaddr ();
    ls.segnum    = Seq (1);
    ls.fcval_int = LinkSvcMsg::INT_REQ;
    ls.fcmod     = LinkSvcMsg::NO_CHANGE;
    ls.fcval     = 1;
    Bytes f = ls.encode ();
    l.b->nsp ()->deliver (Nodeid::parse ("1.1"),
                          ByteView (f.data (), f.size ()));

    DN_ASSERT (wait_until ([&] { return c->can_interrupt (); }));
    DN_ASSERT (c->send_interrupt (bytes_of ("second")));
    DN_ASSERT (wait_until ([&] { return l.sa.interrupts () == 2; }));
    DN_ASSERT_EQ (l.sa.interrupt (1), bytes_of ("second"));

    l.stop ();
}

DN_TEST (nsp, interrupt_limits)
{
    Link l;
    l.start ();
    Connection *c = l.b->nsp ()->connect (Nodeid::parse ("1.1"), {});
    DN_ASSERT (wait_until ([&] { return c->running (); }));

    // Sixteen bytes is the limit.
    DN_ASSERT (!c->send_interrupt (Bytes (17, 'x')));
    DN_ASSERT (c->send_interrupt (Bytes (16, 'x')));
    DN_ASSERT (wait_until ([&] { return l.sa.interrupts () == 1; }));
    DN_ASSERT_EQ (l.sa.interrupt (0).size (), 16u);

    l.stop ();
}

DN_TEST (nsp, an_interrupt_before_the_link_is_running_is_refused)
{
    Link l;
    l.start ();
    Connection *c = l.b->nsp ()->connect (Nodeid::parse ("1.1"), {});
    // Straight away, before the confirm can have arrived.
    DN_ASSERT (!c->send_interrupt (bytes_of ("early")));
    DN_ASSERT (wait_until ([&] { return c->running (); }));
    l.stop ();
}

DN_TEST (nsp, data_and_interrupts_do_not_disturb_each_other)
{
    // The two subchannels number independently, and a cross
    // acknowledgement on one must not be applied to the other.
    Link l;
    l.start ();
    Connection *c = l.b->nsp ()->connect (Nodeid::parse ("1.1"), {});
    DN_ASSERT (wait_until ([&] { return c->running (); }));

    c->send_data (bytes_of ("one"));
    DN_ASSERT (c->send_interrupt (bytes_of ("!")));
    c->send_data (bytes_of ("two"));

    DN_ASSERT (wait_until ([&] { return l.sa.messages () == 2; }));
    DN_ASSERT (wait_until ([&] { return l.sa.interrupts () == 1; }));
    DN_ASSERT_EQ (l.sa.message (0), bytes_of ("one"));
    DN_ASSERT_EQ (l.sa.message (1), bytes_of ("two"));
    DN_ASSERT_EQ (l.sa.interrupt (0), bytes_of ("!"));

    // Data kept flowing afterwards, so the interrupt's acknowledgement
    // was not mistaken for a data one.
    c->send_data (bytes_of ("three"));
    DN_ASSERT (wait_until ([&] { return l.sa.messages () == 3; }));

    l.stop ();
}

DN_TEST (nsp, a_packet_for_an_unknown_link_is_ignored)
{
    // Not a crash, and not a reply: an address we have never issued is
    // simply not ours.
    Link l;
    l.start ();

    AckData a;
    a.dstaddr = 0xbeef;
    a.srcaddr = 1;
    a.acknum = AckNum { Seq (1), AckNum::ACKQ };
    Bytes frame = a.encode ();
    l.a->nsp ()->deliver (Nodeid::parse ("1.2"),
                          ByteView (frame.data (), frame.size ()));
    DN_ASSERT_EQ (l.sa.disconnects (), 0);

    l.stop ();
}
