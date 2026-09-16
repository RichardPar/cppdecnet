// decnet/common/modulo.h -- sequence number arithmetic.
//
// Port of modulo.py.  Integers modulo N compared per RFC 1982: a is less
// than b when the forward distance from a to b is at most half the modulus.
// NSP uses this for 12-bit sequence numbers.
//
// The modulus is a template parameter, so mixing moduli is a compile error.
//
// With an even modulus, values exactly half the modulus apart are
// unordered.  Python raises TypeError; here comparison returns
// std::partial_ordering::unordered and comparable() returns false.

#ifndef DECNET_COMMON_MODULO_H
#define DECNET_COMMON_MODULO_H

#include "decnet/common/exceptions.h"

#include <compare>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace decnet {

template <std::uint32_t Modulus>
class Mod {
    static_assert (Modulus >= 2, "modulus must be at least 2");

public:
    using value_type = std::uint32_t;

    static constexpr value_type modulus = Modulus;

    // The largest forward distance that still counts as "less than".  For
    // an even modulus, a distance of exactly half is undefined; for an odd
    // one, every distance is ordered.
    static constexpr value_type maxdelta =
        (Modulus % 2) ? Modulus / 2 : Modulus / 2 - 1;
    static constexpr bool  has_undef = (Modulus % 2) == 0;
    static constexpr value_type undef = Modulus / 2;   // only if has_undef

    constexpr Mod () noexcept = default;

    // Out of range construction throws, as PyDECnet's __new__ does.  Use
    // wrap() for values that are meant to be reduced.
    constexpr explicit Mod (value_type v) : value_ (v)
    {
        if (v >= Modulus)
            throw std::overflow_error ("value " + std::to_string (v)
                                       + " out of range for modulus "
                                       + std::to_string (Modulus));
    }

    // Reduce into range rather than complaining.  NSP builds sequence
    // numbers this way from packet fields.
    static constexpr Mod wrap (std::uint64_t v) noexcept
    {
        Mod m;
        m.value_ = static_cast<value_type> (v % Modulus);
        return m;
    }

    constexpr value_type value () const noexcept { return value_; }
    constexpr explicit operator value_type () const noexcept { return value_; }

    // ------------------------------------------------------------ ordering

    friend constexpr bool operator== (Mod a, Mod b) noexcept
    { return a.value_ == b.value_; }

    friend constexpr std::partial_ordering operator<=> (Mod a, Mod b) noexcept
    {
        if (a.value_ == b.value_) return std::partial_ordering::equivalent;
        value_type delta = forward (a.value_, b.value_);
        if (has_undef && delta == undef)
            return std::partial_ordering::unordered;
        return delta <= maxdelta ? std::partial_ordering::less
                                 : std::partial_ordering::greater;
    }

    // True when a and b have a defined order.  PyDECnet signals
    // this by raising; being able to ask is more useful in a receive path.
    static constexpr bool comparable (Mod a, Mod b) noexcept
    {
        return !has_undef || forward (a.value_, b.value_) != undef;
    }

    // ---------------------------------------------------------- arithmetic

    constexpr Mod &operator+= (Mod o) noexcept
    { value_ = static_cast<value_type> ((value_ + o.value_) % Modulus); return *this; }

    constexpr Mod &operator-= (Mod o) noexcept
    { value_ = static_cast<value_type> ((value_ + Modulus - o.value_) % Modulus);
      return *this; }

    friend constexpr Mod operator+ (Mod a, Mod b) noexcept { return a += b; }
    friend constexpr Mod operator- (Mod a, Mod b) noexcept { return a -= b; }

    // Pre- and post-increment: the common "next sequence number" operation.
    constexpr Mod &operator++ () noexcept
    { value_ = static_cast<value_type> ((value_ + 1) % Modulus); return *this; }

    constexpr Mod operator++ (int) noexcept
    { Mod old = *this; ++*this; return old; }

    // The forward distance from this value to other, which is what the
    // flow control windows in NSP are actually measuring.
    constexpr value_type distance_to (Mod other) const noexcept
    { return forward (value_, other.value_); }

    std::string str () const { return std::to_string (value_); }

private:
    static constexpr value_type forward (value_type from, value_type to) noexcept
    { return static_cast<value_type> ((to + Modulus - from) % Modulus); }

    value_type value_ = 0;
};

}   // namespace decnet

#endif  // DECNET_COMMON_MODULO_H
