// Port of tests/test_routingpacket.py: the routing layer packet formats.

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

}   // namespace

DN_TEST (rpacket, short_data_roundtrip)
{
    ShortData p;
    p.dstnode = Nodeid::parse ("1.1");
    p.srcnode = Nodeid::parse ("9.54");
    p.visit   = 1;
    p.payload = bytes_of ({ 'd', 'a', 't', 'a' });

    Bytes wire = p.encode ();
    DN_ASSERT_EQ (wire, bytes_of ({ 0x02,               // sfpd = 2
                                    0x01, 0x04,         // dst 1.1
                                    0x36, 0x24,         // src 9.54
                                    0x01,               // visit
                                    'd', 'a', 't', 'a' }));

    ShortData q = ShortData::parse (wire);
    DN_ASSERT_EQ (q.dstnode, p.dstnode);
    DN_ASSERT_EQ (q.srcnode, p.srcnode);
    DN_ASSERT_EQ (q.visit, 1);
    DN_ASSERT_EQ (q.payload, p.payload);
    DN_ASSERT (!q.rqr);
    DN_ASSERT (!q.rts);
}

DN_TEST (rpacket, short_data_flags)
{
    ShortData p;
    p.rqr = true;               // return to sender if undeliverable
    p.rts = true;               // ... and this one is being returned
    p.dstnode = Nodeid::parse ("1.1");
    p.srcnode = Nodeid::parse ("1.2");
    Bytes wire = p.encode ();
    DN_ASSERT_EQ (wire[0], 0x02 | 0x08 | 0x10);

    ShortData q = ShortData::parse (wire);
    DN_ASSERT (q.rqr);
    DN_ASSERT (q.rts);
}

DN_TEST (rpacket, long_data_roundtrip)
{
    LongData p;
    p.ie      = true;
    p.dstnode = Nodeid::parse ("1.1");
    p.srcnode = Nodeid::parse ("9.54");
    p.visit   = 2;
    p.payload = bytes_of ({ 'x' });

    Bytes wire = p.encode ();
    DN_ASSERT_EQ (wire, bytes_of ({ 0x06 | 0x20,        // lfpd = 6, ie
                                    0x00, 0x00,         // d-area, d-subarea
                                    0xaa, 0x00, 0x04, 0x00,
                                    0x01, 0x04,         // dst 1.1
                                    0x00, 0x00,         // s-area, s-subarea
                                    0xaa, 0x00, 0x04, 0x00,
                                    0x36, 0x24,         // src 9.54
                                    0x00,               // reserved
                                    0x02,               // visit
                                    0x00, 0x00,         // s-class, pt
                                    'x' }));

    LongData q = LongData::parse (wire);
    DN_ASSERT_EQ (q.dstnode, p.dstnode);
    DN_ASSERT_EQ (q.srcnode, p.srcnode);
    DN_ASSERT_EQ (q.visit, 2);
    DN_ASSERT (q.ie);
    DN_ASSERT_EQ (q.payload, p.payload);
}

DN_TEST (rpacket, ptp_init_phase4)
{
    PtpInit p;
    p.srcnode = Nodeid::parse ("1.1");
    p.ntype   = ENDNODE;
    p.blksize = MTU;
    p.timer   = 60;

    Bytes wire = p.encode ();
    // The exact bytes a the Python endnode sends.
    DN_ASSERT_EQ (wire, bytes_of ({ 0x01, 0x01, 0x04, 0x03, 0x40, 0x02,
                                    0x02, 0x00, 0x00, 0x3c, 0x00, 0x00 }));

    PtpInit q = PtpInit::parse (wire);
    DN_ASSERT (q.control);
    DN_ASSERT_EQ (q.type, 0);
    DN_ASSERT_EQ (q.srcnode, Nodeid::parse ("1.1"));
    DN_ASSERT_EQ (q.ntype, ENDNODE);
    DN_ASSERT_EQ (q.blksize, 576);
    DN_ASSERT_EQ (q.timer, 60);
    DN_ASSERT_EQ (q.tiver.str (), std::string ("2.0.0"));
    DN_ASSERT (q.check ());
}

DN_TEST (rpacket, ptp_init_rejects_zero_node)
{
    PtpInit p;
    p.srcnode = Nodeid (1, 0);
    DN_ASSERT (!p.check ());
}

DN_TEST (rpacket, ptp_verify_roundtrip)
{
    PtpVerify p;
    p.srcnode = Nodeid::parse ("1.1");
    p.fcnval  = bytes_of ({ 's', 'e', 'c' });
    Bytes wire = p.encode ();
    DN_ASSERT_EQ (wire, bytes_of ({ 0x03, 0x01, 0x04, 3, 's', 'e', 'c' }));

    PtpVerify q = PtpVerify::parse (wire);
    DN_ASSERT_EQ (q.type, 1);
    DN_ASSERT_EQ (q.srcnode, Nodeid::parse ("1.1"));
    DN_ASSERT_EQ (q.fcnval, p.fcnval);
}

DN_TEST (rpacket, ptp_hello_roundtrip_and_testdata)
{
    PtpHello p;
    p.srcnode  = Nodeid::parse ("9.54");
    p.testdata = hello_testdata ();

    Bytes wire = p.encode ();
    DN_ASSERT_EQ (wire[0], 0x05);
    DN_ASSERT_EQ (wire.size (), 1u + 2 + 1 + 10);

    PtpHello q = PtpHello::parse (wire);
    DN_ASSERT_EQ (q.type, 2);
    DN_ASSERT_EQ (q.srcnode, Nodeid::parse ("9.54"));
    DN_ASSERT (q.testdata_valid ());

    // Anything other than the fill byte means the link is corrupting data
    // or the neighbour is confused; the circuit is taken down for it.
    q.testdata[3] = 0;
    DN_ASSERT (!q.testdata_valid ());
}

DN_TEST (rpacket, family_dispatch_from_the_flags_byte)
{
    struct Case { Bytes wire; const char *name; };
    std::vector<Case> cases = {
        { bytes_of ({ 0x02, 0x01, 0x04, 0x36, 0x24, 0x00 }), "ShortData" },
        { bytes_of ({ 0x06, 0, 0, 0xaa, 0, 4, 0, 0x01, 0x04, 0, 0,
                      0xaa, 0, 4, 0, 0x36, 0x24, 0, 0, 0, 0 }), "LongData" },
        { bytes_of ({ 0x01, 0x01, 0x04, 0x03, 0x40, 0x02,
                      0x02, 0x00, 0x00, 0x3c, 0x00, 0x00 }), "PtpInit" },
        { bytes_of ({ 0x01, 0x01, 0x04, 0x03, 0x40, 0x02,
                      0x01, 0x00, 0x00, 0x00 }), "PtpInit3" },
        { bytes_of ({ 0x03, 0x01, 0x04, 0x00 }), "PtpVerify" },
        { bytes_of ({ 0x05, 0x01, 0x04, 0x00 }), "PtpHello" },
        { bytes_of ({ 0x08, 0xaa, 0xaa }), "NopMsg" },
    };
    for (const Case &c : cases) {
        auto p = RoutingPacketBase::parse_frame (c.wire);
        DN_ASSERT (p != nullptr);
        DN_ASSERT_EQ (std::string (p->packet_name ()), std::string (c.name));
        // Everything must survive a round trip through the wire form.
        DN_ASSERT_EQ (p->encode_packet (), c.wire);
    }
}

DN_TEST (rpacket, data_flags_bits_do_not_change_the_class)
{
    // The flags byte mixes the type with per-packet bits, so the class
    // lookup is masked.  For a data packet the mask is 0xc7, so bits 3, 4
    // and 5 -- rqr, rts and the long form's ie -- are the don't-care ones.
    // vers and pf are inside the mask and so are part of the class key.
    for (int extra : { 0x00, 0x08, 0x10, 0x18, 0x20, 0x38 }) {
        Bytes wire = bytes_of ({ 0x02 | extra, 0x01, 0x04, 0x36, 0x24, 0x00 });
        auto p = RoutingPacketBase::parse_frame (wire);
        DN_ASSERT (p != nullptr);
        DN_ASSERT_EQ (std::string (p->packet_name ()), std::string ("ShortData"));
    }
}

DN_TEST (rpacket, flags_byte_above_the_index_is_rejected)
{
    // The class index is 128 entries, keyed on the flags byte, so a byte
    // with the top bit set is out of range rather than merely unknown.
    DN_ASSERT (RoutingPacketBase::parse_frame (
                   bytes_of ({ 0x82, 0x01, 0x04, 0x36, 0x24, 0x00 }))
               == nullptr);
}

DN_TEST (rpacket, malformed_packets_return_null)
{
    DN_ASSERT (RoutingPacketBase::parse_frame (Bytes {}) == nullptr);
    DN_ASSERT (RoutingPacketBase::parse_frame (bytes_of ({ 0x02 })) == nullptr);
    DN_ASSERT (RoutingPacketBase::parse_frame (bytes_of ({ 0x7f, 0, 0 })) == nullptr);
    // A point to point init too short to hold its version byte.
    DN_ASSERT (RoutingPacketBase::parse_frame (bytes_of ({ 0x01, 0, 0 })) == nullptr);
}

DN_TEST (rpacket, ntype_names)
{
    DN_ASSERT_EQ (std::string (ntype_string (ENDNODE)), std::string ("Endnode"));
    DN_ASSERT_EQ (std::string (ntype_string (L1ROUTER)),
                  std::string ("L1 router"));
    DN_ASSERT_EQ (std::string (ntype_string (L2ROUTER)),
                  std::string ("Area router"));
}
