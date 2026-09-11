// decnet/events/events.h -- DECnet event records.
//
// Port of events.py.  An event is a numbered thing that happened: class 4
// code 7 is "circuit down, circuit fault".  It carries a timestamp, the
// node it happened on, the entity it happened to, and a NICE parameter list
// saying more.  The same record is what goes on the console, into a log
// file, and down a logical link to a remote sink, so there is one encoding
// and three ways of presenting it.
//
// the Python writes one Python class per event, several hundred of them, and
// leans on the class hierarchy for the shared parameter definitions.  That
// does not port: a class per event in C++ buys nothing, because none of
// them add behaviour, only a number, a sentence and a parameter table.  So
// there is one Event type and a table of definitions beside it.
//
// The timestamp is the awkward part.  It counts half-days and seconds from
// 1 January 1977 in local wall clock time with no daylight saving rule
// applied, which is what the other implementations show, so it cannot be
// derived from a UTC clock reading alone.

#ifndef DECNET_EVENTS_EVENTS_H
#define DECNET_EVENTS_EVENTS_H

#include "decnet/common/logging.h"
#include "decnet/nice/entity.h"
#include "decnet/nice/params.h"

#include <cstdint>
#include <string>

namespace decnet::events {

using nice::Entity;
using nice::NiceNode;
using nice::ParamDefs;
using nice::ParamList;
using nice::Value;
using packet::Decoder;
using packet::Encoder;

// An event class and code together.  Two 32 entry code spaces per class,
// 512 classes; classes 0 to 30 are architectural, the rest belong to
// particular implementations.
struct EventId {
    std::uint16_t cls  = 0;
    std::uint8_t  code = 0;

    friend constexpr bool operator== (EventId, EventId) noexcept = default;
    friend constexpr auto operator<=> (EventId a, EventId b) noexcept
    { return a.cls != b.cls ? a.cls <=> b.cls : a.code <=> b.code; }

    std::string str () const
    { return std::to_string (cls) + "." + std::to_string (code); }
};

// What we know about an event we can name.
struct EventDef {
    EventId          id;
    const char      *text;    // "Circuit down, circuit fault"
    logging::Level   level;
    ParamDefs        params;
};

// The definition for an event, or null if this build has never heard of it.
const EventDef *find_event (EventId id);

// Every event this implementation can name.  A filter can only select from
// these, because filtering applies to locally generated events and there is
// no point enabling one we never raise.
std::span<const EventDef> known_events ();

// The values the "Reason" parameter of a class 4 (routing) event takes.
// The numbers are the ones the display table above is indexed by, so a
// call site that picks the right one gets the right sentence for free.
namespace reason {
inline constexpr std::uint64_t
    sync_lost = 0, data_errors = 1, unexpected_packet_type = 2,
    checksum_error = 3, address_change = 4, verification_timeout = 5,
    version_skew = 6, address_out_of_range = 7, block_size_too_small = 8,
    invalid_verification = 9, listener_timeout = 10,
    listener_invalid_data = 11, call_failed = 12,
    verification_required = 13, dropped = 14;
}

// The "Status" parameter, which says whether something became reachable.
namespace status {
inline constexpr std::uint64_t reachable = 0, unreachable = 1;
}

// The parameter numbers the routing events use.  Named because a bare 8 at
// a call site says nothing.
namespace param {
inline constexpr std::uint16_t
    packet_header = 0, packet_beginning = 1, highest_address = 2, node = 3,
    expected_node = 4, reason = 5, received_version = 6, status = 7,
    adjacent_node = 8;
}

// A node as a NICE parameter value: the address, and the name beside it
// when we know one.  Several events name a node this way, and the display
// form ("2.5 (ARK)") comes out of the two element list rather than out of
// any special case in the formatter.
Value node_value (const NiceNode &n);

class Event {
public:
    Event () = default;
    Event (EventId id, Entity entity);

    // ------------------------------------------------------------ header
    EventId id { };
    // Always 1: "event log".  The field exists because the record format
    // shares its first byte with the rest of the logging protocol, which
    // has other functions we do not implement.
    std::uint8_t function = 1;
    // Which sinks the originating node asked for.  A record arriving from
    // another node carries its wishes here, and a local sink honours them.
    bool console = true, file = true, monitor = true;
    NiceNode source;
    Entity   entity;
    ParamList params;

    // Half-days and seconds since 1 January 1977, local time, plus
    // milliseconds.  ms_absent says the sender had no better than one
    // second resolution.
    std::uint16_t halfday = 0;
    std::uint16_t seconds = 0;
    std::uint16_t milliseconds = 0;
    bool          ms_absent = false;

    // Set the timestamp fields from the system clock.
    void stamp_now ();

    // ------------------------------------------------------- convenience
    // Add a parameter.  The overloads cover what the call sites need; the
    // Value form is there for anything else.
    Event &param (std::uint16_t n, Value v);
    Event &coded (std::uint16_t n, std::uint64_t v, unsigned bytes = 1);
    Event &number (std::uint16_t n, std::uint64_t v, unsigned bytes = 1);
    Event &text (std::uint16_t n, std::string s);
    Event &image (std::uint16_t n, Bytes b);
    Event &counter (std::uint16_t n, std::uint64_t v, unsigned bytes = 2);

    // ---------------------------------------------------------- encoding
    void  encode (Encoder &e) const;
    Bytes encode () const;
    static Event decode (Decoder &d);
    static Event parse (ByteView buf);

    // ---------------------------------------------------------- display
    // The severity this event is logged at, from the definition table.
    logging::Level level () const;

    // "01-Jan-1977 00:00:00.250"
    std::string timestamp () const;

    // The multi-line form the console sink writes:
    //
    //   Event type 4.7, Circuit down, circuit fault
    //   From node 1.3 (GROK), occurred 01-Jan-1977 00:00:00
    //       Circuit = DMC-0, Packet header = 42 16 3 23, Reason = ...
    std::string str () const;
};

}   // namespace decnet::events

#endif  // DECNET_EVENTS_EVENTS_H
