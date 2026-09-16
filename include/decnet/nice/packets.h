// decnet/nice/packets.h -- NICE protocol messages.
//
// Port of nicepackets.py.  NCP connects to object 19 and exchanges request
// and reply messages.  Value, entity and parameter coding are in value.h,
// entity.h and params.h.
//
// Reply parameters carry their own type code, so replies decode without a
// table and unknown parameters survive a round trip.  Request parameters
// omit the type code, so requests decode against a table and the request
// class has named fields.
//
// Entities in replies have no kind byte; entities in event records do.
// See Entity::encode_body and decode_body.
//
// PORT: Phase II NICE (the P2* classes in nicepackets.py) is not
// implemented.  See NOTDONE.md.

#ifndef DECNET_NICE_PACKETS_H
#define DECNET_NICE_PACKETS_H

#include "decnet/nice/entity.h"
#include "decnet/nice/params.h"

#include <cstdint>
#include <string>

namespace decnet::nice {

// ------------------------------------------------------------- constants

// Function codes, the first byte of a request.
inline constexpr std::uint8_t fn_test = 18;
inline constexpr std::uint8_t fn_set  = 19;
inline constexpr std::uint8_t fn_read = 20;
inline constexpr std::uint8_t fn_zero = 21;

// What a read request asks for.
enum InfoType : unsigned {
    info_summary = 0, info_status = 1, info_char = 2,
    info_counters = 3, info_events = 4
};

// Test (loop) types.
inline constexpr unsigned test_node = 0;
inline constexpr unsigned test_line = 1;
inline constexpr unsigned test_circuit = 3;

// Reply return codes.  The negative ones are errors; these four are not.
inline constexpr int rc_success  = 1;      // this reply is the answer
inline constexpr int rc_multiple = 2;      // more replies follow
inline constexpr int rc_more     = 3;      // more for this same entity
inline constexpr int rc_done     = -128;   // end of a multiple reply

// The errors this implementation returns.  The full list is in the
// Network Management specification and in retcode_text below.
inline constexpr int rc_unrecognized_function = -1;
inline constexpr int rc_invalid_format        = -2;
inline constexpr int rc_privilege_violation   = -3;
inline constexpr int rc_unrecognized_component = -8;
inline constexpr int rc_invalid_parameter     = -16;
inline constexpr int rc_mirror_disconnected   = -19;
inline constexpr int rc_mirror_connect_failed = -21;
inline constexpr int rc_operation_failure     = -25;
inline constexpr int rc_bad_loopback          = -28;

// The message for a return code, or null when the code is not an error.
const char *retcode_text (int code);

// The message for the detail field, when the return code gives it a
// meaning.  Empty when it does not, in which case NCP prints the number.
std::string detail_text (int retcode, unsigned detail);

// NICE node type codes.  These are not the routing layer's type codes.
inline constexpr unsigned nice_routing3 = 0;
inline constexpr unsigned nice_endnode3 = 1;
inline constexpr unsigned nice_phase2   = 2;
inline constexpr unsigned nice_area     = 3;
inline constexpr unsigned nice_routing4 = 4;
inline constexpr unsigned nice_endnode4 = 5;

// ------------------------------------------------------------- ReqEntity

// The entity a request is about.  A signed byte selects the form:
//
//     > 0    a name of that many characters
//     = 0    a node address or area number
//     < 0    a wildcard ("known circuits", "active nodes", ...)
//
// Node wildcards -6 and -7 ("all nodes in area", "node number in any
// area") also carry a number.
struct ReqEntity {
    // Wildcard codes.
    static constexpr std::int8_t known       = -1;
    static constexpr std::int8_t active      = -2;
    static constexpr std::int8_t loop        = -3;
    static constexpr std::int8_t adjacent    = -4;
    static constexpr std::int8_t significant = -5;
    static constexpr std::int8_t area_wild   = -6;
    static constexpr std::int8_t tid_wild    = -7;

    std::uint8_t etype = Entity::node;
    std::int8_t  code  = 0;
    std::string  name;              // code > 0
    Nodeid       id;                // node entity, code 0, -6 or -7
    unsigned     area = 0;          // area entity, code 0

    ReqEntity () = default;
    ReqEntity (std::uint8_t t, std::int8_t c) : etype (t), code (c) {}

    static ReqEntity make_node (Nodeid n);
    static ReqEntity make_named (std::uint8_t etype, std::string n);
    static ReqEntity make_area (unsigned a);
    static ReqEntity make_wild (std::uint8_t etype, std::int8_t code);

    // The predicates NiceReadInfoHdr defines, which decide how much of the
    // database a request covers.
    bool mult () const noexcept { return code < 0; }
    bool one  () const noexcept { return code >= 0; }
    bool is_known () const noexcept
    { return code == known || code == area_wild || code == tid_wild; }
    bool is_active () const noexcept { return code == active; }
    bool is_loop () const noexcept { return code == loop; }
    bool is_adjacent () const noexcept { return code == adjacent; }
    bool is_significant () const noexcept { return code == significant; }
    bool sigact () const noexcept { return code == active || code == significant; }
    bool wild () const noexcept { return code < significant; }

    // Does this request cover the node?  Plural requests other than -6 and -7
    // match everything; the layer decides which nodes are active or adjacent.
    // Port of NodeReqEntity.match.
    bool match (Nodeid n) const noexcept;

    // The same for a named entity: a specific name matches only itself.
    bool match (const std::string &n) const noexcept;

    // "Node 1.3", "Known circuits", "executor".
    std::string str () const;

    void encode (Encoder &e) const;

    // The type code is not on the wire: it comes from the request's own
    // header, so the caller supplies it.
    static ReqEntity decode (Decoder &d, std::uint8_t etype);
};

// ----------------------------------------------------------- NiceRequest

// A decoded request.  One class for all functions, since they share most
// fields.
class NiceRequest {
public:
    std::uint8_t function = 0;

    // Read, set and zero all start with a flag byte holding the entity
    // type; read adds what information is wanted, zero adds "read as well".
    bool         permanent = false;    // read from the permanent database
    bool         readzero  = false;    // zero counters: read them first
    unsigned     info = info_summary;
    std::uint8_t entity_type = Entity::node;
    ReqEntity    entity;

    // Read qualifiers: node reads by circuit (parameter 501, or 822 from VMS),
    // circuit reads by adjacent node (parameter 800).
    bool         has_qual_circuit = false;
    std::string  qual_circuit;
    bool         has_qual_node = false;
    ReqEntity    qual_node;

    // Test (loop).
    unsigned     test_type = test_node;
    bool         access_ctl = false;
    std::string  username, password, account;
    unsigned     loop_count = 1;
    unsigned     loop_length = 128;
    unsigned     loop_with = 2;        // 0 zeroes, 1 ones, 2 mixed
    Bytes        physical_address;     // loop circuit: where to send

    // What kind of information a read asks for.
    bool sum () const noexcept { return info == info_summary; }
    bool stat () const noexcept { return info == info_status; }
    bool sumstat () const noexcept { return info < info_char; }
    bool chars () const noexcept { return info == info_char; }
    bool counters () const noexcept { return info == info_counters; }
    bool events () const noexcept { return info == info_events; }

    Bytes encode () const;

    // Decode a request.  Throws DecodeError on anything malformed, which
    // the listener answers with "invalid message format".
    static NiceRequest parse (ByteView buf);

    // A one line description, for the trace log.
    std::string str () const;
};

// ------------------------------------------------------------- NiceReply

// A reply message: return code, detail and message, plus entity and
// parameters for a read reply.
class NiceReply {
public:
    int           retcode = rc_success;
    std::uint16_t detail = 0xffff;
    std::string   message;

    bool          has_entity = false;
    Entity        entity;
    ParamList     params;

    // A loop error reply carries the number of messages not looped instead
    // of an entity.  It is the one reply whose body is neither.
    bool          has_notlooped = false;
    std::uint16_t notlooped = 0;

    NiceReply () = default;
    explicit NiceReply (int rc) : retcode (rc) {}

    static NiceReply error (int rc, unsigned det = 0xffff,
                            std::string msg = {});

    Bytes encode () const;

    // Decode just the header.  Detail and message are optional in error
    // replies and default when missing.  Port of NiceReplyHeader.decode.
    static NiceReply parse_header (ByteView buf);

    // Decode a full read reply, whose entity is of the given kind.
    static NiceReply parse (ByteView buf, std::uint8_t entity_kind);

    // Decode a reply to a loop request: header, then the count of messages not
    // looped instead of an entity.
    static NiceReply parse_loop (ByteView buf);

    // How NCP prints it: the entity line, then one line per parameter.
    std::string format (ParamDefs defs) const;
};

// -------------------------------------------------------- the definitions

// Parameter tables per entity kind, used for display names and values and
// by the reply builders.
ParamDefs node_params ();
ParamDefs circuit_params ();
ParamDefs line_params ();
ParamDefs area_params ();
ParamDefs module_params ();
ParamDefs logging_params ();

// The table for an entity kind, or an empty span for one with none.
ParamDefs params_for (std::uint8_t entity_kind);

// The node type labels, shared by parameters 810 and 901.
Labels node_type_labels ();

}   // namespace decnet::nice

#endif  // DECNET_NICE_PACKETS_H
