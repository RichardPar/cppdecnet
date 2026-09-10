// Routing message formats: segments, the one's complement checksum, and
// the residue dispatch that tells a Phase III message from a Phase IV one
// at the same code point.

#include "harness.h"

#include "decnet/routing/packets.h"

using namespace decnet;
using namespace decnet::routing;

namespace {

Bytes bytes_of (std::initializer_list<int> v)
{
    Bytes b;
    for (int x : v) b.push_back (static_cast<std::uint8_t> (x));
    return b;
}

// A level 1 message with one segment, for the cases that just need one.
L1Routing sample_l1 ()
{
    L1Routing m;
    m.srcnode = 0x0401;             // 1.1
    RouteSegment s;
    s.startid = 1;
    s.entries = { route_entry (0, 0),      // ourselves: zero hops, zero cost
                  route_entry (1, 4),      // one hop away, cost 4
                  route_entry (INFHOPS, INFCOST) };   // unreachable
    m.segments.push_back (s);
    return m;
}

}   // namespace

DN_TEST (rmsg, entry_packing)
{
    // Hops in the top six bits, cost in the low ten.
    DN_ASSERT_EQ (route_entry (0, 0), 0x0000);
    DN_ASSERT_EQ (route_entry (1, 4), (1 << 10) | 4);
    DN_ASSERT_EQ (route_entry (INFHOPS, INFCOST), 0x7fff);
    DN_ASSERT_EQ (entry_hops (route_entry (5, 100)), 5u);
    DN_ASSERT_EQ (entry_cost (route_entry (5, 100)), 100u);
}

DN_TEST (rmsg, l1_roundtrip)
{
    L1Routing m = sample_l1 ();
    Bytes wire = m.encode_packet ();

    // Header, then count and startid, then the entries, then the checksum.
    DN_ASSERT_EQ (wire.size (), 4u + 2 + 2 + 3 * 2 + 2);
    DN_ASSERT_EQ (wire[0], 0x07);                 // control, type 3
    DN_ASSERT_EQ (wire[1], 0x01);
    DN_ASSERT_EQ (wire[2], 0x04);                 // srcnode 1.1
    DN_ASSERT_EQ (wire[3], 0x00);                 // reserved

    auto p = RoutingPacketBase::parse_frame (wire);
    DN_ASSERT (p != nullptr);
    DN_ASSERT_EQ (std::string (p->packet_name ()), std::string ("L1Routing"));

    auto *q = dynamic_cast<L1Routing *> (p.get ());
    DN_ASSERT (q != nullptr);
    DN_ASSERT_EQ (q->srcnode, 0x0401);
    DN_ASSERT_EQ (q->segments.size (), 1u);
    DN_ASSERT (q->segments[0] == m.segments[0]);
    DN_ASSERT_EQ (q->encode_packet (), wire);
}

DN_TEST (rmsg, phase3_and_phase4_differ_only_by_checksum)
{
    // Both use code point 0x07 with the same body; only the checksum seed
    // differs, and that is what picks the class.
    L1Routing four;
    four.srcnode = 1;
    RouteSegment s;
    s.startid = 1;
    s.entries = { route_entry (1, 4) };
    four.segments.push_back (s);

    PhaseIIIRouting three;
    three.srcnode = 1;
    three.entries = { route_entry (1, 4) };

    Bytes w4 = four.encode_packet ();
    Bytes w3 = three.encode_packet ();

    // The Phase IV message carries a segment header the Phase III one does
    // not, so compare what the dispatch does rather than the bytes.
    auto p4 = RoutingPacketBase::parse_frame (w4);
    auto p3 = RoutingPacketBase::parse_frame (w3);
    DN_ASSERT (p4 != nullptr && p3 != nullptr);
    DN_ASSERT_EQ (std::string (p4->packet_name ()), std::string ("L1Routing"));
    DN_ASSERT_EQ (std::string (p3->packet_name ()),
                  std::string ("PhaseIIIRouting"));

    // The two checksums over the same words differ by exactly the seed.
    std::uint16_t c4 = static_cast<std::uint16_t> (w4[w4.size () - 2]
                        | (w4[w4.size () - 1] << 8));
    std::uint16_t c3 = static_cast<std::uint16_t> (w3[w3.size () - 2]
                        | (w3[w3.size () - 1] << 8));
    DN_ASSERT_EQ (c3, route_entry (1, 4));        // seed 0, one word
    DN_ASSERT_EQ (c4, static_cast<std::uint16_t> (1 + 1 + 1 + route_entry (1, 4)));
}

DN_TEST (rmsg, phase3_roundtrip)
{
    PhaseIIIRouting m;
    m.srcnode = 5;
    m.entries = { route_entry (0, 0), route_entry (2, 9) };

    Bytes wire = m.encode_packet ();
    auto p = RoutingPacketBase::parse_frame (wire);
    DN_ASSERT (p != nullptr);
    DN_ASSERT_EQ (std::string (p->packet_name ()),
                  std::string ("PhaseIIIRouting"));
    auto *q = dynamic_cast<PhaseIIIRouting *> (p.get ());
    DN_ASSERT_EQ (q->entries, m.entries);
    DN_ASSERT_EQ (q->encode_packet (), wire);
}

DN_TEST (rmsg, l2_roundtrip)
{
    L2Routing m;
    m.srcnode = 0x0401;
    RouteSegment s;
    s.startid = 1;                    // areas, and there is no area zero
    s.entries = { route_entry (1, 4), route_entry (2, 8) };
    m.segments.push_back (s);

    Bytes wire = m.encode_packet ();
    DN_ASSERT_EQ (wire[0], 0x09);                 // control, type 4

    auto p = RoutingPacketBase::parse_frame (wire);
    DN_ASSERT (p != nullptr);
    DN_ASSERT_EQ (std::string (p->packet_name ()), std::string ("L2Routing"));
    DN_ASSERT_EQ (p->encode_packet (), wire);
}

DN_TEST (rmsg, checksum_error_yields_no_packet)
{
    L1Routing m = sample_l1 ();
    Bytes wire = m.encode_packet ();
    // Corrupt one entry: the residue is now neither 0xfffe nor 0xffff, so
    // no class matches and the packet is rejected.
    wire[6] ^= 0x01;
    DN_ASSERT (RoutingPacketBase::parse_frame (wire) == nullptr);

    // Corrupting the checksum itself has the same effect.
    Bytes w2 = m.encode_packet ();
    w2[w2.size () - 1] ^= 0x40;
    DN_ASSERT (RoutingPacketBase::parse_frame (w2) == nullptr);
}

DN_TEST (rmsg, malformed_payloads_rejected)
{
    // No payload at all.
    DN_ASSERT (RoutingPacketBase::parse_frame (
                   bytes_of ({ 0x07, 0x01, 0x04, 0x00 })) == nullptr);
    // Odd payload length: it cannot be a whole number of words.
    DN_ASSERT (RoutingPacketBase::parse_frame (
                   bytes_of ({ 0x07, 0x01, 0x04, 0x00, 0x01 })) == nullptr);
    // Shorter than the header.
    DN_ASSERT (RoutingPacketBase::parse_frame (bytes_of ({ 0x07, 0x01 }))
               == nullptr);
}

DN_TEST (rmsg, segment_ranges_are_validated)
{
    // A level 1 segment may not run past node 1023.
    L1Routing m;
    m.srcnode = 1;
    RouteSegment s;
    s.startid = 1022;
    s.entries = { 0, 0, 0, 0 };       // 1022 + 4 > 1024
    m.segments.push_back (s);
    DN_ASSERT (RoutingPacketBase::parse_frame (m.encode_packet ()) == nullptr);

    // A level 2 segment may not start at area zero.
    L2Routing l2;
    l2.srcnode = 1;
    RouteSegment z;
    z.startid = 0;
    z.entries = { 0 };
    l2.segments.push_back (z);
    DN_ASSERT (RoutingPacketBase::parse_frame (l2.encode_packet ()) == nullptr);

    // Nor run past area 63.
    L2Routing big;
    big.srcnode = 1;
    RouteSegment b;
    b.startid = 62;
    b.entries = { 0, 0, 0 };          // 62 + 3 > 64
    big.segments.push_back (b);
    DN_ASSERT (RoutingPacketBase::parse_frame (big.encode_packet ()) == nullptr);
}

DN_TEST (rmsg, multiple_segments)
{
    L1Routing m;
    m.srcnode = 1;
    RouteSegment a, b;
    a.startid = 1;
    a.entries = { route_entry (1, 4) };
    b.startid = 100;
    b.entries = { route_entry (2, 8), route_entry (3, 12) };
    m.segments = { a, b };

    auto p = RoutingPacketBase::parse_frame (m.encode_packet ());
    DN_ASSERT (p != nullptr);
    auto *q = dynamic_cast<L1Routing *> (p.get ());
    DN_ASSERT_EQ (q->segments.size (), 2u);
    DN_ASSERT_EQ (q->segments[1].startid, 100);
    DN_ASSERT_EQ (q->segments[1].entries.size (), 2u);
}

DN_TEST (rmsg, updates_add_the_circuit_cost_and_a_hop)
{
    // What the receiving router actually consumes: the sender's numbers
    // plus the cost of getting to the sender.
    L1Routing m = sample_l1 ();
    std::vector<RouteUpdate> u = m.updates (3);

    DN_ASSERT_EQ (u.size (), 3u);
    DN_ASSERT_EQ (u[0].id, 1u);
    DN_ASSERT_EQ (u[0].hops, 1u);          // 0 + 1
    DN_ASSERT_EQ (u[0].cost, 3u);          // 0 + circuit cost
    DN_ASSERT_EQ (u[1].id, 2u);
    DN_ASSERT_EQ (u[1].hops, 2u);          // 1 + 1
    DN_ASSERT_EQ (u[1].cost, 7u);          // 4 + 3
    DN_ASSERT_EQ (u[2].id, 3u);
    DN_ASSERT_EQ (u[2].hops, INFHOPS + 1);
    DN_ASSERT_EQ (u[2].cost, INFCOST + 3);
}

DN_TEST (rmsg, phase3_updates_start_at_node_one)
{
    PhaseIIIRouting m;
    m.entries = { route_entry (0, 0), route_entry (1, 5) };
    std::vector<RouteUpdate> u = m.updates (2);
    DN_ASSERT_EQ (u.size (), 2u);
    DN_ASSERT_EQ (u[0].id, 1u);
    DN_ASSERT_EQ (u[1].id, 2u);
    DN_ASSERT_EQ (u[1].cost, 7u);
}
