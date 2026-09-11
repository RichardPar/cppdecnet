// decnet/nice/value.h -- NICE data value encoding.
//
// Port of the data type classes in nice_coding.py (DU, DS, H, O, AI, HI, C
// and CM).  A NICE data value carries its own type code, so unlike every
// other field in DECnet the reader learns the type from the data rather
// than from the layout.  pydecnet models that with a class per type code
// and an index that maps a code to its class, generating classes on the fly
// for byte counts it has not seen.
//
// A tagged value is the better fit in C++: one type, a code, and a variant
// payload.  The "generate a class per length" machinery then disappears
// entirely, because the length is just a field.
//
// The type code byte:
//
//     0x00 + n   DU-n   unsigned decimal, n bytes
//     0x10 + n   DS-n   signed decimal, n bytes
//     0x20 + n   H-n    hexadecimal, n bytes      (0x20 alone: HI, an image)
//     0x30 + n   O-n    octal, n bytes
//     0x40       AI     ASCII image, one byte count then that many bytes
//     0x80 + n   C-n    coded, n bytes, formatted through a label list
//     0xc0 + n   CM-n   coded multiple: n values, each with its own code
//
// PORT: the counter types (CTR/CTM, which use a two byte code combining the
// counter kind with the parameter number) and the NICE parameter group that
// carries them belong with nicepackets in phase 6, where their only callers
// are.  This header is the value encoding those will build on.

#ifndef DECNET_NICE_VALUE_H
#define DECNET_NICE_VALUE_H

#include "decnet/packet/buffer.h"

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace decnet::nice {

using packet::Decoder;
using packet::Encoder;

// Labels for a coded (C) value: labels[v] is the text for value v.  An
// index past the end formats as "#v", as C1.format does.
using Labels = std::span<const char *const>;

class Value {
public:
    enum class Kind : std::uint8_t { du, ds, h, o, ai, hi, c, cm };

    using List = std::vector<Value>;

    Value () = default;

    // ------------------------------------------------------- construction
    static Value du (std::uint64_t v, unsigned bytes = 1);
    static Value ds (std::int64_t v, unsigned bytes = 1);
    static Value h  (std::uint64_t v, unsigned bytes = 1);
    static Value o  (std::uint64_t v, unsigned bytes = 1);
    static Value c  (std::uint64_t v, unsigned bytes = 1);
    static Value ai (std::string s);
    static Value hi (Bytes b);
    static Value cm (List items);

    // ------------------------------------------------------------ queries
    Kind     kind      () const noexcept { return kind_; }
    unsigned byte_count() const noexcept { return bytes_; }
    std::uint8_t type_code () const noexcept;

    bool is_number () const noexcept
    { return kind_ == Kind::du || kind_ == Kind::ds || kind_ == Kind::h
          || kind_ == Kind::o  || kind_ == Kind::c; }

    // Accessors.  Each throws std::bad_variant_access on the wrong kind,
    // so a caller that has checked the kind pays nothing.
    std::uint64_t     as_uint () const;
    std::int64_t      as_int  () const;
    const std::string &as_string () const;
    const Bytes       &as_bytes  () const;
    const List        &as_list   () const;

    // ----------------------------------------------------------- encoding
    // Both include the type code, as the Python encode methods do.
    void  encode (Encoder &e) const;
    Bytes encode () const;

    // Read a type code and the value that follows it.
    static Value decode (Decoder &d);

    // The same, for a caller that already has the type code.  A NICE
    // *request* omits the code byte -- the reader is expected to know what
    // each parameter means -- so the request decoder looks the code up in a
    // table and calls this.  See nice/packets.h.
    static Value decode_body (std::uint8_t code, Decoder &d);
    static Value parse (ByteView buf);

    // ---------------------------------------------------------- formatting
    // The display form NICE uses.  labels applies to a coded value, and to
    // nothing else; for CM it is ignored, since each element carries its
    // own type.
    std::string format (Labels labels = {}) const;

    friend bool operator== (const Value &, const Value &);

private:
    using Payload = std::variant<std::monostate, std::uint64_t, std::int64_t,
                                 std::string, Bytes, List>;

    Kind     kind_  = Kind::du;
    unsigned bytes_ = 1;
    Payload  data_ {};
};

// The delimiter CM values are joined with when formatted.
inline constexpr const char *cm_delimiter = " ";

}   // namespace decnet::nice

#endif  // DECNET_NICE_VALUE_H
