// decnet/nice/params.h -- NICE parameter lists.
//
// Port of the NICE field group in nice_coding.py.  A list of entries, each
// a two byte number and a value:
//
//   parameter: the value carries its own type code (see value.h)
//   counter:   bit 15 set, type in the top nibble of the number
//
// Parameters and counters are numbered independently.  A mapped counter
// (CTM) has a two byte qualifier bitmap before the value.
//
// Decoding needs no table because values are self describing.  The
// definitions are only used for display, and unknown parameters survive a
// round trip.

#ifndef DECNET_NICE_PARAMS_H
#define DECNET_NICE_PARAMS_H

#include "decnet/nice/value.h"

#include <map>
#include <string>
#include <vector>

namespace decnet::nice {

// A counter: a saturating unsigned integer of 1, 2 or 4 bytes, optionally
// with a bitmap saying which qualifiers were seen.
struct Counter {
    std::uint64_t value  = 0;
    unsigned      bytes  = 1;       // 1, 2 or 4
    bool          mapped = false;   // CTM rather than CTR
    std::uint16_t map    = 0;       // qualifier bits, when mapped

    // Maximum value.  A counter that reaches it stays there and displays as
    // ">254" etc.
    std::uint64_t max () const noexcept;
    std::string   format () const;
};

// One entry in the list.
struct Param {
    std::uint16_t number  = 0;
    bool          counter = false;
    Value         value;    // when !counter
    Counter       count;    // when counter

    friend bool operator== (const Param &, const Param &) = default;
};

// Display style when the type code is not enough: node addresses as "1.2",
// versions joined by dots, elapsed times as "107:43:04".  PyDECnet uses
// subclasses (DUNode, CMNode, CMVersion, CMEtime) for this.
enum class Style : std::uint8_t { plain, node, version, etime };

// Parameter definition.  labels are display strings for a coded (C) value
// indexed by value, or qualifier names for a mapped counter indexed by bit.
struct ParamDef {
    std::uint16_t number;
    bool          counter;
    const char   *desc;
    Labels        labels;
    Style         style = Style::plain;
};

// "1.2", or "42" for an address with no area.  The display form of a node
// number, wherever one appears.
std::string format_node_number (std::uint64_t v);

using ParamDefs = std::span<const ParamDef>;

const ParamDef *find_def (ParamDefs defs, std::uint16_t number, bool counter);

class ParamList {
public:
    void set (std::uint16_t number, Value v);
    void set_counter (std::uint16_t number, Counter c);
    void clear () { params_.clear (); }

    const Param *find (std::uint16_t number, bool counter = false) const;
    bool empty () const noexcept { return params_.empty (); }
    std::size_t size () const noexcept { return params_.size (); }

    // Iteration is in the order PyDECnet formats in: by number, with the
    // counters after the plain parameters because of their bit 15.
    auto begin () const noexcept { return params_.begin (); }
    auto end   () const noexcept { return params_.end (); }

    void encode (Encoder &e) const;

    // Read parameters until the buffer runs out.
    void decode (Decoder &d);

    // One string per parameter, in list order, ready to be laid out by the
    // caller.  A parameter with no definition is named by its number.
    std::vector<std::string> format (ParamDefs defs) const;

    friend bool operator== (const ParamList &, const ParamList &) = default;

private:
    // Keyed by number with bit 15 set for counters, which is both how the
    // wire encodes it and the order PyDECnet displays in.
    std::map<std::uint16_t, Param> params_;
};

}   // namespace decnet::nice

#endif  // DECNET_NICE_PARAMS_H
