// decnet/common/types.h -- the small value types shared by every layer.
//
// Port of the corresponding classes in the Python's decnet/common.py.  In
// Python these all subclass Field so they can appear directly in a packet
// layout; here they instead satisfy the Codec concept in
// decnet/packet/field.h, which the layout machinery picks up by ADL-free
// static dispatch.

#ifndef DECNET_COMMON_TYPES_H
#define DECNET_COMMON_TYPES_H

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace decnet {

// A packet under construction or being parsed.  the Python passes bytes and
// memoryview objects around; we use a vector for owned data and a span for
// borrowed slices, which keeps the decoders allocation free.
using Bytes     = std::vector<std::uint8_t>;
using ByteView  = std::span<const std::uint8_t>;

// Space separated hex, newline every bytes_per_line octets.  Trace logging.
std::string hexdump (ByteView b, std::size_t bytes_per_line = 16);

// ---------------------------------------------------------------- Nodeid
//
// A Phase IV node address: 6 bits of area, 10 bits of node number, held as
// the 16 bit value the wire format uses.  Phase II and III addresses have
// area 0.  Mirrors common.Nodeid.
class Nodeid {
public:
    constexpr Nodeid () noexcept = default;
    constexpr explicit Nodeid (std::uint16_t v) noexcept : value_ (v) {}
    constexpr Nodeid (unsigned area, unsigned tid) noexcept
        : value_ (static_cast<std::uint16_t> ((area << 10) | tid)) {}

    // Parse "area.node" or a bare node number.  Throws std::invalid_argument
    // on anything else, which is what config parsing wants.
    static Nodeid parse (std::string_view s);

    constexpr unsigned area () const noexcept { return value_ >> 10; }
    constexpr unsigned tid  () const noexcept { return value_ & 0x3ff; }
    constexpr std::uint16_t value () const noexcept { return value_; }
    constexpr explicit operator bool () const noexcept { return value_ != 0; }

    // True for an address that a Phase III node can carry (area 0).
    constexpr bool is_phase3 () const noexcept { return area () == 0; }

    std::string str () const;

    friend constexpr bool operator== (Nodeid, Nodeid) noexcept = default;
    friend constexpr auto operator<=> (Nodeid a, Nodeid b) noexcept
    { return a.value_ <=> b.value_; }

private:
    std::uint16_t value_ = 0;
};

// --------------------------------------------------------------- Macaddr
//
// An Ethernet address.  Mirrors common.Macaddr.
class Macaddr {
public:
    constexpr Macaddr () noexcept = default;
    constexpr explicit Macaddr (const std::array<std::uint8_t, 6> &b) noexcept
        : bytes_ (b) {}

    // "aa-bb-cc-dd-ee-ff" or "aa:bb:cc:dd:ee:ff".
    static Macaddr parse (std::string_view s);

    // The DECnet Phase IV mapping: AA-00-04-00-<tid low>-<tid high|area>.
    static Macaddr from_nodeid (Nodeid n);

    const std::array<std::uint8_t, 6> &bytes () const noexcept { return bytes_; }
    ByteView view () const noexcept { return ByteView (bytes_.data (), 6); }

    bool is_multicast () const noexcept { return (bytes_[0] & 1) != 0; }
    bool is_local     () const noexcept { return (bytes_[0] & 2) != 0; }

    std::string str () const;

    friend bool operator== (const Macaddr &, const Macaddr &) noexcept = default;

private:
    std::array<std::uint8_t, 6> bytes_ {};
};

// --------------------------------------------------------------- Version
//
// A three part DECnet version number as it appears in routing and NSP
// initialisation messages.  Mirrors common.Version.
struct Version {
    std::uint8_t v1 = 0, v2 = 0, v3 = 0;

    static Version parse (std::string_view s);   // "4.0.0"
    std::string str () const;

    friend bool operator== (Version, Version) noexcept = default;
};

// The version numbers the Python reports; kept here so every layer agrees.
inline constexpr Version tiver_ph2 { 3, 1, 0 };
inline constexpr Version tiver_ph3 { 1, 3, 0 };
inline constexpr Version tiver_ph4 { 2, 0, 0 };
inline constexpr Version nspver_ph2 { 3, 1, 0 };
inline constexpr Version nspver_ph3 { 3, 1, 0 };
inline constexpr Version nspver_ph4 { 4, 1, 0 };

// ------------------------------------------------------------------ misc

// Validate and canonicalise a node name: alphanumeric, at least one letter,
// at most six characters, upper cased.  Port of common.nodename.  A real
// the Python node rejects a longer name at config read-in, so accepting one
// here would only defer the failure to the far end.
std::string nodename (std::string_view s);

// The same for a circuit name: letters, then letters, digits or hyphens.
// Port of common.circname.
std::string circname (std::string_view s);

enum class Phase : std::uint8_t { ph2 = 2, ph3 = 3, ph4 = 4 };

// Node type, matching the "type" values accepted by the routing config line.
enum class NodeType {
    l2router, l1router, endnode, phase3router, phase3endnode, phase2
};

Phase phase_of (NodeType t) noexcept;
const char *name_of (NodeType t) noexcept;

}   // namespace decnet

#endif  // DECNET_COMMON_TYPES_H
