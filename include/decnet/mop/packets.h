// decnet/mop/packets.h -- MOP message formats.
//
// Port of the packet classes in mop.py.  MOP is the maintenance protocol:
// it announces what a node is, answers requests for its identity and its
// Ethernet counters, and provides the loopback test that NCP LOOP CIRCUIT
// uses.  None of it involves routing or NSP; it rides directly on the
// datalink.
//
// Two protocol types are involved.  Everything with a MopHdr travels on
// 60-01 in the DEC padded format.  Loopback messages travel on 90-00 with
// no padding, and are not part of the MopHdr family.

#ifndef DECNET_MOP_PACKETS_H
#define DECNET_MOP_PACKETS_H

#include "decnet/packet/group.h"
#include "decnet/packet/indexed.h"

#include <map>
#include <optional>
#include <string>

namespace decnet::mop {

using namespace decnet::packet;

// The multicast addresses MOP uses.  Console carrier and system ID go to
// the first, loopback to the second.
Macaddr console_multicast ();
Macaddr loop_multicast ();

// Message codes, the first byte of anything on 60-01.
enum Code : std::uint8_t {
    REQUEST_ID       = 5,
    SYSTEM_ID        = 7,
    REQUEST_COUNTERS = 9,
    COUNTERS         = 11,
    CONSOLE_REQUEST  = 13,
    CONSOLE_RELEASE  = 15,
    CONSOLE_COMMAND  = 17,
    CONSOLE_RESPONSE = 19
};

// The software identification field of a system ID message.  It is either
// a counted string or a single signed byte in the range -2 to 0, which
// stand for "no software id", "maintenance system" and "operating system".
// Port of mop.C.
struct SoftwareId {
    bool         is_code = false;
    std::int8_t  code = 0;
    std::string  text;

    static SoftwareId named (std::string s)
    { SoftwareId i; i.text = std::move (s); return i; }
    static SoftwareId numbered (std::int8_t c)
    { SoftwareId i; i.is_code = true; i.code = c; return i; }

    std::string str () const;
    friend bool operator== (const SoftwareId &, const SoftwareId &) = default;
};

struct SoftwareIdField {
    using value_type = SoftwareId;
    static void encode (Encoder &e, const value_type &v);
    static void decode (Decoder &d, value_type &v);
};

void register_mop_packets ();

struct MopPacketBase : Indexed<MopPacketBase> {
    static std::uint64_t index_key (ByteView b) { return require_byte (b, 0); }
    DN_PACKET_INDEX_REGISTERED (MopPacketBase, 256, register_mop_packets)

    static std::unique_ptr<MopPacketBase> parse_frame (ByteView b) noexcept
    { return try_parse_indexed (b); }
};

// What a node says about itself.  The body is a TLV list, and tags we do
// not recognise are kept so that a message can be passed on or re-encoded
// unchanged.  Marked tolerant because real implementations send items that
// do not quite fit the spec.
struct SysId : IndexedBody<SysId, MopPacketBase> {
    static constexpr const char *name = "SysId";
    static constexpr std::uint8_t code_value = SYSTEM_ID;

    std::uint8_t  code = SYSTEM_ID;
    std::uint16_t receipt = 0;

    // Services this node offers.
    bool loop = false, dump = false, ploader = false, sloader = false;
    bool boot = false, carrier = false, counters = false;
    bool carrier_reserved = false;

    std::optional<Version>       version;
    std::optional<Bytes>         console_user;
    std::optional<std::uint16_t> reservation_timer;
    std::optional<std::uint16_t> console_cmd_size;
    std::optional<std::uint16_t> console_resp_size;
    std::optional<Bytes>         hwaddr;
    std::optional<std::uint8_t>  device;
    std::optional<SoftwareId>    software;
    std::optional<std::uint8_t>  processor;
    std::optional<std::uint8_t>  datalink;
    std::optional<std::uint16_t> bufsize;

    // Items we do not know about, kept so a round trip is faithful.  Tag 8
    // is the system time, which nothing here needs but other nodes send.
    std::map<unsigned, Bytes> unknown;

    static constexpr auto layout = fields (
        field<B<1>> (&SysId::code, "code"),
        reserved<RES<1>, SysId> (),
        field<B<2>> (&SysId::receipt, "receipt"),
        tlv_wild<SysId, 2, 1, Tolerant::yes> (
            &SysId::unknown,
            tlvf<1, VersionField> (&SysId::version, "version"),
            tlvg<2> (bm<SysId> (
                         bmf (&SysId::loop,     "loop",     0, 1),
                         bmf (&SysId::dump,     "dump",     1, 1),
                         bmf (&SysId::ploader,  "ploader",  2, 1),
                         bmf (&SysId::sloader,  "sloader",  3, 1),
                         bmf (&SysId::boot,     "boot",     4, 1),
                         bmf (&SysId::carrier,  "carrier",  5, 1),
                         bmf (&SysId::counters, "counters", 6, 1),
                         bmf (&SysId::carrier_reserved, "carrier_reserved",
                              7, 1)),
                     "services"),
            tlvf<3, BV<6>>       (&SysId::console_user, "console_user"),
            tlvf<4, B<2>>        (&SysId::reservation_timer, "reservation_timer"),
            tlvf<5, B<2>>        (&SysId::console_cmd_size, "console_cmd_size"),
            tlvf<6, B<2>>        (&SysId::console_resp_size, "console_resp_size"),
            tlvf<7, BV<6>>       (&SysId::hwaddr, "hwaddr"),
            tlvf<100, B<1>>      (&SysId::device, "device"),
            tlvf<200, SoftwareIdField> (&SysId::software, "software"),
            tlvf<300, B<1>>      (&SysId::processor, "processor"),
            tlvf<400, B<1>>      (&SysId::datalink, "datalink"),
            tlvf<401, B<2>>      (&SysId::bufsize, "bufsize")));

    // The services set, as names, for logging and monitoring.
    std::vector<std::string> services () const;
};

// "Tell me who you are."  The receipt number comes back in the answer, so
// a requester can match them up.
struct RequestId : IndexedBody<RequestId, MopPacketBase> {
    static constexpr const char *name = "RequestId";
    std::uint8_t  code = REQUEST_ID;
    std::uint16_t receipt = 0;

    static constexpr auto layout = fields (
        field<B<1>> (&RequestId::code, "code"),
        reserved<RES<1>, RequestId> (),
        field<B<2>> (&RequestId::receipt, "receipt"));
};

struct RequestCounters : IndexedBody<RequestCounters, MopPacketBase> {
    static constexpr const char *name = "RequestCounters";
    std::uint8_t  code = REQUEST_COUNTERS;
    std::uint16_t receipt = 0;

    static constexpr auto layout = fields (
        field<B<1>> (&RequestCounters::code, "code"),
        field<B<2>> (&RequestCounters::receipt, "receipt"));
};

// Ethernet counters.  Most of the error counts mean nothing to a software
// implementation, but they are defined so that counters from a real
// controller can be parsed and reported.
struct Counters : IndexedBody<Counters, MopPacketBase> {
    static constexpr const char *name = "Counters";

    std::uint8_t  code = COUNTERS;
    std::uint16_t receipt = 0;
    std::uint16_t time_since_zeroed = 0;
    std::uint32_t bytes_recv = 0, bytes_sent = 0;
    std::uint32_t pkts_recv = 0, pkts_sent = 0;
    std::uint32_t mcbytes_recv = 0, mcpkts_recv = 0;
    std::uint32_t pkts_deferred = 0;
    std::uint32_t pkts_1_collision = 0, pkts_mult_collision = 0;
    std::uint16_t send_fail = 0, send_reasons = 0;
    std::uint16_t recv_fail = 0, recv_reasons = 0;
    std::uint16_t unk_dest = 0, data_overrun = 0;
    std::uint16_t no_sys_buf = 0, no_user_buf = 0;

    static constexpr auto layout = fields (
        field<B<1>> (&Counters::code, "code"),
        field<B<2>> (&Counters::receipt, "receipt"),
        field<B<2>> (&Counters::time_since_zeroed, "time_since_zeroed"),
        field<B<4>> (&Counters::bytes_recv, "bytes_recv"),
        field<B<4>> (&Counters::bytes_sent, "bytes_sent"),
        field<B<4>> (&Counters::pkts_recv, "pkts_recv"),
        field<B<4>> (&Counters::pkts_sent, "pkts_sent"),
        field<B<4>> (&Counters::mcbytes_recv, "mcbytes_recv"),
        field<B<4>> (&Counters::mcpkts_recv, "mcpkts_recv"),
        field<B<4>> (&Counters::pkts_deferred, "pkts_deferred"),
        field<B<4>> (&Counters::pkts_1_collision, "pkts_1_collision"),
        field<B<4>> (&Counters::pkts_mult_collision, "pkts_mult_collision"),
        field<B<2>> (&Counters::send_fail, "send_fail"),
        field<B<2>> (&Counters::send_reasons, "send_reasons"),
        field<B<2>> (&Counters::recv_fail, "recv_fail"),
        field<B<2>> (&Counters::recv_reasons, "recv_reasons"),
        field<B<2>> (&Counters::unk_dest, "unk_dest"),
        field<B<2>> (&Counters::data_overrun, "data_overrun"),
        field<B<2>> (&Counters::no_sys_buf, "no_sys_buf"),
        field<B<2>> (&Counters::no_user_buf, "no_user_buf"));
};

// PORT: the console carrier messages are defined so that they parse, but
// nothing implements the console carrier itself yet.  See NOTDONE.md.
struct ConsoleRequest : IndexedBody<ConsoleRequest, MopPacketBase> {
    static constexpr const char *name = "ConsoleRequest";
    std::uint8_t code = CONSOLE_REQUEST;
    Bytes        verification;

    static constexpr auto layout = fields (
        field<B<1>>  (&ConsoleRequest::code, "code"),
        field<BV<8>> (&ConsoleRequest::verification, "verification"));
};

struct ConsoleRelease : IndexedBody<ConsoleRelease, MopPacketBase> {
    static constexpr const char *name = "ConsoleRelease";
    std::uint8_t code = CONSOLE_RELEASE;

    static constexpr auto layout = fields (
        field<B<1>> (&ConsoleRelease::code, "code"));
};

struct ConsoleCommand : IndexedBody<ConsoleCommand, MopPacketBase,
                                    Extra::allow> {
    static constexpr const char *name = "ConsoleCommand";
    std::uint8_t code = CONSOLE_COMMAND;
    bool         seq = false, brk = false;
    Bytes        payload;

    static constexpr auto layout = fields (
        field<B<1>> (&ConsoleCommand::code, "code"),
        bm<ConsoleCommand> (bmf (&ConsoleCommand::seq, "seq",   0, 1),
                            bmf (&ConsoleCommand::brk, "break", 1, 1)),
        field<Payload> (&ConsoleCommand::payload, "payload"));
};

struct ConsoleResponse : IndexedBody<ConsoleResponse, MopPacketBase,
                                     Extra::allow> {
    static constexpr const char *name = "ConsoleResponse";
    std::uint8_t code = CONSOLE_RESPONSE;
    bool         seq = false, cmd_lost = false, resp_lost = false;
    Bytes        payload;

    static constexpr auto layout = fields (
        field<B<1>> (&ConsoleResponse::code, "code"),
        bm<ConsoleResponse> (
            bmf (&ConsoleResponse::seq,       "seq",       0, 1),
            bmf (&ConsoleResponse::cmd_lost,  "cmd_lost",  1, 1),
            bmf (&ConsoleResponse::resp_lost, "resp_lost", 2, 1)),
        field<Payload> (&ConsoleResponse::payload, "payload"));
};

// ------------------------------------------------------------- loopback
//
// Loopback messages are a different shape: a skip count saying how far
// into the message the next function is, then a sequence of functions.
// A station that is asked to forward bumps the skip count and passes the
// message on, so the reply retraces the path.

struct LoopSkip : Packet<LoopSkip, Extra::allow> {
    std::uint16_t skip = 0;
    Bytes         payload;

    static constexpr auto layout = fields (
        field<B<2>>    (&LoopSkip::skip, "skip"),
        field<Payload> (&LoopSkip::payload, "payload"));
};

struct LoopFwd : Packet<LoopFwd, Extra::allow> {
    static constexpr std::uint16_t function_code = 2;
    std::uint16_t function = function_code;
    Bytes         dest;
    Bytes         payload;

    static constexpr auto layout = fields (
        field<B<2>>    (&LoopFwd::function, "function"),
        field<BV<6>>   (&LoopFwd::dest, "dest"),
        field<Payload> (&LoopFwd::payload, "payload"));
};

struct LoopReply : Packet<LoopReply, Extra::allow> {
    static constexpr std::uint16_t function_code = 1;
    std::uint16_t function = function_code;
    std::uint16_t receipt = 0;
    Bytes         payload;

    static constexpr auto layout = fields (
        field<B<2>>    (&LoopReply::function, "function"),
        field<B<2>>    (&LoopReply::receipt, "receipt"),
        field<Payload> (&LoopReply::payload, "payload"));
};

}   // namespace decnet::mop

#endif  // DECNET_MOP_PACKETS_H
