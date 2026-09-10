// Tests for the BM and TLV field groups (packet.BM, packet.TLV).
//
// The TLV cases are modelled on MOP's System ID message, which is the
// richest user of the encoding in pydecnet: two byte tags, one byte
// lengths, unknown tags accepted, and one item whose value is a bitmap.

#include "harness.h"

#include "decnet/packet/group.h"
#include "decnet/packet/packet.h"

using namespace decnet;
using namespace decnet::packet;

namespace {

Bytes bytes_of (std::initializer_list<int> v)
{
    Bytes b;
    for (int x : v) b.push_back (static_cast<std::uint8_t> (x));
    return b;
}

// A one byte bitmap: three named ranges, as a routing layer flags byte.
struct Flags : Packet<Flags> {
    bool          control = false;
    std::uint8_t  type = 0;
    bool          pf = false;

    static constexpr auto layout = fields (
        bm<Flags> (bmf (&Flags::control, "control", 0, 1),
                   bmf (&Flags::type,    "type",    1, 3),
                   bmf (&Flags::pf,      "pf",      7, 1)));
};

// A bitmap wider than one byte, to check the derived length.
struct Wide : Packet<Wide> {
    std::uint16_t low = 0;
    std::uint8_t  high = 0;

    static constexpr auto layout = fields (
        bm<Wide> (bmf (&Wide::low,  "low",  0, 12),
                  bmf (&Wide::high, "high", 12, 4)));
};

// A TLV message in the shape of MOP System ID.
struct SysId : Packet<SysId> {
    std::uint16_t              receipt = 0;
    std::optional<Version>     version;
    std::optional<Macaddr>     hwaddr;
    std::optional<std::string> software;
    // The services bitmap, tag 2.
    bool loop = false, dump = false, carrier = false;
    std::map<unsigned, Bytes>  unknown;

    static constexpr auto layout = fields (
        field<B<2>> (&SysId::receipt, "receipt"),
        tlv_wild<SysId, 2, 1, Tolerant::yes> (
            &SysId::unknown,
            tlvf<1, VersionField> (&SysId::version, "version"),
            tlvg<2> (bm<SysId> (bmf (&SysId::loop,    "loop",    0, 1),
                                bmf (&SysId::dump,    "dump",    1, 1),
                                bmf (&SysId::carrier, "carrier", 5, 1)),
                     "services"),
            tlvf<7, MacaddrField> (&SysId::hwaddr,   "hwaddr"),
            tlvf<200, A<127>>     (&SysId::software, "software")));
};

// The strict form: an unknown tag is an error.
struct Strict : Packet<Strict> {
    std::optional<std::uint16_t> a;
    std::optional<Bytes>         b;

    static constexpr auto layout = fields (
        tlv<Strict, 1, 1> (tlvf<3, B<2>>  (&Strict::a, "a"),
                           tlvf<4, BV<4>> (&Strict::b, "b")));
};

}   // namespace

// ------------------------------------------------------------------- BM

DN_TEST (bm, roundtrip)
{
    Flags f;
    f.control = true;
    f.type    = 5;          // 0b101, at bits 1..3
    f.pf      = true;
    Bytes wire = f.encode ();
    DN_ASSERT_EQ (wire.size (), 1u);
    DN_ASSERT_EQ (wire[0], 0x01 | (5 << 1) | 0x80);

    Flags g = Flags::parse (wire);
    DN_ASSERT_EQ (g.control, true);
    DN_ASSERT_EQ (g.type, 5);
    DN_ASSERT_EQ (g.pf, true);
}

DN_TEST (bm, unset_bits_decode_false)
{
    Flags g = Flags::parse (bytes_of ({ 0x00 }));
    DN_ASSERT (!g.control);
    DN_ASSERT_EQ (g.type, 0);
    DN_ASSERT (!g.pf);
}

DN_TEST (bm, width_comes_from_the_highest_bit)
{
    // Bits 0..15 means two bytes, as (topbit + 8) / 8 gives.
    Wide w;
    w.low  = 0xabc;
    w.high = 0xd;
    Bytes wire = w.encode ();
    DN_ASSERT_EQ (wire.size (), 2u);
    DN_ASSERT_EQ (wire, bytes_of ({ 0xbc, 0xda }));

    Wide v = Wide::parse (wire);
    DN_ASSERT_EQ (v.low, 0xabc);
    DN_ASSERT_EQ (v.high, 0xd);
}

DN_TEST (bm, value_too_wide_is_rejected)
{
    Flags f;
    f.type = 8;             // needs four bits, the field has three
    DN_ASSERT_THROWS (FieldOverflow, f.encode ());
}

DN_TEST (bm, peek_reads_a_subfield_from_a_raw_buffer)
{
    // This is how indexed dispatch picks a class before parsing.
    Bytes wire = bytes_of ({ 0x0b });        // control=1, type=5
    using G = BMGroup<Flags>;
    DN_ASSERT_EQ (G::peek (wire, 0, 1, 1, 3), 5u);
    DN_ASSERT_EQ (G::peek (wire, 0, 1, 0, 1), 1u);
    DN_ASSERT_THROWS (MissingData, G::peek (Bytes {}, 0, 1, 0, 1));
}

// ------------------------------------------------------------------ TLV

DN_TEST (tlv, roundtrip_present_items_only)
{
    SysId s;
    s.receipt = 0x1234;
    s.version = Version { 3, 0, 0 };
    s.hwaddr  = Macaddr::parse ("aa-00-04-00-36-24");
    s.carrier = true;
    // software left unset, so its item must not appear.

    Bytes wire = s.encode ();
    DN_ASSERT_EQ (wire, bytes_of ({
        0x34, 0x12,                         // receipt
        1, 0, 3, 3, 0, 0,                   // tag 1, len 3, version 3.0.0
        2, 0, 1, 0x20,                      // tag 2, len 1, carrier bit
        7, 0, 6, 0xaa, 0x00, 0x04, 0x00, 0x36, 0x24 }));

    SysId t = SysId::parse (wire);
    DN_ASSERT_EQ (t.receipt, 0x1234);
    DN_ASSERT (t.version.has_value ());
    DN_ASSERT_EQ (t.version->str (), std::string ("3.0.0"));
    DN_ASSERT (t.hwaddr.has_value ());
    DN_ASSERT_EQ (t.hwaddr->str (), std::string ("aa-00-04-00-36-24"));
    DN_ASSERT (t.carrier);
    DN_ASSERT (!t.loop);
    DN_ASSERT (!t.software.has_value ());
}

DN_TEST (tlv, absent_items_stay_absent)
{
    SysId s;
    s.receipt = 0;
    Bytes wire = s.encode ();
    // Only the receipt and the always-present services bitmap.
    DN_ASSERT_EQ (wire, bytes_of ({ 0, 0, 2, 0, 1, 0 }));
}

DN_TEST (tlv, unknown_tags_are_kept_and_re_emitted)
{
    Bytes wire = bytes_of ({
        0, 0,                               // receipt
        1, 0, 3, 4, 0, 0,                   // version
        2, 0, 1, 0x00,                      // services
        99, 0, 2, 0xde, 0xad });            // an item we do not know

    SysId t = SysId::parse (wire);
    DN_ASSERT_EQ (t.unknown.size (), 1u);
    DN_ASSERT_EQ (t.unknown[99], bytes_of ({ 0xde, 0xad }));

    // A wild packet must survive a round trip with its unknown items.
    DN_ASSERT_EQ (t.encode (), wire);
}

DN_TEST (tlv, strict_rejects_unknown_tags)
{
    Bytes wire = bytes_of ({ 3, 2, 0x34, 0x12, 9, 1, 0xff });
    DN_ASSERT_THROWS (InvalidTag, Strict::parse (wire));

    // The known tags on their own parse.
    Strict ok = Strict::parse (bytes_of ({ 3, 2, 0x34, 0x12 }));
    DN_ASSERT (ok.a.has_value ());
    DN_ASSERT_EQ (*ok.a, 0x1234);
    DN_ASSERT (!ok.b.has_value ());
}

DN_TEST (tlv, value_extending_past_the_buffer_is_rejected)
{
    Bytes wire = bytes_of ({ 3, 8, 0x34, 0x12 });   // claims 8, has 2
    DN_ASSERT_THROWS (MissingData, Strict::parse (wire));
}

DN_TEST (tlv, item_not_fully_parsed_is_rejected_when_strict)
{
    // Tag 3 is a two byte field but the item claims four.
    Bytes wire = bytes_of ({ 3, 4, 0x34, 0x12, 0, 0 });
    DN_ASSERT_THROWS (ExtraData, Strict::parse (wire));
}

DN_TEST (tlv, tolerant_ignores_a_truncated_trailing_item)
{
    // A tolerant packet stops at a runt item instead of failing; pydecnet
    // marks MOP System ID tolerant because real implementations send these.
    Bytes wire = bytes_of ({ 0, 0, 1, 0, 3, 4, 0, 0, 7 });
    SysId t = SysId::parse (wire);
    DN_ASSERT (t.version.has_value ());
    DN_ASSERT_EQ (t.version->str (), std::string ("4.0.0"));

    // The strict equivalent fails on the same input.
    DN_ASSERT_THROWS (MissingData, Strict::parse (bytes_of ({ 3 })));
}

DN_TEST (tlv, empty_body_is_valid)
{
    Strict s = Strict::parse (Bytes {});
    DN_ASSERT (!s.a.has_value ());
    DN_ASSERT (!s.b.has_value ());
    DN_ASSERT (s.encode ().empty ());
}
