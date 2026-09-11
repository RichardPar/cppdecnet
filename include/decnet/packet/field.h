// decnet/packet/field.h -- field codecs.
//
// Port of the field classes in packet.py.  A the Python layout row is
//
//     ( packet.B, "srcnode", 2 )
//
// which a metaclass resolves at class creation time.  Here the same thing
// resolves at compile time: field class -> template argument, attribute
// name -> pointer to member, arguments -> template parameters.
//
//     field<B<2>> (&Msg::srcnode, "srcnode")
//
// A codec is a stateless type providing
//     using value_type = ...;
//     static void encode (Encoder &, const value_type &);
//     static void decode (Decoder &, value_type &);

#ifndef DECNET_PACKET_FIELD_H
#define DECNET_PACKET_FIELD_H

#include "decnet/packet/buffer.h"

#include <string>
#include <tuple>
#include <type_traits>

namespace decnet::packet {

// ------------------------------------------------------------- B: integer
//
// An unsigned little endian integer in N bytes.  packet.B.
template <std::size_t N, typename T = std::uint64_t>
struct B {
    static_assert (N >= 1 && N <= 8, "B<N> supports 1 to 8 bytes");
    using value_type = std::conditional_t<
        std::is_same_v<T, std::uint64_t>,
        std::conditional_t<N == 1, std::uint8_t,
        std::conditional_t<N == 2, std::uint16_t,
        std::conditional_t<N <= 4, std::uint32_t, std::uint64_t>>>, T>;

    static void encode (Encoder &e, const value_type &v)
    {
        if constexpr (N < 8) {
            if (static_cast<std::uint64_t> (v) >= (1ull << (8 * N)))
                throw FieldOverflow ("value too large for B<"
                                     + std::to_string (N) + ">");
        }
        e.uint (static_cast<std::uint64_t> (v), N);
    }

    static void decode (Decoder &d, value_type &v)
    { v = static_cast<value_type> (d.uint (N)); }
};

// ------------------------------------------------- SIGNED: signed integer
//
// A two's complement signed integer in N bytes, little endian.
// packet.SIGNED.
template <std::size_t N>
struct SIGNED {
    static_assert (N >= 1 && N <= 8, "SIGNED<N> supports 1 to 8 bytes");
    using value_type = std::int64_t;

    static void encode (Encoder &e, const value_type &v)
    {
        if constexpr (N < 8) {
            std::int64_t lim = std::int64_t (1) << (8 * N - 1);
            if (v < -lim || v >= lim)
                throw FieldOverflow ("value out of range for SIGNED<"
                                     + std::to_string (N) + ">");
        }
        e.uint (static_cast<std::uint64_t> (v), N);
    }

    static void decode (Decoder &d, value_type &v)
    {
        std::uint64_t u = d.uint (N);
        if constexpr (N < 8) {
            std::uint64_t sign = std::uint64_t (1) << (8 * N - 1);
            if (u & sign) u |= ~((sign << 1) - 1);
        }
        v = static_cast<std::int64_t> (u);
    }
};

// ------------------------------------------------------- BV: fixed string
//
// Exactly N bytes, no length prefix.  packet.BV.
template <std::size_t N>
struct BV {
    using value_type = Bytes;

    static void encode (Encoder &e, const value_type &v)
    {
        if (v.size () > N)
            throw FieldOverflow ("value too long for BV<"
                                 + std::to_string (N) + ">");
        e.raw (ByteView (v.data (), v.size ()));
        e.zeros (N - v.size ());     // BV pads on the right, as the Python does
    }

    static void decode (Decoder &d, value_type &v)
    {
        ByteView b = d.raw (N);
        v.assign (b.begin (), b.end ());
    }
};

// ------------------------------------------------------------ I: image
//
// A one byte count followed by that many bytes, with a declared maximum.
// packet.I.
template <std::size_t Max>
struct I {
    static_assert (Max <= 255, "I<Max> count field is one byte");
    using value_type = Bytes;

    static void encode (Encoder &e, const value_type &v)
    {
        if (v.size () > Max)
            throw FieldOverflow ("image field longer than "
                                 + std::to_string (Max));
        e.byte (static_cast<std::uint8_t> (v.size ()));
        e.raw (ByteView (v.data (), v.size ()));
    }

    static void decode (Decoder &d, value_type &v)
    {
        std::size_t n = d.byte ();
        if (n > Max)
            throw FieldOverflow ("image field longer than "
                                 + std::to_string (Max));
        ByteView b = d.raw (n);
        v.assign (b.begin (), b.end ());
    }
};

// A: an image field carried as text rather than bytes.  packet.A.
template <std::size_t Max>
struct A {
    using value_type = std::string;

    static void encode (Encoder &e, const value_type &v)
    {
        if (v.size () > Max)
            throw FieldOverflow ("string field longer than "
                                 + std::to_string (Max));
        e.byte (static_cast<std::uint8_t> (v.size ()));
        e.raw (ByteView (reinterpret_cast<const std::uint8_t *> (v.data ()),
                         v.size ()));
    }

    static void decode (Decoder &d, value_type &v)
    {
        std::size_t n = d.byte ();
        if (n > Max)
            throw FieldOverflow ("string field longer than "
                                 + std::to_string (Max));
        ByteView b = d.raw (n);
        v.assign (reinterpret_cast<const char *> (b.data ()), b.size ());
    }
};

// ------------------------------------------------------- EX: extensible
//
// A variable length integer: each byte carries seven bits, the high bit
// says another byte follows.  packet.EX.
template <std::size_t MaxBytes = 4>
struct EX {
    using value_type = std::uint32_t;

    static void encode (Encoder &e, const value_type &v)
    {
        value_type x = v;
        std::size_t n = 0;
        do {
            std::uint8_t b = x & 0x7f;
            x >>= 7;
            if (x) b |= 0x80;
            e.byte (b);
            if (++n > MaxBytes)
                throw FieldOverflow ("extensible field too long");
        } while (x);
    }

    static void decode (Decoder &d, value_type &v)
    {
        v = 0;
        for (std::size_t n = 0; ; ++n) {
            if (n >= MaxBytes)
                throw FieldOverflow ("extensible field too long");
            std::uint8_t b = d.byte ();
            v |= static_cast<value_type> (b & 0x7f) << (7 * n);
            if (!(b & 0x80)) break;
        }
    }
};

// ----------------------------------------------------------- RES, Payload

// N reserved bytes: written as zero, skipped on input.  packet.RES.  It has
// no attribute, so it is spelled without a member pointer in the layout.
template <std::size_t N>
struct RES {
    using value_type = void;
    static void encode (Encoder &e) { e.zeros (N); }
    static void decode (Decoder &d) { (void) d.raw (N); }
};

// Everything left in the buffer.  packet.Payload.
struct Payload {
    using value_type = Bytes;

    static void encode (Encoder &e, const value_type &v)
    { e.raw (ByteView (v.data (), v.size ())); }

    static void decode (Decoder &d, value_type &v)
    {
        ByteView b = d.rest ();
        v.assign (b.begin (), b.end ());
    }
};

// ---------------------------------------------------- self-coding types
//
// Nodeid, Macaddr and Version know their own wire format, so they act as
// their own codec.  This matches the Python, where they subclass Field.
struct NodeidField {
    using value_type = Nodeid;
    static void encode (Encoder &e, const value_type &v) { e.uint (v.value (), 2); }
    static void decode (Decoder &d, value_type &v)
    { v = Nodeid (static_cast<std::uint16_t> (d.uint (2))); }
};

struct MacaddrField {
    using value_type = Macaddr;
    static void encode (Encoder &e, const value_type &v) { e.raw (v.view ()); }
    static void decode (Decoder &d, value_type &v)
    {
        ByteView b = d.raw (6);
        std::array<std::uint8_t, 6> a {};
        std::copy (b.begin (), b.end (), a.begin ());
        v = Macaddr (a);
    }
};

struct VersionField {
    using value_type = Version;
    static void encode (Encoder &e, const value_type &v)
    { e.byte (v.v1); e.byte (v.v2); e.byte (v.v3); }
    static void decode (Decoder &d, value_type &v)
    { v.v1 = d.byte (); v.v2 = d.byte (); v.v3 = d.byte (); }
};

// ------------------------------------------------------------- BM: bitmap
//
// One or more named bit ranges packed into a single integer field.
// packet.BM.  In a layout, each range is its own row referring to the same
// underlying byte offset; here each range is a separate BM entry and the
// packet's encode collects them.  Position and width are compile time, so
// the shift and mask fold away.
template <std::size_t Bits, typename T = std::uint32_t>
struct BMField {
    using value_type = T;
    static constexpr T mask = static_cast<T> ((1ull << Bits) - 1);
};

// ------------------------------------------------------ layout machinery

// One row of a layout: a codec, the member it reads and writes, and the
// name the Python version used (kept for error messages and monitoring).
template <typename Codec, typename Packet, typename Member>
struct FieldSpec {
    using codec_type = Codec;
    Member Packet::*member;
    const char      *name;

    void encode (Encoder &e, const Packet &p) const
    {
        try {
            Codec::encode (e, p.*member);
        } catch (const DecodeError &ex) {
            throw FieldOverflow (std::string ("field '") + name + "': "
                                 + ex.what ());
        }
    }

    void decode (Decoder &d, Packet &p) const
    {
        try {
            Codec::decode (d, p.*member);
        } catch (const DecodeError &ex) {
            throw DecodeError (std::string ("field '") + name + "': "
                               + ex.what ());
        }
    }
};

// A layout row with no attribute, i.e. RES.
template <typename Codec, typename Packet>
struct VoidFieldSpec {
    using codec_type = Codec;
    const char *name;

    void encode (Encoder &e, const Packet &) const { Codec::encode (e); }
    void decode (Decoder &d, Packet &) const { Codec::decode (d); }
};

// field<Codec> (&Packet::member, "name")
template <typename Codec, typename Packet, typename Member>
constexpr auto field (Member Packet::*m, const char *name)
{
    static_assert (std::is_convertible_v<typename Codec::value_type, Member>
                   || std::is_same_v<typename Codec::value_type, Member>,
                   "layout field type does not match the member type");
    return FieldSpec<Codec, Packet, Member> { m, name };
}

// reserved<RES<n>, Packet> ("name") -- a row that consumes bytes but has no
// attribute.
template <typename Codec, typename Packet>
constexpr auto reserved (const char *name = "reserved")
{
    return VoidFieldSpec<Codec, Packet> { name };
}

template <typename... F>
constexpr auto fields (F... f) { return std::tuple<F...> (f...); }

// Layout inheritance: the Python subclasses append their _layout to the base
// class's.  extend (Base::layout, ...) does the same thing here.
template <typename... B, typename... F>
constexpr auto extend (const std::tuple<B...> &base, F... f)
{
    return std::tuple_cat (base, std::tuple<F...> (f...));
}

}   // namespace decnet::packet

#endif  // DECNET_PACKET_FIELD_H
