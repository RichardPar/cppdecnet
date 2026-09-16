// MOP: message formats, system id exchange, counters, and the loopback
// protocol, run between two nodes sharing a UDP-carried LAN.

#include "harness.h"

#include "decnet/common/socket.h"
#include "decnet/config.h"
#include "decnet/mop/mop.h"
#include "decnet/node.h"

#include <chrono>
#include <thread>

using namespace decnet;
using namespace decnet::mop;

namespace {

Bytes bytes_of (const std::string &s) { return Bytes (s.begin (), s.end ()); }

std::uint16_t free_udp_port ()
{
    SourceAddress any ("127.0.0.1", 0);
    Socket s = any.bind_socket (AF_INET, SOCK_DGRAM);
    if (!s) throw std::runtime_error ("cannot find a free port");
    sockaddr_storage sa {};
    socklen_t len = sizeof sa;
    ::getsockname (s.fd (), reinterpret_cast<sockaddr *> (&sa), &len);
    return ntohs (reinterpret_cast<sockaddr_in *> (&sa)->sin_port);
}

template <typename P>
bool wait_until (P pred, std::chrono::milliseconds timeout
                     = std::chrono::seconds (10))
{
    auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        if (pred ()) return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (20));
    }
    return pred ();
}

// Two nodes on a shared LAN, both running MOP.
struct Lan {
    std::uint16_t pa = free_udp_port (), pb = free_udp_port ();
    Config acfg, bcfg;
    std::unique_ptr<Node> a, b;

    Lan ()
        : acfg (Config::from_string (
              "node 1.1 NODEA\ncircuit eth-0 Ethernet udp:"
              + std::to_string (pa) + ":127.0.0.1:" + std::to_string (pb)
              + " --random-address --mop\n")),
          bcfg (Config::from_string (
              "node 1.2 NODEB\ncircuit eth-0 Ethernet udp:"
              + std::to_string (pb) + ":127.0.0.1:" + std::to_string (pa)
              + " --random-address --mop\n"))
    {
        a = std::make_unique<Node> (acfg);
        b = std::make_unique<Node> (bcfg);
    }

    void start () { a->start (); b->start (); }
    void stop () { if (b) b->stop (); if (a) a->stop (); }

    MopCircuit *ca () { return a->mop ()->circuit ("eth-0"); }
    MopCircuit *cb () { return b->mop ()->circuit ("eth-0"); }
    Macaddr addr_a () { return ca ()->datalink ()->hwaddr (); }
    Macaddr addr_b () { return cb ()->datalink ()->hwaddr (); }
};

}   // namespace

// ------------------------------------------------------------- formats

DN_TEST (mop, system_id_roundtrip)
{
    SysId s;
    s.receipt   = 0x1234;
    s.version   = Version { 3, 0, 0 };
    s.loop      = true;
    s.counters  = true;
    s.hwaddr    = Bytes { 0xaa, 0x00, 0x04, 0x00, 0x36, 0x24 };
    s.device    = 9;
    s.datalink  = 1;
    s.processor = 2;
    s.software  = SoftwareId::named ("DECnet/C++");

    Bytes wire = s.encode_packet ();
    DN_ASSERT_EQ (wire[0], SYSTEM_ID);

    auto p = MopPacketBase::parse_frame (wire);
    DN_ASSERT (p != nullptr);
    DN_ASSERT_EQ (std::string (p->packet_name ()), std::string ("SysId"));

    auto *q = dynamic_cast<SysId *> (p.get ());
    DN_ASSERT_EQ (q->receipt, 0x1234);
    DN_ASSERT (q->loop);
    DN_ASSERT (q->counters);
    DN_ASSERT (!q->carrier);
    DN_ASSERT (q->software.has_value ());
    DN_ASSERT_EQ (q->software->text, std::string ("DECnet/C++"));
    DN_ASSERT_EQ (q->device.value (), 9);
    DN_ASSERT_EQ (q->encode_packet (), wire);

    std::vector<std::string> srv = q->services ();
    DN_ASSERT_EQ (srv.size (), 2u);
    DN_ASSERT_EQ (srv[0], std::string ("loop"));
}

DN_TEST (mop, software_id_special_values)
{
    // A length byte of zero or below is a code, not a string.  Signedness
    // is what distinguishes them.
    for (std::int8_t c : { std::int8_t (0), std::int8_t (-1),
                           std::int8_t (-2) }) {
        SysId s;
        s.software = SoftwareId::numbered (c);
        auto q = SysId::parse (s.encode_packet ());
        DN_ASSERT (q.software->is_code);
        DN_ASSERT_EQ (q.software->code, c);
    }
    // And a name round trips as a name.
    SysId s;
    s.software = SoftwareId::named ("VAX/VMS");
    auto q = SysId::parse (s.encode_packet ());
    DN_ASSERT (!q.software->is_code);
    DN_ASSERT_EQ (q.software->text, std::string ("VAX/VMS"));
}

DN_TEST (mop, unknown_system_id_items_are_kept)
{
    // Tag 8 is the system time, which nothing here reads.  A message
    // carrying one must survive a round trip so it can be passed on.
    SysId s;
    s.version = Version { 3, 0, 0 };
    s.unknown[8] = Bytes { 1, 2, 3, 4 };
    Bytes wire = s.encode_packet ();

    auto q = SysId::parse (wire);
    DN_ASSERT_EQ (q.unknown.size (), 1u);
    DN_ASSERT_EQ (q.unknown[8], (Bytes { 1, 2, 3, 4 }));
    DN_ASSERT_EQ (q.encode_packet (), wire);
}

DN_TEST (mop, message_dispatch_by_code)
{
    struct Case { Bytes wire; const char *name; };
    std::vector<Case> cases = {
        { Bytes { 5, 0, 0x34, 0x12 },        "RequestId" },
        { Bytes { 9, 0x34, 0x12 },           "RequestCounters" },
        { Bytes { 15 },                      "ConsoleRelease" },
        { Bytes { 13, 1, 2, 3, 4, 5, 6, 7, 8 }, "ConsoleRequest" },
    };
    for (const Case &c : cases) {
        auto p = MopPacketBase::parse_frame (c.wire);
        DN_ASSERT (p != nullptr);
        DN_ASSERT_EQ (std::string (p->packet_name ()), std::string (c.name));
        DN_ASSERT_EQ (p->encode_packet (), c.wire);
    }
    // An unassigned code is not a MOP message we know.
    DN_ASSERT (MopPacketBase::parse_frame (Bytes { 200, 0 }) == nullptr);
    DN_ASSERT (MopPacketBase::parse_frame (Bytes {}) == nullptr);
}

DN_TEST (mop, counters_roundtrip)
{
    Counters c;
    c.receipt    = 7;
    c.bytes_recv = 1234567;
    c.pkts_sent  = 89;
    Bytes wire = c.encode_packet ();
    // Fixed layout: one code byte plus the counters.
    DN_ASSERT_EQ (wire.size (), 1u + 2 + 2 + 7 * 4 + 4 + 4 + 8 * 2);

    auto q = Counters::parse (wire);
    DN_ASSERT_EQ (q.receipt, 7);
    DN_ASSERT_EQ (q.bytes_recv, 1234567u);
    DN_ASSERT_EQ (q.pkts_sent, 89u);
}

// ------------------------------------------------------------ on a LAN

DN_TEST (mop, mop_runs_only_where_it_is_asked_for)
{
    Config no = Config::from_string (
        "node 1.1 NODEA\ncircuit eth-0 Ethernet udp:1:127.0.0.1:2"
        " --random-address\n");
    Node n (no);
    DN_ASSERT (n.mop () != nullptr);
    DN_ASSERT (n.mop ()->circuits ().empty ());

    Config yes = Config::from_string (
        "node 1.1 NODEA\ncircuit eth-0 Ethernet udp:1:127.0.0.1:2"
        " --random-address --mop\n");
    Node m (yes);
    DN_ASSERT_EQ (m.mop ()->circuits ().size (), 1u);
    DN_ASSERT (m.mop ()->circuit ("eth-0") != nullptr);
}

DN_TEST (mop, a_point_to_point_circuit_gets_no_mop)
{
    // MOP is a broadcast protocol.  Asking for it on a Multinet circuit
    // is a configuration mistake, not something to half-do.
    Config c = Config::from_string (
        "node 1.1 NODEA\ncircuit mul-0 Multinet 127.0.0.1:1:connect --mop\n");
    Node n (c);
    DN_ASSERT (n.mop ()->circuits ().empty ());
}

DN_TEST (mop, nodes_hear_each_other_when_asked)
{
    Lan l;
    l.start ();

    // Nothing heard until somebody speaks.
    DN_ASSERT_EQ (l.ca ()->sysid ()->heard ().size (), 0u);

    // Ask B who it is; its answer goes to the console multicast address,
    // which A is listening on.
    l.ca ()->sysid ()->request_id (l.addr_b (), 42);
    DN_ASSERT (wait_until ([&] {
        return l.ca ()->sysid ()->heard ().size () == 1;
    }));

    const auto &heard = l.ca ()->sysid ()->heard ();
    const HeardSystem &h = heard.begin ()->second;
    DN_ASSERT_EQ (h.address, l.addr_b ());
    DN_ASSERT_EQ (h.sysid.receipt, 42);          // our receipt came back
    DN_ASSERT (h.sysid.loop);                    // it offers loopback
    DN_ASSERT (h.sysid.counters);
    DN_ASSERT (h.sysid.software.has_value ());
    DN_ASSERT_EQ (h.sysid.software->text, std::string ("DECnet/C++"));

    l.stop ();
}

DN_TEST (mop, an_unsolicited_system_id_is_recorded)
{
    Lan l;
    l.start ();
    // B announces itself to the multicast address without being asked.
    l.cb ()->sysid ()->send_id (console_multicast (), 0);
    DN_ASSERT (wait_until ([&] {
        return l.ca ()->sysid ()->heard ().size () == 1;
    }));
    DN_ASSERT_EQ (l.ca ()->sysid ()->heard ().begin ()->second.address,
                  l.addr_b ());
    l.stop ();
}

DN_TEST (mop, loopback_round_trip)
{
    // What NCP LOOP CIRCUIT does: send a message that asks the far station
    // to forward it back here.
    Lan l;
    l.start ();

    DN_ASSERT_EQ (l.ca ()->loop ()->replies (), 0u);
    l.ca ()->loop ()->loop (l.addr_b (), bytes_of ("round trip"));

    DN_ASSERT (wait_until ([&] { return l.ca ()->loop ()->replies () == 1; }));
    l.stop ();
}

DN_TEST (mop, loop_message_structure)
{
    // Loop request: forward to the requester, then reply.
    LoopReply rep;
    rep.receipt = 7;
    rep.payload = bytes_of ("data");

    LoopFwd fwd;
    fwd.dest = Bytes { 0xaa, 0x00, 0x04, 0x00, 0x36, 0x24 };
    fwd.payload = rep.encode ();

    LoopSkip top;
    top.skip = 0;
    top.payload = fwd.encode ();
    Bytes wire = top.encode ();

    // Two for the skip count, eight for the forward, four for the reply.
    DN_ASSERT_EQ (wire.size (), 2u + 8 + 4 + 4);
    DN_ASSERT_EQ (wire[2], LoopFwd::function_code);

    // What the far station sends back: the same bytes with skip at 8.
    LoopSkip back;
    back.skip = 8;
    back.payload = top.payload;
    Bytes ret = back.encode ();

    // The requester then finds the reply eight bytes further along.
    LoopSkip seen = LoopSkip::parse (ret);
    DN_ASSERT_EQ (seen.skip, 8);
    ByteView rest (ret.data () + 2 + seen.skip, ret.size () - 2 - seen.skip);
    DN_ASSERT_EQ (rest[0], LoopReply::function_code);
    LoopReply got;
    got.decode (rest);
    DN_ASSERT_EQ (got.receipt, 7);
    DN_ASSERT_EQ (got.payload, bytes_of ("data"));
}

DN_TEST (mop, a_loop_to_nobody_produces_no_reply)
{
    Lan l;
    l.start ();
    // An address nothing on this LAN answers to.
    l.ca ()->loop ()->loop (Macaddr::parse ("aa-00-04-00-99-99"),
                            bytes_of ("hello?"));
    std::this_thread::sleep_for (std::chrono::milliseconds (400));
    DN_ASSERT_EQ (l.ca ()->loop ()->replies (), 0u);

    // And a real one still works afterwards.
    l.ca ()->loop ()->loop (l.addr_b (), bytes_of ("still here"));
    DN_ASSERT (wait_until ([&] { return l.ca ()->loop ()->replies () == 1; }));
    l.stop ();
}
