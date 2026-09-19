// Counters across the layers: the NSP node counters, the routing layer's
// circuit counters, the executor counters, the DDCMP error counters, and
// the datalink traffic counters a line read reports.
//
// These are the numbers the monitoring pages show, so each test asks the
// question the way a page does -- through nice_read -- as well as reading
// the counter struct directly where the struct is the point.

#include "harness.h"

#include "decnet/common/socket.h"
#include "decnet/config.h"
#include "decnet/datalink/bc.h"
#include "decnet/datalink/datalink.h"
#include "decnet/datalink/ddcmp.h"
#include "decnet/nice/nml.h"
#include "decnet/nice/packets.h"
#include "decnet/node.h"
#include "decnet/nsp/nsp.h"
#include "decnet/routing/lan.h"
#include "decnet/routing/ptp.h"
#include "decnet/routing/routing.h"

#include <chrono>
#include <mutex>
#include <thread>

using namespace decnet;
using namespace decnet::nice;

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

// A session control that accepts everything and records what arrives.
class TestSession : public nsp::SessionControl {
public:
    void connect_received (nsp::Connection &c, ByteView) override
    { c.accept (); }
    void connect_confirmed (nsp::Connection &, ByteView) override
    { std::lock_guard l (m_); ++confirmed_; }
    void connect_rejected (nsp::Connection &, unsigned, ByteView) override {}
    void data_received (nsp::Connection &, ByteView d) override
    { std::lock_guard l (m_); ++messages_; bytes_ += d.size (); }
    void disconnected (nsp::Connection &, unsigned, ByteView) override {}

    int confirmed () { std::lock_guard l (m_); return confirmed_; }
    int messages () { std::lock_guard l (m_); return messages_; }
    std::size_t bytes () { std::lock_guard l (m_); return bytes_; }

private:
    std::mutex  m_;
    int         confirmed_ = 0, messages_ = 0;
    std::size_t bytes_ = 0;
};

// Two nodes joined by a Multinet circuit.  Endnodes by default; pass a
// type to get routers instead.
struct Link {
    std::uint16_t port = free_port ();
    Config acfg, bcfg;
    std::unique_ptr<Node> a, b;
    TestSession sa, sb;

    explicit Link (const std::string &type = "endnode")
        : acfg (Config::from_string (
              "routing 1.1 --type " + type
              + "\nnode 1.1 NODEA\nnode 1.2 NODEB\n"
                "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
              + ":listen --t3 2\n")),
          bcfg (Config::from_string (
              "routing 1.2 --type " + type
              + "\nnode 1.2 NODEB\nnode 1.1 NODEA\n"
                "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
              + ":connect --t3 2\n"))
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
        wait_until ([&] {
            return a->routing ()->adjacency_count () == 1
                && b->routing ()->adjacency_count () == 1;
        });
    }
    void stop () { if (b) b->stop (); if (a) a->stop (); }
};

// A read request for one entity kind at one information level.
NiceRequest read_request (std::uint8_t etype, unsigned info, ReqEntity ent)
{
    NiceRequest r;
    r.function    = fn_read;
    r.info        = info;
    r.entity_type = etype;
    r.entity      = ent;
    return r;
}

// Run a counters read on the node's own thread and hand back the replies.
// Layer state may only be touched there, which is exactly why the HTTP
// server posts a callback rather than reading directly.
ReplyDict counters_read (Node *n, std::uint8_t kind, ReqEntity ent)
{
    ReplyDict replies (kind, n);
    replies.want_every_address (true);
    std::mutex m;
    bool done = false;
    n->add_work (std::make_unique<CallbackWork> ([&] {
        NiceRequest req = read_request (kind, info_counters, ent);
        n->nice_read (req, replies);
        std::lock_guard l (m);
        done = true;
    }));
    wait_until ([&] { std::lock_guard l (m); return done; });
    return replies;
}

// The value of one counter in a reply, or -1 if it is not there.
long long counter_of (const NiceReply &r, std::uint16_t number)
{
    const Param *p = r.params.find (number, true);
    return p ? static_cast<long long> (p->count.value) : -1;
}

Bytes bytes_of (const std::string &s) { return Bytes (s.begin (), s.end ()); }

}   // namespace

// ------------------------------------------------------------ node counters

DN_TEST (counters, nsp_node_counters_follow_a_conversation)
{
    Link l;
    l.start ();

    nsp::Connection *c = l.b->nsp ()->connect (Nodeid::parse ("1.1"),
                                               bytes_of ("hello"));
    DN_ASSERT (c != nullptr);
    DN_ASSERT (wait_until ([&] { return l.sb.confirmed () == 1; }));

    // One message each way, so both ends see one user message sent and one
    // received, and the byte counts match the payloads.
    const std::string out = "a message from B";
    c->send_data (bytes_of (out));
    DN_ASSERT (wait_until ([&] { return l.sa.messages () == 1; }));

    Nodeinfo *at_b = l.b->find_node (Nodeid::parse ("1.1"));   // A, as B sees it
    Nodeinfo *at_a = l.a->find_node (Nodeid::parse ("1.2"));   // B, as A sees it
    DN_ASSERT (at_b != nullptr && at_a != nullptr);

    // B connected out, A saw a connect come in.
    DN_ASSERT_EQ (at_b->counters.con_xmt, 1u);
    DN_ASSERT_EQ (at_b->counters.con_rcv, 0u);
    DN_ASSERT_EQ (at_a->counters.con_rcv, 1u);
    DN_ASSERT_EQ (at_a->counters.con_xmt, 0u);

    // The user message, counted once however many segments it became.
    DN_ASSERT_EQ (at_b->counters.msg_xmt, 1u);
    DN_ASSERT_EQ (at_b->counters.byt_xmt, out.size ());
    DN_ASSERT (wait_until ([&] { return at_a->counters.msg_rcv == 1; }));
    DN_ASSERT_EQ (at_a->counters.byt_rcv, out.size ());

    // Totals count every NSP packet, so they are larger than the user
    // figures and non-zero in both directions on both ends.
    DN_ASSERT (at_b->counters.t_msg_xmt >= 2);
    DN_ASSERT (at_b->counters.t_msg_rcv >= 1);
    DN_ASSERT (at_b->counters.t_byt_xmt > at_b->counters.byt_xmt);
    DN_ASSERT (at_a->counters.t_msg_rcv >= 2);

    l.stop ();
}

DN_TEST (counters, a_node_with_no_traffic_is_not_significant)
{
    Link l;
    l.start ();

    Nodeinfo *quiet = l.a->find_node (Nodeid::parse ("1.2"));
    DN_ASSERT (quiet != nullptr);
    DN_ASSERT (!quiet->counters.used ());

    nsp::Connection *c = l.b->nsp ()->connect (Nodeid::parse ("1.1"),
                                               bytes_of ("x"));
    DN_ASSERT (c != nullptr);
    DN_ASSERT (wait_until ([&] { return quiet->counters.used (); }));

    l.stop ();
}

DN_TEST (counters, a_node_counters_read_reports_every_counter)
{
    Link l;
    l.start ();

    ReplyDict replies = counters_read (l.a.get (), Entity::node,
                                       ReqEntity::make_node (Nodeid ()));
    NiceReply &exe = replies.node_entry (Nodeid::parse ("1.1"));

    // The twelve NSP counters plus time since zeroed, all present even when
    // they are zero: a counter that is missing reads as "not supported",
    // which is a different statement from "none".
    for (std::uint16_t n : { 0, 600, 601, 602, 603, 608, 609, 610, 611,
                             620, 621, 630, 640 })
        DN_ASSERT (counter_of (exe, n) >= 0);

    // And the executor's own.  This node is an endnode, so it has the four
    // that do not need a routing table and none of the four that do.
    for (std::uint16_t n : { 700, 903, 910, 930 })
        DN_ASSERT (counter_of (exe, n) >= 0);
    for (std::uint16_t n : { 900, 901, 902, 920 })
        DN_ASSERT_EQ (counter_of (exe, n), -1);

    l.stop ();
}

DN_TEST (counters, a_router_reports_the_routing_table_counters_too)
{
    Link l ("l1router");
    l.start ();

    ReplyDict replies = counters_read (l.a.get (), Entity::node,
                                       ReqEntity::make_node (Nodeid ()));
    NiceReply &exe = replies.node_entry (Nodeid::parse ("1.1"));

    // Aged, unreachable, out of range and partial update loss: the four an
    // endnode does not have.
    for (std::uint16_t n : { 900, 901, 902, 920 })
        DN_ASSERT (counter_of (exe, n) >= 0);

    l.stop ();
}

// --------------------------------------------------------- circuit counters

DN_TEST (counters, circuit_counts_originating_and_terminating_traffic)
{
    Link l;
    l.start ();

    routing::PtpCircuit *ca = l.a->routing ()->circuits ().front ();
    routing::PtpCircuit *cb = l.b->routing ()->circuits ().front ();

    // Bringing the circuit up is not traffic.
    DN_ASSERT_EQ (ca->counters ().orig_sent, 0u);
    DN_ASSERT_EQ (ca->counters ().term_recv, 0u);

    nsp::Connection *c = l.b->nsp ()->connect (Nodeid::parse ("1.1"),
                                               bytes_of ("hello"));
    DN_ASSERT (c != nullptr);
    DN_ASSERT (wait_until ([&] { return l.sb.confirmed () == 1; }));

    // B originated the connect, A terminated it.  Neither is transit: an
    // endnode forwards nothing.
    DN_ASSERT (wait_until ([&] { return cb->counters ().orig_sent > 0; }));
    DN_ASSERT (wait_until ([&] { return ca->counters ().term_recv > 0; }));
    DN_ASSERT_EQ (ca->counters ().trans_recv, 0u);
    DN_ASSERT_EQ (ca->counters ().trans_sent, 0u);

    l.stop ();
}

DN_TEST (counters, circuit_records_when_it_came_up)
{
    Link l;
    l.start ();

    routing::PtpCircuit *ca = l.a->routing ()->circuits ().front ();
    DN_ASSERT (ca->counters ().ever_up ());
    DN_ASSERT_EQ (ca->counters ().peak_adj, 1u);
    // Nothing has gone wrong yet.
    DN_ASSERT_EQ (ca->counters ().cir_down, 0u);
    DN_ASSERT_EQ (ca->counters ().adj_down, 0u);

    l.stop ();
}

DN_TEST (counters, losing_the_neighbour_counts_a_circuit_down)
{
    Link l;
    l.start ();

    routing::PtpCircuit *ca = l.a->routing ()->circuits ().front ();
    DN_ASSERT (ca->counters ().ever_up ());

    // Stop B and let A's listen timer expire.  A running circuit that goes
    // away is a circuit down and an adjacency down, not an init failure.
    l.b->stop ();
    l.b.reset ();
    // Wait for each separately.  restart() counts the circuit down before
    // it takes the adjacency down, so seeing one says nothing yet about
    // the other.
    DN_ASSERT (wait_until ([&] { return ca->counters ().cir_down >= 1; },
                           std::chrono::seconds (30)));
    DN_ASSERT (wait_until ([&] { return ca->counters ().adj_down >= 1; },
                           std::chrono::seconds (30)));

    l.a->stop ();
}

DN_TEST (counters, a_circuit_counters_read_reports_both_layers)
{
    Link l;
    l.start ();

    ReplyDict replies = counters_read (l.a.get (), Entity::circuit,
                                       ReqEntity::make_wild (
                                           Entity::circuit, ReqEntity::known));
    NiceReply &r = replies.named_entry ("MUL-0");

    // The routing layer's counters...
    for (std::uint16_t n : { 0, 800, 801, 810, 811, 820, 821, 900, 3901 })
        DN_ASSERT (counter_of (r, n) >= 0);
    // ...the circuit-up stamp, which is there because the circuit is up...
    DN_ASSERT (counter_of (r, 3900) >= 0);
    // ...and the datalink's, on the same reply.
    for (std::uint16_t n : { 1000, 1001, 1010, 1011 })
        DN_ASSERT (counter_of (r, n) >= 0);

    l.stop ();
}

// ------------------------------------------------------------ line counters

DN_TEST (counters, an_ethernet_line_read_reports_its_counters)
{
    // The bug this test exists for: a broadcast datalink keeps its traffic
    // per port, and a line read that asked only the datalink found nothing,
    // so an Ethernet line's counters page came back empty.
    Config cfg = Config::from_string (
        "routing 1.1 --type endnode\nnode 1.1 NODEA\n"
        "circuit eth-0 Ethernet udp:" + std::to_string (free_port ())
        + ":127.0.0.1:" + std::to_string (free_port ())
        + " --random-address\n");
    Node n (cfg);
    n.start ();

    ReplyDict replies = counters_read (&n, Entity::line,
                                       ReqEntity::make_wild (
                                           Entity::line, ReqEntity::known));
    NiceReply &r = replies.named_entry ("ETH-0");

    // Traffic, multicast traffic and frames nothing wanted.
    for (std::uint16_t c : { 0, 1000, 1001, 1002, 1010, 1011, 1012, 1063 })
        DN_ASSERT (counter_of (r, c) >= 0);

    n.stop ();
}

DN_TEST (counters, a_broadcast_line_sums_its_ports)
{
    Config cfg = Config::from_string (
        "routing 1.1 --type endnode\nnode 1.1 NODEA\n"
        "circuit eth-0 Ethernet udp:" + std::to_string (free_port ())
        + ":127.0.0.1:" + std::to_string (free_port ())
        + " --random-address\n");
    Node n (cfg);
    auto *bc = dynamic_cast<datalink::BcDatalink *> (
        n.datalink ()->circuit ("ETH-0"));
    DN_ASSERT (bc != nullptr);

    // Nothing has run, so every port is at zero and so is their sum.
    datalink::BcPortCounters total = bc->combined_counters ();
    DN_ASSERT_EQ (total.bytes_sent, 0u);
    DN_ASSERT_EQ (total.pkts_recv, 0u);
}

// ----------------------------------------------------------- DDCMP counters

DN_TEST (counters, nak_reasons_map_to_the_right_counter_and_bit)
{
    using namespace decnet::datalink::ddcmp;
    NakCounter nc;

    // Data errors, bits 0 to 2 in the order the qualifier names are listed.
    DN_ASSERT (nak_counter (R_HCRC, nc));
    DN_ASSERT (nc.data); DN_ASSERT_EQ (nc.bit, 0u);
    DN_ASSERT (nak_counter (R_CRC, nc));
    DN_ASSERT (nc.data); DN_ASSERT_EQ (nc.bit, 1u);
    DN_ASSERT (nak_counter (R_REP, nc));
    DN_ASSERT (nc.data); DN_ASSERT_EQ (nc.bit, 2u);

    // Buffer errors.
    DN_ASSERT (nak_counter (R_BUF, nc));
    DN_ASSERT (!nc.data); DN_ASSERT_EQ (nc.bit, 0u);
    DN_ASSERT (nak_counter (R_SHRT, nc));
    DN_ASSERT (!nc.data); DN_ASSERT_EQ (nc.bit, 1u);

    // Overrun and format error have no counter, as in PyDECnet's nak_map.
    DN_ASSERT (!nak_counter (R_OVER, nc));
    DN_ASSERT (!nak_counter (R_FMT, nc));
}

DN_TEST (counters, a_received_nak_counts_an_outbound_data_error)
{
    using namespace decnet::datalink::ddcmp;

    // Drive the engine by hand: the hooks are what a transport supplies, so
    // a test can be the transport.
    std::vector<Message> sent;
    Protocol::Hooks h;
    h.send = [&] (const Message &m) { sent.push_back (m); };
    h.deliver = [] (Bytes) {};
    h.up = [] {};
    h.down = [] {};
    h.set_timer = [] (double) {};
    Protocol p (h);

    p.connected ();
    // Walk the start handshake: answer whatever it sends until it runs.
    for (int i = 0; i < 4 && p.state () != Protocol::State::running; ++i) {
        if (sent.empty ()) break;
        Message last = sent.back ();
        if (last.kind == MsgKind::start)      p.receive (make_stack ());
        else if (last.kind == MsgKind::stack) p.receive (make_ack (Seq (0)));
        else break;
    }
    DN_ASSERT_EQ (int (p.state ()), int (Protocol::State::running));

    DN_ASSERT_EQ (p.counters ().data_errors_outbound, 0u);
    p.receive (make_nak (Seq (0), R_CRC));
    DN_ASSERT_EQ (p.counters ().data_errors_outbound, 1u);
    DN_ASSERT_EQ (p.counters ().data_errors_outbound_map, 0x2u);  // bit 1

    // A buffer complaint lands in the other counter.
    p.receive (make_nak (Seq (0), R_BUF));
    DN_ASSERT_EQ (p.counters ().remote_buffer_errors, 1u);
    DN_ASSERT_EQ (p.counters ().remote_buffer_errors_map, 0x1u);  // bit 0
    DN_ASSERT_EQ (p.counters ().data_errors_outbound, 1u);        // unchanged
}

// --------------------------------------------------------------- formatting

DN_TEST (counters, elapsed_time_reads_as_hours_minutes_seconds)
{
    ParamList params;
    params.set (111, Value::cm ({ Value::du (107), Value::du (43),
                                  Value::du (4) }));
    std::vector<std::string> lines = params.format (params_for (Entity::module));
    DN_ASSERT_EQ (lines.size (), 1u);
    DN_ASSERT_EQ (lines[0], std::string ("Elapsed time = 107:43:04"));
}
