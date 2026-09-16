// Tests for packet class lookup by code (packet.Indexed).
//
// Uses the routing family: masked registration on the flags byte, a
// nested index, and a default class for unknown versions.

#include "harness.h"

#include "decnet/packet/group.h"
#include "decnet/packet/indexed.h"

using namespace decnet;
using namespace decnet::packet;

namespace {

Bytes bytes_of (std::initializer_list<int> v)
{
    Bytes b;
    for (int x : v) b.push_back (static_cast<std::uint8_t> (x));
    return b;
}

// ---- the family root, keyed on the flags byte, as RoutingPacketBase is.
struct RoutingBase : Indexed<RoutingBase> {
    static std::uint64_t index_key (ByteView b) { return require_byte (b, 0); }
    DN_PACKET_INDEX (RoutingBase, 128)
};

// ---- a data packet: flags 0x02 under mask 0xc7, with a payload.
struct ShortData : IndexedBody<ShortData, RoutingBase, Extra::allow> {
    static constexpr const char *name = "ShortData";

    std::uint8_t sfpd = 2;
    bool         rqr = false, rts = false, pf = false;
    Nodeid       dstnode, srcnode;
    std::uint8_t visit = 0;
    Bytes        payload;

    static constexpr auto layout = fields (
        bm<ShortData> (bmf (&ShortData::sfpd, "sfpd", 0, 3),
                       bmf (&ShortData::rqr,  "rqr",  3, 1),
                       bmf (&ShortData::rts,  "rts",  4, 1),
                       bmf (&ShortData::pf,   "pf",   7, 1)),
        field<NodeidField> (&ShortData::dstnode, "dstnode"),
        field<NodeidField> (&ShortData::srcnode, "srcnode"),
        bm<ShortData> (bmf (&ShortData::visit, "visit", 0, 6)),
        field<Payload> (&ShortData::payload, "payload"));
};
DN_REGISTER_PACKET_MASKED (RoutingBase, ShortData, 0x02, 0xc7);

// ---- a hello packet: a plain single key registration.
struct Hello : IndexedBody<Hello, RoutingBase> {
    static constexpr const char *name = "Hello";

    std::uint8_t flags = 0x05;
    Nodeid       srcnode;

    static constexpr auto layout = fields (
        field<B<1>>        (&Hello::flags,   "flags"),
        field<NodeidField> (&Hello::srcnode, "srcnode"));
};
DN_REGISTER_PACKET (RoutingBase, Hello, 0x05);

// ---- the nested case: flags 0x01 selects a second index keyed on the
// version byte at offset 6, exactly as PtpInit34 does.
struct PtpInit : RoutingBase {
    static std::uint64_t index_key (ByteView b) { return require_byte (b, 6); }
    DN_PACKET_SUBINDEX (RoutingBase, PtpInit, 4)
};
DN_REGISTER_NESTED_MASKED (RoutingBase, PtpInit, 0x01, 0x8f);

// The fields PtpInit34 shares, spelled once.  In PyDECnet these come from
// the CtlHdr and PtpInit34 layouts that both phases inherit.
#define PTP_INIT_COMMON(Cls)                                                  \
    bm<Cls> (bmf (&Cls::control,  "control",  0, 1),                          \
             bmf (&Cls::type,     "type",     1, 3),                          \
             bmf (&Cls::ext_type, "ext_type", 4, 3),                          \
             bmf (&Cls::pf,       "pf",       7, 1)),                         \
    field<NodeidField> (&Cls::srcnode, "srcnode"),                            \
    bm<Cls> (bmf (&Cls::ntype, "ntype", 0, 2),                                \
             bmf (&Cls::verif, "verif", 2, 1),                                \
             bmf (&Cls::blo,   "blo",   3, 1)),                               \
    field<B<2>>         (&Cls::blksize, "blksize"),                           \
    field<VersionField> (&Cls::tiver,   "tiver")

#define PTP_INIT_FIELDS                                                       \
    bool          control = true;                                             \
    std::uint8_t  type = 0, ext_type = 0;                                     \
    bool          pf = false;                                                 \
    Nodeid        srcnode;                                                    \
    std::uint8_t  ntype = 0;                                                  \
    bool          verif = false, blo = false;                                 \
    std::uint16_t blksize = 0;                                                \
    Version       tiver

struct PtpInit4 : IndexedBody<PtpInit4, PtpInit> {
    static constexpr const char *name = "PtpInit4";

    PTP_INIT_FIELDS;
    std::uint16_t timer = 0;
    Bytes         reserved;

    static constexpr auto layout = fields (
        PTP_INIT_COMMON (PtpInit4),
        field<B<2>>  (&PtpInit4::timer,    "timer"),
        field<I<64>> (&PtpInit4::reserved, "reserved"));
};
DN_REGISTER_PACKET (PtpInit, PtpInit4, 2);      // version 2 == Phase IV

struct PtpInit3 : IndexedBody<PtpInit3, PtpInit> {
    static constexpr const char *name = "PtpInit3";

    PTP_INIT_FIELDS;
    Bytes reserved;

    static constexpr auto layout = fields (
        PTP_INIT_COMMON (PtpInit3),
        field<I<64>> (&PtpInit3::reserved, "reserved"));
};
DN_REGISTER_PACKET (PtpInit, PtpInit3, 1);      // version 1 == Phase III

// A point to point init for the given protocol version.  Phase IV carries
// a hello timer that Phase III does not.
Bytes ptp_init (int version)
{
    Bytes b = bytes_of ({ 0x01, 0x36, 0x24, 0x02, 0x40, 0x02, version, 0, 0 });
    if (version == 2) { b.push_back (0x3c); b.push_back (0x00); }
    b.push_back (0x00);                 // the empty reserved image field
    return b;
}

}   // namespace

DN_TEST (indexed, picks_the_class_from_the_flags_byte)
{
    // 0x02 under mask 0xc7 -- a short data packet carrying "hi".
    Bytes wire = bytes_of ({ 0x02, 0x36, 0x24, 0x01, 0x04, 0x00, 'h', 'i' });
    auto p = RoutingBase::parse_indexed (wire);
    DN_ASSERT_EQ (std::string (p->packet_name ()), std::string ("ShortData"));

    auto *sd = dynamic_cast<ShortData *> (p.get ());
    DN_ASSERT (sd != nullptr);
    DN_ASSERT_EQ (sd->dstnode, Nodeid::parse ("9.54"));
    DN_ASSERT_EQ (sd->srcnode, Nodeid::parse ("1.1"));
    DN_ASSERT_EQ (sd->payload, bytes_of ({ 'h', 'i' }));
}

DN_TEST (indexed, mask_claims_the_whole_key_set)
{
    // Every flags value whose 0xc7 bits are 0x02 maps to ShortData.
    for (int extra : { 0x00, 0x08, 0x10, 0x18, 0x20, 0x30, 0x38 }) {
        Bytes wire = bytes_of ({ 0x02 | extra, 0x36, 0x24, 0x01, 0x04, 0x00 });
        auto p = RoutingBase::parse_indexed (wire);
        DN_ASSERT_EQ (std::string (p->packet_name ()), std::string ("ShortData"));
    }
    // The rqr and rts bits survive into the parsed packet.
    Bytes wire = bytes_of ({ 0x02 | 0x08, 0x36, 0x24, 0x01, 0x04, 0x00 });
    auto p = RoutingBase::parse_indexed (wire);
    DN_ASSERT (dynamic_cast<ShortData *> (p.get ())->rqr);
}

DN_TEST (indexed, single_key_registration)
{
    Bytes wire = bytes_of ({ 0x05, 0x36, 0x24 });
    auto p = RoutingBase::parse_indexed (wire);
    DN_ASSERT_EQ (std::string (p->packet_name ()), std::string ("Hello"));
    DN_ASSERT_EQ (dynamic_cast<Hello *> (p.get ())->srcnode,
                  Nodeid::parse ("9.54"));
}

DN_TEST (indexed, nested_index_on_a_later_field)
{
    // Flags say "point to point init"; the version byte at offset 6 then
    // selects the phase.
    auto p4 = RoutingBase::parse_indexed (ptp_init (2));
    DN_ASSERT_EQ (std::string (p4->packet_name ()), std::string ("PtpInit4"));

    auto p3 = RoutingBase::parse_indexed (ptp_init (1));
    DN_ASSERT_EQ (std::string (p3->packet_name ()), std::string ("PtpInit3"));
}

DN_TEST (indexed, unknown_key_is_a_decode_error)
{
    Bytes wire = bytes_of ({ 0x7f, 0, 0 });
    DN_ASSERT_THROWS (DecodeError, RoutingBase::parse_indexed (wire));
    // try_parse is the form a receive path uses: no exception, no packet.
    DN_ASSERT (RoutingBase::try_parse_indexed (wire) == nullptr);
}

DN_TEST (indexed, default_class_covers_unknown_nested_keys)
{
    // PtpInit34 supplies itself as the default for an unrecognised
    // version.  Here PtpInit4 plays that role.
    PtpInit::index ().set_default (&PtpInit4::make, "PtpInit4");
    // A Phase IV shaped message claiming an unrecognised protocol version.
    Bytes wire = ptp_init (2);
    wire[6] = 3;
    auto p = RoutingBase::parse_indexed (wire);
    DN_ASSERT_EQ (std::string (p->packet_name ()), std::string ("PtpInit4"));
}

DN_TEST (indexed, key_beyond_the_index_limit_is_rejected)
{
    // The routing index is nlist (128), so a flags byte with the top bit
    // set is out of range rather than merely unknown.
    Bytes wire = bytes_of ({ 0x80, 0, 0 });
    DN_ASSERT_THROWS (DecodeError, RoutingBase::parse_indexed (wire));
}

DN_TEST (indexed, empty_buffer_is_a_decode_error_not_a_crash)
{
    DN_ASSERT_THROWS (MissingData, RoutingBase::parse_indexed (Bytes {}));
    DN_ASSERT (RoutingBase::try_parse_indexed (Bytes {}) == nullptr);
    // Long enough to index, too short for the nested key at offset 6.
    DN_ASSERT (RoutingBase::try_parse_indexed (bytes_of ({ 0x01, 0, 0 }))
               == nullptr);
}

DN_TEST (indexed, decodes_a_real_python_init_message)
{
    // Captured from a PyDECnet V1.1.1 node (1.1, endnode) over Multinet TCP.
    Bytes wire = bytes_of ({ 0x01, 0x01, 0x04, 0x03, 0x40, 0x02,
                             0x02, 0x00, 0x00, 0x3c, 0x00, 0x00 });

    auto p = RoutingBase::parse_indexed (wire);
    DN_ASSERT_EQ (std::string (p->packet_name ()), std::string ("PtpInit4"));

    auto *init = dynamic_cast<PtpInit4 *> (p.get ());
    DN_ASSERT (init != nullptr);
    DN_ASSERT (init->control);
    DN_ASSERT_EQ (init->type, 0);                       // point to point init
    DN_ASSERT_EQ (init->srcnode, Nodeid::parse ("1.1"));
    DN_ASSERT_EQ (init->ntype, 3);                      // endnode
    DN_ASSERT (!init->verif);
    DN_ASSERT_EQ (init->blksize, 576);
    DN_ASSERT_EQ (init->tiver.str (), std::string ("2.0.0"));
    DN_ASSERT_EQ (init->timer, 60);                     // hello timer
    DN_ASSERT (init->reserved.empty ());

    // And it re-encodes to exactly the bytes PyDECnet sent.
    DN_ASSERT_EQ (p->encode_packet (), wire);
}

DN_TEST (indexed, encode_through_the_base_pointer)
{
    Bytes wire = bytes_of ({ 0x02, 0x36, 0x24, 0x01, 0x04, 0x00, 'h', 'i' });
    auto p = RoutingBase::parse_indexed (wire);
    DN_ASSERT_EQ (p->encode_packet (), wire);
}
