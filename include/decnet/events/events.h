// decnet/events/events.h -- DECnet event records.
//
// Port of events.py.  An event has a class and code (4.7 is "circuit down,
// circuit fault"), a timestamp, the source node, an entity and a NICE
// parameter list.  The same encoding is used for console, file and remote
// sinks.
//
// PyDECnet has a class per event.  Here there is one Event type and a
// table of definitions.
//
// Timestamps are half-days and seconds since 1 January 1977, local time
// without daylight saving adjustment.

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

// Event class and code.  32 codes per class, 512 classes; classes 0 to 30
// are architectural.
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

// All events this implementation can raise.  Filters select from these.
std::span<const EventDef> known_events ();

// Values of the "Reason" parameter in class 4 (routing) events, matching
// the display table.
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

// A node as a NICE parameter value: the address and, if known, the name.
// Displays as "2.5 (ARK)".
Value node_value (const NiceNode &n);

class Event {
public:
    Event () = default;
    Event (EventId id, Entity entity);

    // ------------------------------------------------------------ header
    EventId id { };
    // Always 1 (event log).
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
    // Add a parameter.
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
