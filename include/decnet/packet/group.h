// decnet/packet/group.h -- field groups: BM and TLV.
//
// Port of packet.BM and packet.TLV, which pydecnet calls FieldGroups
// because one layout row produces several named attributes.  A plain
// FieldSpec binds one codec to one member; these bind one codec to several,
// so they carry their own tuple of sub-entries.

#ifndef DECNET_PACKET_GROUP_H
#define DECNET_PACKET_GROUP_H

#include "decnet/packet/field.h"

#include <map>
#include <optional>

namespace decnet::packet {

// ============================================================== BM
//
// A group of named bit ranges packed into one little endian integer field.
// The Python layout row
//
//     ( packet.BM,
//       ( "mbz",  0, 1 ),
//       ( "type", 1, 3 ),
//       ( "qual", 4, 2, QualType ))
//
// becomes
//
//     bm<Msg> (bmf (&Msg::mbz,  "mbz",  0, 1),
//              bmf (&Msg::type, "type", 1, 3),
//              bmf (&Msg::qual, "qual", 4, 2))
//
// The field width in bytes is derived from the highest bit used, exactly as
// makecoderow does: (topbit + 8) / 8.

template <typename Packet, typename Member>
struct BMBit {
    Member Packet::*member;
    const char     *name;
    unsigned        start;
    unsigned        bits;

    constexpr unsigned topbit () const noexcept { return start + bits - 1; }
};

template <typename Packet, typename Member>
constexpr auto bmf (Member Packet::*m, const char *name,
                    unsigned start, unsigned bits)
{
    return BMBit<Packet, Member> { m, name, start, bits };
}

namespace detail {

// Members appear as bool, an unsigned integer or a scoped enum; funnel them
// all through a pair of conversions so BM does not need an overload per type.
template <typename T>
constexpr std::uint64_t to_bits (const T &v) noexcept
{
    if constexpr (std::is_enum_v<T>)
        return static_cast<std::uint64_t> (static_cast<std::underlying_type_t<T>> (v));
    else
        return static_cast<std::uint64_t> (v);
}

template <typename T>
constexpr T from_bits (std::uint64_t v) noexcept
{
    if constexpr (std::is_enum_v<T>)
        return static_cast<T> (static_cast<std::underlying_type_t<T>> (v));
    else
        return static_cast<T> (v);
}

}   // namespace detail

template <typename Packet, typename... Bits>
struct BMGroup {
    std::tuple<Bits...> bits;
    std::size_t         flen;

    void encode (Encoder &e, const Packet &p) const
    {
        std::uint64_t field = 0;
        std::apply ([&] (const auto &...b) {
            ([&] {
                std::uint64_t v = detail::to_bits (p.*(b.member));
                if (b.bits < 64 && (v >> b.bits) != 0)
                    throw FieldOverflow (std::string ("field '") + b.name
                                         + "' value " + std::to_string (v)
                                         + " too large for "
                                         + std::to_string (b.bits) + " bits");
                field |= v << b.start;
            } (), ...);
        }, bits);
        e.uint (field, flen);
    }

    void decode (Decoder &d, Packet &p) const
    {
        std::uint64_t field = d.uint (flen);
        std::apply ([&] (const auto &...b) {
            ([&] {
                std::uint64_t mask = (b.bits >= 64)
                    ? ~std::uint64_t (0) : ((std::uint64_t (1) << b.bits) - 1);
                using M = std::remove_reference_t<decltype (p.*(b.member))>;
                p.*(b.member) = detail::from_bits<M> ((field >> b.start) & mask);
            } (), ...);
        }, bits);
    }

    // The value of one named sub-field, read straight from a raw buffer at
    // the given offset.  This is what indexed dispatch uses to pick a
    // packet class before the packet is parsed -- packet.BM.makegetindex.
    static std::uint64_t peek (ByteView buf, std::size_t offset,
                               std::size_t flen_, unsigned start, unsigned bits)
    {
        if (buf.size () < offset + flen_)
            throw MissingData ("buffer too short for bitmap index field");
        std::uint64_t field = 0;
        for (std::size_t i = 0; i < flen_; ++i)
            field |= static_cast<std::uint64_t> (buf[offset + i]) << (8 * i);
        std::uint64_t mask = (bits >= 64)
            ? ~std::uint64_t (0) : ((std::uint64_t (1) << bits) - 1);
        return (field >> start) & mask;
    }
};

template <typename Packet, typename... Bits>
constexpr auto bm (Bits... b)
{
    unsigned topbit = 0;
    ((topbit = b.topbit () > topbit ? b.topbit () : topbit), ...);
    return BMGroup<Packet, Bits...> { std::tuple<Bits...> (b...),
                                      (topbit + 8) / 8 };
}

// ============================================================== TLV
//
// A sequence of tag/length/value items filling the rest of the packet.
// Port of packet.TLV.  Optional members carry presence the way Python's
// None does: an item is emitted only when its member holds a value.
//
//     tlv<Msg, 2, 1, Wild::yes> (
//         tlvf<1, VersionField> (&Msg::version, "version"),
//         tlvf<7, MacaddrField> (&Msg::hwaddr,  "hwaddr"))

enum class Wild     { no, yes };      // accept and keep unknown tags
enum class Tolerant { no, yes };      // ignore a truncated item at the end

template <unsigned Tag, typename Codec, typename Packet, typename Member>
struct TlvEntry {
    std::optional<Member> Packet::*member;
    const char                    *name;

    static constexpr unsigned tag = Tag;

    bool encode_value (Bytes &out, const Packet &p) const
    {
        const auto &opt = p.*member;
        if (!opt.has_value ()) return false;
        Encoder e (out);
        Codec::encode (e, *opt);
        return true;
    }

    void decode_value (Decoder &d, Packet &p) const
    {
        Member v {};
        Codec::decode (d, v);
        p.*member = std::move (v);
    }
};

template <unsigned Tag, typename Codec, typename Packet, typename Member>
constexpr auto tlvf (std::optional<Member> Packet::*m, const char *name)
{
    static_assert (std::is_same_v<typename Codec::value_type, Member>,
                   "TLV field type does not match the member type");
    return TlvEntry<Tag, Codec, Packet, Member> { m, name };
}

// A TLV item whose value is a bitmap group rather than a single field --
// MOP's System ID "services" item, for example.  As in pydecnet, a group
// item has no presence flag and is always emitted.
template <unsigned Tag, typename Group>
struct TlvGroupEntry {
    Group       group;
    const char *name;

    static constexpr unsigned tag = Tag;

    template <typename Packet>
    bool encode_value (Bytes &out, const Packet &p) const
    {
        Encoder e (out);
        group.encode (e, p);
        return true;
    }

    template <typename Packet>
    void decode_value (Decoder &d, Packet &p) const { group.decode (d, p); }
};

template <unsigned Tag, typename Group>
constexpr auto tlvg (Group g, const char *name)
{
    return TlvGroupEntry<Tag, Group> { g, name };
}

template <typename Packet, unsigned TagLen, unsigned LenLen,
          Wild W, Tolerant T, typename... Entries>
struct TlvGroup {
    std::tuple<Entries...> entries;
    // Where unknown items go when wild.  Null unless W is Wild::yes.
    std::map<unsigned, Bytes> Packet::*unknown = nullptr;

    void encode (Encoder &e, const Packet &p) const
    {
        std::apply ([&] (const auto &...ent) {
            ([&] {
                Bytes value;
                if (!ent.encode_value (value, p)) return;
                e.uint (ent.tag, TagLen);
                if (LenLen < 8
                    && value.size () >= (std::size_t (1) << (8 * LenLen)))
                    throw FieldOverflow (std::string ("TLV item '") + ent.name
                                         + "' value too long for the length field");
                e.uint (value.size (), LenLen);
                e.raw (ByteView (value.data (), value.size ()));
            } (), ...);
        }, entries);

        // Unknown items are written back out so that a packet that was
        // parsed and re-encoded survives a round trip intact.
        if constexpr (W == Wild::yes) {
            if (unknown)
                for (const auto &[tag, value] : p.*unknown) {
                    e.uint (tag, TagLen);
                    e.uint (value.size (), LenLen);
                    e.raw (ByteView (value.data (), value.size ()));
                }
        }
    }

    void decode (Decoder &d, Packet &p) const
    {
        while (!d.empty ()) {
            if (d.remaining () < TagLen + LenLen) {
                if constexpr (T == Tolerant::yes) {
                    // Swallow the runt, as TLV.decode does by returning b''
                    // when the packet class is tolerant.  Leaving it would
                    // trip the enclosing packet's extra-data check instead.
                    (void) d.rest ();
                    return;
                }
                throw MissingData ("incomplete TLV item at end of buffer");
            }
            unsigned    tag  = static_cast<unsigned> (d.uint (TagLen));
            std::size_t vlen = static_cast<std::size_t> (d.uint (LenLen));
            if (d.remaining () < vlen)
                throw MissingData ("TLV item " + std::to_string (tag)
                                   + " extends beyond the end of the buffer");
            ByteView value = d.raw (vlen);

            bool handled = false;
            std::apply ([&] (const auto &...ent) {
                ([&] {
                    if (handled || ent.tag != tag) return;
                    handled = true;
                    Decoder vd (value);
                    ent.decode_value (vd, p);
                    // The value must account for exactly its declared
                    // length, unless the packet class is tolerant.
                    if (!vd.empty () && T == Tolerant::no)
                        throw ExtraData ("TLV item " + std::to_string (tag)
                                         + " has " + std::to_string (vd.remaining ())
                                         + " unparsed byte(s)");
                } (), ...);
            }, entries);

            if (handled) continue;

            if constexpr (W == Wild::yes) {
                if (unknown)
                    (p.*unknown)[tag].assign (value.begin (), value.end ());
            } else {
                throw InvalidTag ("unknown TLV tag " + std::to_string (tag));
            }
        }
    }
};

// tlv<Packet, taglen, lenlen> (entries...) -- strict, no unknown tags.
template <typename Packet, unsigned TagLen, unsigned LenLen,
          Wild W = Wild::no, Tolerant T = Tolerant::no, typename... Entries>
constexpr auto tlv (Entries... e)
{
    return TlvGroup<Packet, TagLen, LenLen, W, T, Entries...>
        { std::tuple<Entries...> (e...), nullptr };
}

// The wild form, which needs somewhere to put unrecognised items.
template <typename Packet, unsigned TagLen, unsigned LenLen,
          Tolerant T = Tolerant::no, typename... Entries>
constexpr auto tlv_wild (std::map<unsigned, Bytes> Packet::*unknown,
                         Entries... e)
{
    return TlvGroup<Packet, TagLen, LenLen, Wild::yes, T, Entries...>
        { std::tuple<Entries...> (e...), unknown };
}

}   // namespace decnet::packet

#endif  // DECNET_PACKET_GROUP_H
