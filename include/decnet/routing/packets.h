// decnet/routing/packets.h -- routing packet formats.
//
// Port of routing_packets.py.  All classes are in one indexed family
// rooted at RoutingPacketBase, keyed on the first byte.
//
// PORT: Phase II NodeInit/NodeVerify.

#ifndef DECNET_ROUTING_PACKETS_H
#define DECNET_ROUTING_PACKETS_H

#include "decnet/packet/group.h"
#include "decnet/packet/indexed.h"

namespace decnet::routing {

class Circuit;

using namespace decnet::packet;

// Exceptions the routing layer raises on top of the generic decode ones.
// Ports of routing_packets.InvalidAddress, FormatError and ChecksumError.
struct RoutingDecodeError : DecodeError { using DecodeError::DecodeError; };
struct InvalidAddress : RoutingDecodeError
{ using RoutingDecodeError::RoutingDecodeError; };
struct FormatError : RoutingDecodeError
{ using RoutingDecodeError::RoutingDecodeError; };
struct ChecksumError : RoutingDecodeError
{ using RoutingDecodeError::RoutingDecodeError; };

// Router type codes as they appear in routing packets.  These are not the
// NICE encoding, which differs.
enum NodeTypeCode : std::uint8_t {
    PHASE2 = 0, L2ROUTER = 1, L1ROUTER = 2, ENDNODE = 3, UNKNOWN = 4
};

const char *ntype_string (unsigned t) noexcept;

// Max NPDU size, and the listen timeout multiplier for point to point
// circuits.  Ports of common.MTU and common.PTP_T3MULT.
inline constexpr std::uint16_t MTU = 576;
inline constexpr double PTP_T3MULT = 2.1;

// "No route" values.  Routing message entries hold hops in the top 6 bits
// and cost in the low 10.  Ports of common.INFHOPS and INFCOST.
inline constexpr unsigned INFHOPS = 31;
inline constexpr unsigned INFCOST = 1023;

// The high order four bytes of a Phase IV Ethernet address.
inline constexpr std::uint8_t HIORD[4] = { 0xaa, 0x00, 0x04, 0x00 };

// The byte a hello message's test data is filled with.
inline constexpr std::uint8_t HELLO_FILL = 0252;

// Broadcast circuit timing.  Ports of common.BCT3MULT, DRDELAY and T2.
inline constexpr double BCT3MULT = 3.1;
inline constexpr double DRDELAY = 5.0;
inline constexpr double T2 = 1.0;

// The MTU a LAN circuit advertises: the long header is fifteen bytes
// bigger than the short one.  Port of common.ETHMTU.
inline constexpr std::uint16_t ETHMTU = MTU + 21 - 6;

// ------------------------------------------------------------ family root

// Registers all routing packet classes.  See DN_PACKET_INDEX_REGISTERED.
void register_routing_packets ();

struct RoutingPacketBase : Indexed<RoutingPacketBase> {
    static std::uint64_t index_key (ByteView b) { return require_byte (b, 0); }
    DN_PACKET_INDEX_REGISTERED (RoutingPacketBase, 128, register_routing_packets)

    // Parse a frame into the right routing packet class.  Returns null if
    // malformed.
    static std::unique_ptr<RoutingPacketBase> parse_frame (ByteView b) noexcept
    { return try_parse_indexed (b); }
};

// ---------------------------------------------------------- data packets

// Short data packet header.  Port of routing_packets.ShortData.
struct ShortData : IndexedBody<ShortData, RoutingPacketBase, Extra::allow> {
    static constexpr const char *name = "ShortData";

    std::uint8_t sfpd = 2;
    bool         rqr = false;      // return to sender on failure
    bool         rts = false;      // this packet is being returned
    bool         vers = false;
    bool         pf = false;
    Nodeid       dstnode, srcnode;
    std::uint8_t visit = 0;
    Bytes        payload;

    // "Intra Ethernet" flag from the long header.  Never encoded.  Port of the
    // ROAnyField of the same name.
    bool ie = false;

    // The circuit this packet arrived on, or null when we originated it.
    // Never encoded, like ie; PyDECnet carries the source adjacency in the
    // same way, as the "src" attribute.  It is what lets the forwarding path
    // tell terminating from transit traffic and name the circuit in a packet
    // loss event.
    Circuit *src = nullptr;

    static constexpr auto layout = fields (
        bm<ShortData> (bmf (&ShortData::sfpd, "sfpd", 0, 3),
                       bmf (&ShortData::rqr,  "rqr",  3, 1),
                       bmf (&ShortData::rts,  "rts",  4, 1),
                       bmf (&ShortData::vers, "vers", 6, 1),
                       bmf (&ShortData::pf,   "pf",   7, 1)),
        field<NodeidField> (&ShortData::dstnode, "dstnode"),
        field<NodeidField> (&ShortData::srcnode, "srcnode"),
        bm<ShortData> (bmf (&ShortData::visit, "visit", 0, 6)),
        field<Payload> (&ShortData::payload, "payload"));
};

// Long data packet header, the form used on Ethernet.  Port of LongData.
struct LongData : IndexedBody<LongData, RoutingPacketBase, Extra::allow> {
    static constexpr const char *name = "LongData";

    std::uint8_t lfpd = 6;
    bool         rqr = false, rts = false;
    bool         ie = false;       // intra-Ethernet
    bool         vers = false, pf = false;
    Bytes        dsthi { HIORD, HIORD + 4 };
    Nodeid       dstnode;
    Bytes        srchi { HIORD, HIORD + 4 };
    Nodeid       srcnode;
    std::uint8_t visit = 0;
    Bytes        payload;

    static constexpr auto layout = fields (
        bm<LongData> (bmf (&LongData::lfpd, "lfpd", 0, 3),
                      bmf (&LongData::rqr,  "rqr",  3, 1),
                      bmf (&LongData::rts,  "rts",  4, 1),
                      bmf (&LongData::ie,   "ie",   5, 1),
                      bmf (&LongData::vers, "vers", 6, 1),
                      bmf (&LongData::pf,   "pf",   7, 1)),
        reserved<RES<2>, LongData> ("d-area, d-subarea"),
        field<BV<4>>       (&LongData::dsthi,   "dsthi"),
        field<NodeidField> (&LongData::dstnode, "dstnode"),
        reserved<RES<2>, LongData> ("s-area, s-subarea"),
        field<BV<4>>       (&LongData::srchi,   "srchi"),
        field<NodeidField> (&LongData::srcnode, "srcnode"),
        reserved<RES<1>, LongData> (),
        field<B<1>>        (&LongData::visit,   "visit"),
        reserved<RES<2>, LongData> ("s-class, pt"),
        field<Payload>     (&LongData::payload, "payload"));
};

// ------------------------------------------------------- control packets

// Fields at the start of every control packet.  Port of CtlHdr.  A macro
// rather than a base class, because layouts bind pointers to members.
#define DN_CTL_HDR_FIELDS                                                     \
    bool         control = true;                                              \
    std::uint8_t type = 0;                                                    \
    std::uint8_t ext_type = 0;                                                \
    bool         pf = false

#define DN_CTL_HDR_LAYOUT(Cls)                                                \
    bm<Cls> (bmf (&Cls::control,  "control",  0, 1),                          \
             bmf (&Cls::type,     "type",     1, 3),                          \
             bmf (&Cls::ext_type, "ext_type", 4, 3),                          \
             bmf (&Cls::pf,       "pf",       7, 1))

// The fields Phase III and Phase IV point to point inits share.
#define DN_PTP_INIT_FIELDS                                                    \
    DN_CTL_HDR_FIELDS;                                                        \
    Nodeid        srcnode;                                                    \
    std::uint8_t  ntype = 0;                                                  \
    bool          verif = false;                                              \
    bool          blo = false;                                                \
    std::uint16_t blksize = MTU;                                              \
    Version       tiver

#define DN_PTP_INIT_LAYOUT(Cls)                                               \
    DN_CTL_HDR_LAYOUT (Cls),                                                  \
    field<NodeidField> (&Cls::srcnode, "srcnode"),                            \
    bm<Cls> (bmf (&Cls::ntype, "ntype", 0, 2),                                \
             bmf (&Cls::verif, "verif", 2, 1),                                \
             bmf (&Cls::blo,   "blo",   3, 1)),                               \
    field<B<2>>         (&Cls::blksize, "blksize"),                           \
    field<VersionField> (&Cls::tiver,   "tiver")

// The second level of lookup for point to point inits: the protocol
// version byte at offset 6 says whether this is Phase III or Phase IV.
struct PtpInit34 : RoutingPacketBase {
    static std::uint64_t index_key (ByteView b) { return require_byte (b, 6); }
    DN_PACKET_SUBINDEX (RoutingPacketBase, PtpInit34, 4)
};

// Phase IV point to point init.  Port of routing_packets.PtpInit.
struct PtpInit : IndexedBody<PtpInit, PtpInit34> {
    static constexpr const char *name = "PtpInit";

    DN_PTP_INIT_FIELDS;
    std::uint16_t timer = 0;        // the hello timer the neighbour will use
    Bytes         reserved;

    PtpInit () { tiver = tiver_ph4; }

    static constexpr auto layout = fields (
        DN_PTP_INIT_LAYOUT (PtpInit),
        field<B<2>>  (&PtpInit::timer,    "timer"),
        field<I<64>> (&PtpInit::reserved, "reserved"));

    // A Phase IV address must have a node number.  Port of PtpInit.check.
    bool check () const noexcept { return srcnode.tid () != 0; }
};

// Phase III point to point init.  Port of PtpInit3.  No hello timer.
struct PtpInit3 : IndexedBody<PtpInit3, PtpInit34> {
    static constexpr const char *name = "PtpInit3";

    DN_PTP_INIT_FIELDS;
    Bytes reserved;

    PtpInit3 () { tiver = tiver_ph3; }

    static constexpr auto layout = fields (
        DN_PTP_INIT_LAYOUT (PtpInit3),
        field<I<64>> (&PtpInit3::reserved, "reserved"));

    // A Phase III address is an eight bit value.
    bool check () const noexcept
    { return srcnode.value () >= 1 && srcnode.value () <= 255; }
};

// Verification message.  Port of PtpVerify.
struct PtpVerify : IndexedBody<PtpVerify, RoutingPacketBase> {
    static constexpr const char *name = "PtpVerify";

    DN_CTL_HDR_FIELDS;
    Nodeid srcnode;
    Bytes  fcnval;

    PtpVerify () { type = 1; }

    static constexpr auto layout = fields (
        DN_CTL_HDR_LAYOUT (PtpVerify),
        field<NodeidField> (&PtpVerify::srcnode, "srcnode"),
        field<I<64>>       (&PtpVerify::fcnval,  "fcnval"));
};

// Point to point hello.  Port of PtpHello.
struct PtpHello : IndexedBody<PtpHello, RoutingPacketBase> {
    static constexpr const char *name = "PtpHello";

    DN_CTL_HDR_FIELDS;
    Nodeid srcnode;
    Bytes  testdata;

    PtpHello () { type = 2; }

    static constexpr auto layout = fields (
        DN_CTL_HDR_LAYOUT (PtpHello),
        field<NodeidField> (&PtpHello::srcnode,  "srcnode"),
        field<I<128>>      (&PtpHello::testdata, "testdata"));

    // The test data must be all 0252 bytes.  Port of testdata_re.
    bool testdata_valid () const noexcept;
};

// NOP, which Phase II nodes use as their hello.  Port of NopMsg.
struct NopMsg : IndexedBody<NopMsg, RoutingPacketBase, Extra::allow> {
    static constexpr const char *name = "NopMsg";

    std::uint8_t flags = 0x08;
    Bytes        payload;

    static constexpr auto layout = fields (
        field<B<1>>    (&NopMsg::flags,   "flags"),
        field<Payload> (&NopMsg::payload, "payload"));
};

// Build a hello message's test data: n bytes of 0252.
Bytes hello_testdata (std::size_t n = 10);

// ---------------------------------------------------- LAN hello messages
//
// Ports of routing_packets.RouterHello and EndnodeHello.

// Router hello.  elist lists each router the sender hears, with a two-way
// flag.
struct RouterHello : IndexedBody<RouterHello, RoutingPacketBase> {
    static constexpr const char *name = "RouterHello";

    DN_CTL_HDR_FIELDS;
    Version       tiver;
    Bytes         hiid { HIORD, HIORD + 4 };
    Nodeid        id;
    std::uint8_t  ntype = 0;
    std::uint16_t blksize = 0;
    std::uint8_t  prio = 0;
    std::uint16_t timer = 0;
    Bytes         elist;

    RouterHello () { type = 5; tiver = tiver_ph4; }

    static constexpr auto layout = fields (
        DN_CTL_HDR_LAYOUT (RouterHello),
        field<VersionField> (&RouterHello::tiver,   "tiver"),
        field<BV<4>>        (&RouterHello::hiid,    "hiid"),
        field<NodeidField>  (&RouterHello::id,      "id"),
        bm<RouterHello> (bmf (&RouterHello::ntype, "ntype", 0, 2)),
        field<B<2>>         (&RouterHello::blksize, "blksize"),
        field<B<1>>         (&RouterHello::prio,    "prio"),
        reserved<RES<1>, RouterHello> ("area"),
        field<B<2>>         (&RouterHello::timer,   "timer"),
        reserved<RES<1>, RouterHello> ("mpd"),
        field<I<244>>       (&RouterHello::elist,   "elist"));

    // Router hello ntype values: 1 is area router, 2 is level 1 router.
    // Ports of RouterHello.ntype_l1 and ntype_l2.
    static constexpr std::uint8_t ntype_l2 = 1;
    static constexpr std::uint8_t ntype_l1 = 2;
};

// The body of the elist field: a reserved header, then the list of
// adjacent routers.  Port of routing_packets.Elist.
struct Elist : Packet<Elist> {
    Bytes rslist;

    static constexpr auto layout = fields (
        reserved<RES<7>, Elist> (),
        field<I<236>> (&Elist::rslist, "rslist"));
};

// One router list entry: address, priority and two-way flag.  Port of
// RSent.
struct RSent : Packet<RSent> {
    Bytes        hiid { HIORD, HIORD + 4 };
    Nodeid       router;
    std::uint8_t prio = 0;
    bool         twoway = false;

    static constexpr auto layout = fields (
        field<BV<4>>       (&RSent::hiid,   "hiid"),
        field<NodeidField> (&RSent::router, "router"),
        bm<RSent> (bmf (&RSent::prio,   "prio",   0, 7),
                   bmf (&RSent::twoway, "twoway", 7, 1)));

    static constexpr std::size_t wire_size = 7;
};

// An endnode's hello.  "neighbor" names the router it is using, so a
// router can tell whether an endnode has chosen it.
struct EndnodeHello : IndexedBody<EndnodeHello, RoutingPacketBase> {
    static constexpr const char *name = "EndnodeHello";

    DN_CTL_HDR_FIELDS;
    Version       tiver;
    Bytes         hiid { HIORD, HIORD + 4 };
    Nodeid        id;
    std::uint8_t  ntype = ENDNODE;
    std::uint16_t blksize = 0;
    Bytes         neighbor;
    std::uint16_t timer = 0;
    Bytes         testdata;

    EndnodeHello () { type = 6; tiver = tiver_ph4; }

    static constexpr auto layout = fields (
        DN_CTL_HDR_LAYOUT (EndnodeHello),
        field<VersionField> (&EndnodeHello::tiver,   "tiver"),
        field<BV<4>>        (&EndnodeHello::hiid,    "hiid"),
        field<NodeidField>  (&EndnodeHello::id,      "id"),
        bm<EndnodeHello> (bmf (&EndnodeHello::ntype, "ntype", 0, 2)),
        field<B<2>>         (&EndnodeHello::blksize, "blksize"),
        reserved<RES<9>, EndnodeHello> ("area and seed"),
        field<BV<6>>        (&EndnodeHello::neighbor, "neighbor"),
        field<B<2>>         (&EndnodeHello::timer,    "timer"),
        reserved<RES<1>, EndnodeHello> (),
        field<I<128>>       (&EndnodeHello::testdata, "testdata"));
};

// ======================================================= routing messages
//
// Four byte header, segments, then a one's complement checksum.  These
// classes do their own encoding instead of using a layout.
//
// Code 0x07 is either a Phase III or a Phase IV level 1 routing message.
// The checksum is seeded with 1 for Phase IV and 0 for Phase III, so the
// residue (0xfffe or 0xffff) is used as a nested index key.

// One entry of a routing message: hops in the top six bits, cost in the
// low ten.  Port of routing_packets.RouteSegEntry.
constexpr std::uint16_t route_entry (unsigned hops, unsigned cost) noexcept
{ return static_cast<std::uint16_t> ((hops << 10) | (cost & 0x3ff)); }

constexpr unsigned entry_hops (std::uint16_t e) noexcept { return e >> 10; }
constexpr unsigned entry_cost (std::uint16_t e) noexcept { return e & 0x3ff; }

// Phase IV routing message segment: entries for consecutive destinations
// from startid.  Ports L1Segment and L2Segment.
struct RouteSegment {
    std::uint16_t              startid = 0;
    std::vector<std::uint16_t> entries;

    friend bool operator== (const RouteSegment &, const RouteSegment &) = default;
};

// One destination entry, with the circuit cost added and hop count
// incremented.  Port of the entries() generators.
struct RouteUpdate {
    unsigned id;
    unsigned hops;
    unsigned cost;
};

// The shared header and checksum machinery.
class RoutingMessage : public RoutingPacketBase {
public:
    bool          control = true;
    std::uint8_t  type = 3;
    std::uint8_t  ext_type = 0;
    bool          pf = false;
    std::uint16_t srcnode = 0;

    // Checksum residue of a routing message, used to select the class.  Throws
    // DecodeError if too short or odd length.
    static std::uint64_t index_key (ByteView b);

    // Every destination this message describes, with the receiving
    // circuit's cost folded in.
    virtual std::vector<RouteUpdate> updates (unsigned circuit_cost) const = 0;

protected:
    static constexpr std::size_t header_len = 4;

    void encode_header (Bytes &out) const;
    // Reads the header and returns the payload that follows it, with the
    // checksum word already removed and validated.
    std::vector<std::uint16_t> decode_header (ByteView b);

    // Append the body words and the checksum that covers them.  seed is 1
    // for Phase IV, 0 for Phase III.
    static void append_body (Bytes &out, const std::vector<std::uint16_t> &words,
                             unsigned seed);
};

// The second level of lookup for code point 0x07, keyed on the residue.
struct P34Routing : RoutingMessage {
    DN_PACKET_SUBINDEX (RoutingPacketBase, RoutingMessage, 0)
};

// And for code point 0x09, which is level 2 only.
struct P4L2Routing : RoutingMessage {
    DN_PACKET_SUBINDEX (RoutingPacketBase, RoutingMessage, 0)
};

// A Phase IV routing message: a list of segments.  Ports L1Routing and
// L2Routing, which differ only in code point and what a valid segment is.
class Ph4RoutingMessage : public RoutingMessage {
public:
    std::vector<RouteSegment> segments;

    std::size_t decode_into (ByteView b) override;
    Bytes       encode_packet () const override;
    std::vector<RouteUpdate> updates (unsigned circuit_cost) const override;

    // Lowest destination: 0 for level 1, 1 for level 2.  Port of lowid.
    virtual unsigned lowid () const noexcept = 0;

protected:
    // Is this a segment range this message type can carry?
    virtual bool valid_segment (const RouteSegment &s) const noexcept = 0;
};

class L1Routing : public Ph4RoutingMessage {
public:
    static constexpr const char *name = "L1Routing";
    L1Routing () { type = 3; }
    const char *packet_name () const noexcept override { return name; }
    unsigned lowid () const noexcept override { return 0; }
    static std::unique_ptr<RoutingPacketBase> make ()
    { return std::make_unique<L1Routing> (); }

protected:
    bool valid_segment (const RouteSegment &s) const noexcept override;
};

class L2Routing : public Ph4RoutingMessage {
public:
    static constexpr const char *name = "L2Routing";
    L2Routing () { type = 4; }
    const char *packet_name () const noexcept override { return name; }
    unsigned lowid () const noexcept override { return 1; }
    static std::unique_ptr<RoutingPacketBase> make ()
    { return std::make_unique<L2Routing> (); }

protected:
    bool valid_segment (const RouteSegment &s) const noexcept override;
};

// A Phase III routing message: one unsegmented run of entries covering all
// destinations from node 1 upward.  Port of PhaseIIIRouting.
class PhaseIIIRouting : public RoutingMessage {
public:
    static constexpr const char *name = "PhaseIIIRouting";
    PhaseIIIRouting () { type = 3; }

    std::vector<std::uint16_t> entries;

    const char *packet_name () const noexcept override { return name; }
    std::size_t decode_into (ByteView b) override;
    Bytes       encode_packet () const override;
    std::vector<RouteUpdate> updates (unsigned circuit_cost) const override;

    static std::unique_ptr<RoutingPacketBase> make ()
    { return std::make_unique<PhaseIIIRouting> (); }
};

}   // namespace decnet::routing

#endif  // DECNET_ROUTING_PACKETS_H
