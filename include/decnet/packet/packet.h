// decnet/packet/packet.h -- the Packet base class.
//
// Port of packet.Packet.  The Python version builds encode and decode
// methods from _layout via a metaclass at class definition time; here the
// CRTP base walks Derived::layout, which is a constexpr tuple, so the loop
// is unrolled and each field's codec is inlined.

#ifndef DECNET_PACKET_PACKET_H
#define DECNET_PACKET_PACKET_H

#include "decnet/packet/field.h"

#include <optional>

namespace decnet::packet {

// Whether a packet class tolerates bytes left over after its last field.
// pydecnet allows them only when the class declares a payload attribute.
enum class Extra { reject, allow };

template <typename Derived, Extra ExtraPolicy = Extra::reject>
class Packet {
public:
    // Append this packet's wire form to out.
    void encode_to (Bytes &out) const
    {
        Encoder e (out);
        const Derived &self = static_cast<const Derived &> (*this);
        std::apply ([&] (const auto &...f) { (f.encode (e, self), ...); },
                    Derived::layout);
    }

    Bytes encode () const
    {
        Bytes out;
        out.reserve (64);
        encode_to (out);
        return out;
    }

    // Parse buf into this packet.  Returns the number of bytes consumed, so
    // a caller that layers packets (routing over datalink, NSP over
    // routing) can hand the remainder on.
    std::size_t decode (ByteView buf)
    {
        Decoder d (buf);
        Derived &self = static_cast<Derived &> (*this);
        std::apply ([&] (const auto &...f) { (f.decode (d, self), ...); },
                    Derived::layout);
        if constexpr (ExtraPolicy == Extra::reject)
            if (!d.empty ())
                throw ExtraData (std::to_string (d.remaining ())
                                 + " byte(s) of extra data");
        return d.position ();
    }

    // Convenience for the common "parse a whole datagram" case.
    static Derived parse (ByteView buf)
    {
        Derived p;
        p.decode (buf);
        return p;
    }

    // Try to parse; returns nullopt instead of throwing.  The receive paths
    // in routing and NSP drop malformed packets rather than unwinding, so
    // this is the form they use.
    static std::optional<Derived> try_parse (ByteView buf) noexcept
    {
        try {
            return parse (buf);
        } catch (const DecodeError &) {
            return std::nullopt;
        }
    }
};

}   // namespace decnet::packet

#endif  // DECNET_PACKET_PACKET_H
