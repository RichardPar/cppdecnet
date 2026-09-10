// decnet/nice/params.h -- a NICE parameter list.
//
// Port of the NICE field group in nice_coding.py.  A NICE message ends with
// a run of parameters, each one a two byte number followed by a value.  Two
// kinds share that run:
//
//   a parameter, whose value carries its own type code (see value.h), and
//   a counter, whose number has bit 15 set and whose type lives in the top
//   nibble of the number rather than in a code byte of its own.
//
// The two are numbered independently, so parameter 1000 and counter 1000
// are different things and can both be present.  A mapped counter (CTM)
// adds a two byte bitmap of qualifiers before the value, saying which
// specific errors contributed to the count.
//
// pydecnet drives decoding from a per-message table of parameter
// definitions.  In a response -- which is what an event record is -- it
// does not have to: every value says what type it is.  So decoding here
// needs no table at all, and the definitions are used only for display, to
// turn parameter 5 into "Reason" and value 11 into "Adjacency listener
// received invalid data".  That is also why a message with parameters we
// have never heard of survives a decode and re-encode unchanged.

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

    // The largest value the field can hold.  A counter that reaches it
    // stops there and displays as ">254" and so on, because the reader
    // cannot tell how much was lost.
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

// How a parameter is displayed when the type code alone does not say.  A
// node address is a two byte number on the wire and "1.2" on the screen; a
// version is three numbers joined by dots rather than by spaces.  pydecnet
// expresses this by subclassing the data type (DUNode, CMNode, CMVersion)
// and overriding format; here it is a property of the parameter, which is
// the only place that knows.
enum class Style : std::uint8_t { plain, node, version };

// What a parameter means.  labels are the display strings for a coded (C)
// value indexed by the value, or the qualifier names of a mapped counter
// indexed by bit number.
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

    // Iteration is in the order pydecnet formats in: by number, with the
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
    // wire encodes it and the order pydecnet displays in.
    std::map<std::uint16_t, Param> params_;
};

}   // namespace decnet::nice

#endif  // DECNET_NICE_PARAMS_H
