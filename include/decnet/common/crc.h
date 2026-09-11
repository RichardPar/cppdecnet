// decnet/common/crc.h -- the CRC variants DECnet data links need.
//
// Port of crc.py, which builds CRC classes from a polynomial, width, seed
// and reflection flags.  The Python version does that at class creation
// time with a metaclass; here the table is a constexpr array, so each
// variant costs nothing at runtime and the compiler can inline the update
// loop.

#ifndef DECNET_COMMON_CRC_H
#define DECNET_COMMON_CRC_H

#include "decnet/common/types.h"

#include <array>
#include <cstdint>

namespace decnet {

namespace detail {

template <typename T>
constexpr T reverse_bits (T v, unsigned width)
{
    T r = 0;
    for (unsigned i = 0; i < width; ++i) {
        r = static_cast<T> ((r << 1) | (v & 1));
        v = static_cast<T> (v >> 1);
    }
    return r;
}

}   // namespace detail

// Poly, Width, Seed, Reflect and Final match the parameters crc.py takes.
template <typename Word, Word Poly, unsigned Width, Word Seed,
          bool Reflect, Word Final>
class Crc {
public:
    using word_type = Word;
    static constexpr unsigned width = Width;
    static constexpr Word     mask  = Width == sizeof (Word) * 8
        ? static_cast<Word> (~Word (0))
        : static_cast<Word> ((Word (1) << Width) - 1);

    constexpr Crc () noexcept : value_ (initial ()) {}

    constexpr void update (std::uint8_t b) noexcept
    {
        if constexpr (Reflect)
            value_ = static_cast<Word> (
                (value_ >> 8) ^ table ()[(value_ ^ b) & 0xff]);
        else
            value_ = static_cast<Word> (
                (value_ << 8) ^ table ()[((value_ >> (Width - 8)) ^ b) & 0xff]);
        value_ &= mask;
    }

    void update (ByteView b) noexcept { for (auto c : b) update (c); }

    constexpr Word value () const noexcept
    { return static_cast<Word> ((value_ ^ Final) & mask); }

    // True when the accumulated CRC over data plus its appended check bytes
    // equals the residue, which is how the DDCMP receiver validates a block.
    constexpr bool good () const noexcept { return value () == residue (); }

    static constexpr Word residue ()
    { return static_cast<Word> (Final & mask); }

    constexpr void reset () noexcept { value_ = initial (); }

    // Convenience: the check value over one buffer.
    static Word compute (ByteView b) noexcept
    {
        Crc c;
        c.update (b);
        return c.value ();
    }

private:
    static constexpr Word initial ()
    {
        if constexpr (Reflect)
            return static_cast<Word> (detail::reverse_bits (Seed, Width) & mask);
        else
            return static_cast<Word> (Seed & mask);
    }

    static constexpr std::array<Word, 256> make_table ()
    {
        std::array<Word, 256> t {};
        Word poly = Reflect ? detail::reverse_bits<Word> (Poly, Width) : Poly;
        for (unsigned i = 0; i < 256; ++i) {
            Word c;
            if constexpr (Reflect) {
                c = static_cast<Word> (i);
                for (int k = 0; k < 8; ++k)
                    c = static_cast<Word> ((c & 1) ? (c >> 1) ^ poly : c >> 1);
            } else {
                c = static_cast<Word> (static_cast<Word> (i) << (Width - 8));
                for (int k = 0; k < 8; ++k)
                    c = static_cast<Word> (
                        (c & (Word (1) << (Width - 1)))
                        ? (c << 1) ^ poly : c << 1);
            }
            t[i] = static_cast<Word> (c & mask);
        }
        return t;
    }

    static const std::array<Word, 256> &table ()
    {
        static constexpr std::array<Word, 256> t = make_table ();
        return t;
    }

    Word value_;
};

// The variants the Python defines, with the names it uses.

// DDCMP header and data block check: CRC-16, reflected, seed 0.
using CRC16 = Crc<std::uint16_t, 0x8005, 16, 0, true, 0>;

// CRC-CCITT, used by the synchronous framer.
using CRCCCITT = Crc<std::uint16_t, 0x1021, 16, 0xffff, false, 0>;

// Ethernet FCS.
using CRC32 = Crc<std::uint32_t, 0x04c11db7, 32, 0xffffffff, true, 0xffffffff>;

}   // namespace decnet

#endif  // DECNET_COMMON_CRC_H
