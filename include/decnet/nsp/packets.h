// decnet/nsp/packets.h -- NSP packet formats.
//
// Port of nsp_packets.py.
//
// PyDECnet describes the first byte as a bitmap with overlapping fields
// (subtype is bits 4-6; int_ls, bom and eom are bits 4, 5 and 6).  Here the
// byte is stored raw with accessors for each interpretation.

#ifndef DECNET_NSP_PACKETS_H
#define DECNET_NSP_PACKETS_H

#include "decnet/common/modulo.h"
#include "decnet/packet/group.h"
#include "decnet/packet/indexed.h"

#include <optional>

namespace decnet::nsp {

using namespace decnet::packet;

// NSP sequence numbers are integers modulo 4096, compared by the rules of
// RFC 1982 so that 4095 precedes 0.  Port of nsp_packets.Seq.
using Seq = Mod<4096>;

// Message type codes, from the MSGFLG byte.
enum MsgType : std::uint8_t { DATA = 0, ACK = 1, CTL = 2 };

// Acknowledgement subtypes.
enum AckSubtype : std::uint8_t { ACK_DATA = 0, ACK_OTHER = 1, ACK_CONN = 2 };

// Control subtypes.
enum CtlSubtype : std::uint8_t {
    NOP = 0, CI = 1, CC = 2, DI = 3, DC = 4, RCI = 6
};

// The NSP version a connect message declares, and the phase it implies.
enum NspVersion : std::uint8_t {
    VER_PH3 = 0,    // NSP 3.2
    VER_PH2 = 1,    // NSP 3.1
    VER_PH4 = 2,    // NSP 4.0
    VER_41  = 3     // NSP 4.1
};

const char *version_string (unsigned v) noexcept;
unsigned phase_of_version (unsigned v) noexcept;

// Flow control options a connect message can ask for.
enum FlowControl : std::uint8_t { SVC_NONE = 0, SVC_SEG = 1, SVC_MSG = 2 };

// --------------------------------------------------------------- AckNum
//
// Acknowledgement number: sequence number plus a qualifier for subchannel
// and ACK/NAK.  Present only when the top bit is set, so it decodes to an
// optional.  Port of nsp_packets.AckNum.
struct AckNum {
    enum Qual : std::uint8_t { ACKQ = 0, NAK = 1, XACK = 2, XNAK = 3 };

    Seq  num;
    Qual qual = ACKQ;

    // A "cross" acknowledgement refers to the other subchannel.
    bool is_cross () const noexcept { return qual == XACK || qual == XNAK; }
    bool is_nak () const noexcept { return qual == NAK || qual == XNAK; }

    std::string str () const;

    friend bool operator== (const AckNum &a, const AckNum &b) noexcept
    { return a.num == b.num && a.qual == b.qual; }
};

struct AckNumField {
    using value_type = std::optional<AckNum>;
    static void encode (Encoder &e, const value_type &v);
    static void decode (Decoder &d, value_type &v);
};

// Sequence numbers are two bytes little endian; the low 12 bits are the
// number and the high bits belong to the field sharing the word.
struct SeqField {
    using value_type = Seq;
    static void encode (Encoder &e, const value_type &v)
    { e.uint (v.value (), 2); }
    static void decode (Decoder &d, value_type &v)
    { v = Seq::wrap (d.uint (2)); }
};

// ----------------------------------------------------------- family root

void register_nsp_packets ();

struct NspPacketBase : Indexed<NspPacketBase> {
    static std::uint64_t index_key (ByteView b) { return require_byte (b, 0); }
    DN_PACKET_INDEX_REGISTERED (NspPacketBase, 128, register_nsp_packets)

    static std::unique_ptr<NspPacketBase> parse_frame (ByteView b) noexcept
    { return try_parse_indexed (b); }

    // Destination link address.  Virtual so the receive dispatcher can read it
    // before knowing the packet class.
    virtual std::uint16_t link_address () const noexcept = 0;
};

// Supplies that accessor for a class whose field is called dstaddr, which
// is all of them.
#define DN_NSP_LINK_ADDRESS                                                   \
    std::uint16_t link_address () const noexcept override { return dstaddr; }

// The MSGFLG byte, carried raw.  See the note at the top of this file.
#define DN_NSP_FLAGS                                                          \
    std::uint8_t msgflag = 0;                                                 \
    unsigned msg_type () const noexcept { return (msgflag >> 2) & 3; }        \
    unsigned msg_subtype () const noexcept { return (msgflag >> 4) & 7; }     \
    /* Data messages: which subchannel, and where in a message we are. */     \
    bool int_ls () const noexcept { return (msgflag & 0x10) != 0; }           \
    bool bom () const noexcept { return (msgflag & 0x20) != 0; }              \
    bool eom () const noexcept { return (msgflag & 0x40) != 0; }              \
    /* Other-data messages: interrupt rather than link service. */            \
    bool is_interrupt () const noexcept { return (msgflag & 0x20) != 0; }

// The link addresses and acknowledgement numbers most packets begin with.
// Port of nsp_packets.AckHdr.
#define DN_NSP_ACK_HDR                                                        \
    DN_NSP_FLAGS;                                                             \
    std::uint16_t dstaddr = 0;                                                \
    std::uint16_t srcaddr = 0;                                                \
    std::optional<AckNum> acknum;                                             \
    std::optional<AckNum> acknum2

#define DN_NSP_ACK_LAYOUT(Cls)                                                \
    field<B<1>>       (&Cls::msgflag, "msgflag"),                             \
    field<B<2>>       (&Cls::dstaddr, "dstaddr"),                             \
    field<B<2>>       (&Cls::srcaddr, "srcaddr"),                             \
    field<AckNumField> (&Cls::acknum,  "acknum"),                             \
    field<AckNumField> (&Cls::acknum2, "acknum2")

// ------------------------------------------------------ acknowledgements

struct AckData : IndexedBody<AckData, NspPacketBase> {
    static constexpr const char *name = "AckData";
    static constexpr std::uint8_t flag = 0x04;
    DN_NSP_ACK_HDR;
    AckData () { msgflag = flag; }
    DN_NSP_LINK_ADDRESS;
    static constexpr auto layout = fields (DN_NSP_ACK_LAYOUT (AckData));
};

struct AckOther : IndexedBody<AckOther, NspPacketBase> {
    static constexpr const char *name = "AckOther";
    static constexpr std::uint8_t flag = 0x14;
    DN_NSP_ACK_HDR;
    AckOther () { msgflag = flag; }
    DN_NSP_LINK_ADDRESS;
    static constexpr auto layout = fields (DN_NSP_ACK_LAYOUT (AckOther));
};

// Connect acknowledgement.  VAXELN appends extra bytes, which are accepted
// as payload, as in PyDECnet.
struct AckConn : IndexedBody<AckConn, NspPacketBase, Extra::allow> {
    static constexpr const char *name = "AckConn";
    static constexpr std::uint8_t flag = 0x24;
    DN_NSP_FLAGS;
    std::uint16_t dstaddr = 0;
    Bytes         payload;
    AckConn () { msgflag = flag; }
    DN_NSP_LINK_ADDRESS;
    static constexpr auto layout = fields (
        field<B<1>>    (&AckConn::msgflag, "msgflag"),
        field<B<2>>    (&AckConn::dstaddr, "dstaddr"),
        field<Payload> (&AckConn::payload, "payload"));
};

// -------------------------------------------------------- data messages

struct DataSeg : IndexedBody<DataSeg, NspPacketBase, Extra::allow> {
    static constexpr const char *name = "DataSeg";
    static constexpr std::uint8_t flag = 0x00;

    DN_NSP_ACK_HDR;
    // The segment number word has a delayed-acknowledgement flag in bit 12,
    // so it is a bitmap.  segnum() gives the typed sequence number.
    std::uint16_t segnum_bits = 0;
    bool          dly = false;
    Bytes         payload;

    DataSeg () { msgflag = flag; }
    DN_NSP_LINK_ADDRESS;

    Seq  segnum () const noexcept { return Seq::wrap (segnum_bits); }
    void set_segnum (Seq s) noexcept
    { segnum_bits = static_cast<std::uint16_t> (s.value ()); }

    static constexpr auto layout = fields (
        DN_NSP_ACK_LAYOUT (DataSeg),
        bm<DataSeg> (bmf (&DataSeg::segnum_bits, "segnum", 0, 12),
                     bmf (&DataSeg::dly,         "dly",   12, 1)),
        field<Payload> (&DataSeg::payload, "payload"));
};

struct IntMsg : IndexedBody<IntMsg, NspPacketBase, Extra::allow> {
    static constexpr const char *name = "IntMsg";
    static constexpr std::uint8_t flag = 0x30;
    DN_NSP_ACK_HDR;
    Seq   segnum;
    Bytes payload;
    IntMsg () { msgflag = flag; }
    DN_NSP_LINK_ADDRESS;
    static constexpr auto layout = fields (
        DN_NSP_ACK_LAYOUT (IntMsg),
        field<SeqField> (&IntMsg::segnum,  "segnum"),
        field<Payload>  (&IntMsg::payload, "payload"));
};

// Link service: the other end telling us how much more it will accept.
struct LinkSvcMsg : IndexedBody<LinkSvcMsg, NspPacketBase> {
    static constexpr const char *name = "LinkSvcMsg";
    static constexpr std::uint8_t flag = 0x10;

    enum FcVal : std::uint8_t { DATA_REQ = 0, INT_REQ = 1 };
    enum FcMod : std::uint8_t { NO_CHANGE = 0, XOFF = 1, XON = 2 };

    DN_NSP_ACK_HDR;
    Seq          segnum;
    std::uint8_t fcmod = 0;
    std::uint8_t fcval_int = 0;
    std::int64_t fcval = 0;

    LinkSvcMsg () { msgflag = flag; }
    DN_NSP_LINK_ADDRESS;

    static constexpr auto layout = fields (
        DN_NSP_ACK_LAYOUT (LinkSvcMsg),
        field<SeqField> (&LinkSvcMsg::segnum, "segnum"),
        bm<LinkSvcMsg> (bmf (&LinkSvcMsg::fcmod,     "fcmod",     0, 2),
                        bmf (&LinkSvcMsg::fcval_int, "fcval_int", 2, 2)),
        field<SIGNED<1>> (&LinkSvcMsg::fcval, "fcval"));

    // Reserved combinations, which the spec says to reject.
    bool valid () const noexcept { return fcval_int <= 1 && fcmod != 3; }
};

// ----------------------------------------------------- control messages

// What Connect Initiate and Connect Confirm share.
#define DN_NSP_CONN_HDR                                                       \
    DN_NSP_FLAGS;                                                             \
    std::uint16_t dstaddr = 0;                                                \
    std::uint16_t srcaddr = 0;                                                \
    std::uint8_t  mb1 = 1;                                                    \
    std::uint8_t  fcopt = 0;                                                  \
    std::uint8_t  mbz = 0;                                                    \
    std::uint32_t info = 0;                                                   \
    std::uint16_t segsize = 0

#define DN_NSP_CONN_LAYOUT(Cls)                                               \
    field<B<1>> (&Cls::msgflag, "msgflag"),                                   \
    field<B<2>> (&Cls::dstaddr, "dstaddr"),                                   \
    field<B<2>> (&Cls::srcaddr, "srcaddr"),                                   \
    bm<Cls> (bmf (&Cls::mb1,   "mb1",   0, 2),                                \
             bmf (&Cls::fcopt, "fcopt", 2, 2),                                \
             bmf (&Cls::mbz,   "mbz",   4, 4)),                               \
    field<EX<1>> (&Cls::info,    "info"),                                     \
    field<B<2>>  (&Cls::segsize, "segsize")

// Connect Initiate, or its retransmission -- the two differ only in
// subtype, so one class serves both.
struct ConnInit : IndexedBody<ConnInit, NspPacketBase, Extra::allow> {
    static constexpr const char *name = "ConnInit";
    static constexpr std::uint8_t flag = 0x18;      // and 0x68 when resent
    static constexpr std::uint8_t flag_retransmit = 0x68;

    DN_NSP_CONN_HDR;
    Bytes payload;
    ConnInit () { msgflag = flag; }
    DN_NSP_LINK_ADDRESS;

    bool retransmitted () const noexcept { return msgflag == flag_retransmit; }

    static constexpr auto layout = fields (
        DN_NSP_CONN_LAYOUT (ConnInit),
        field<Payload> (&ConnInit::payload, "payload"));
};

struct ConnConf : IndexedBody<ConnConf, NspPacketBase> {
    static constexpr const char *name = "ConnConf";
    static constexpr std::uint8_t flag = 0x28;

    DN_NSP_CONN_HDR;
    Bytes data_ctl;
    ConnConf () { msgflag = flag; }
    DN_NSP_LINK_ADDRESS;

    static constexpr auto layout = fields (
        DN_NSP_CONN_LAYOUT (ConnConf),
        field<I<16>> (&ConnConf::data_ctl, "data_ctl"));
};

// Disconnect Confirm.  Reason codes other than the three defined ones are
// treated as Disconnect Initiate, for Phase II compatibility.
struct DiscConf : IndexedBody<DiscConf, NspPacketBase> {
    static constexpr const char *name = "DiscConf";
    static constexpr std::uint8_t flag = 0x48;

    enum Reason : std::uint16_t { NO_RESOURCES = 1, NO_LINK = 41,
                                  DISC_COMPLETE = 42 };

    DN_NSP_FLAGS;
    std::uint16_t dstaddr = 0;
    std::uint16_t srcaddr = 0;
    std::uint16_t reason = 0;
    DiscConf () { msgflag = flag; }
    DN_NSP_LINK_ADDRESS;

    static constexpr auto layout = fields (
        field<B<1>> (&DiscConf::msgflag, "msgflag"),
        field<B<2>> (&DiscConf::dstaddr, "dstaddr"),
        field<B<2>> (&DiscConf::srcaddr, "srcaddr"),
        field<B<2>> (&DiscConf::reason,  "reason"));
};

// Disconnect Initiate: a disconnect confirm plus session control data.
struct DiscInit : IndexedBody<DiscInit, NspPacketBase> {
    static constexpr const char *name = "DiscInit";
    static constexpr std::uint8_t flag = 0x38;

    DN_NSP_FLAGS;
    std::uint16_t dstaddr = 0;
    std::uint16_t srcaddr = 0;
    std::uint16_t reason = 0;
    Bytes         data_ctl;
    DiscInit () { msgflag = flag; }
    DN_NSP_LINK_ADDRESS;

    static constexpr auto layout = fields (
        field<B<1>>  (&DiscInit::msgflag,  "msgflag"),
        field<B<2>>  (&DiscInit::dstaddr,  "dstaddr"),
        field<B<2>>  (&DiscInit::srcaddr,  "srcaddr"),
        field<B<2>>  (&DiscInit::reason,   "reason"),
        field<I<16>> (&DiscInit::data_ctl, "data_ctl"));
};

// Reason codes session control uses, copied from session.py.
inline constexpr std::uint16_t OBJ_FAIL = 38;
inline constexpr std::uint16_t UNREACH  = 39;

}   // namespace decnet::nsp

#endif  // DECNET_NSP_PACKETS_H
