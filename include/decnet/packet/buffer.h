// decnet/packet/buffer.h -- the cursor types the field codecs work against.

#ifndef DECNET_PACKET_BUFFER_H
#define DECNET_PACKET_BUFFER_H

#include "decnet/common/exceptions.h"
#include "decnet/common/types.h"

#include <cstring>

namespace decnet::packet {

// DECnet is little endian on the wire throughout, so there is no "big
// endian" spelling of these helpers on purpose.

class Encoder {
public:
    explicit Encoder (Bytes &out) noexcept : out_ (out) {}

    void byte (std::uint8_t b) { out_.push_back (b); }

    void raw (ByteView b) { out_.insert (out_.end (), b.begin (), b.end ()); }

    // n byte little endian unsigned integer.
    void uint (std::uint64_t v, std::size_t n)
    {
        for (std::size_t i = 0; i < n; ++i)
            out_.push_back (static_cast<std::uint8_t> (v >> (8 * i)));
    }

    void zeros (std::size_t n) { out_.insert (out_.end (), n, 0); }

    std::size_t size () const noexcept { return out_.size (); }
    Bytes &bytes () noexcept { return out_; }

private:
    Bytes &out_;
};

class Decoder {
public:
    explicit Decoder (ByteView in) noexcept : buf_ (in) {}

    std::size_t remaining () const noexcept { return buf_.size () - pos_; }
    bool empty () const noexcept { return remaining () == 0; }
    std::size_t position () const noexcept { return pos_; }

    void need (std::size_t n) const
    {
        if (remaining () < n)
            throw MissingData ("packet truncated: need " + std::to_string (n)
                               + " byte(s), have " + std::to_string (remaining ()));
    }

    std::uint8_t byte ()
    {
        need (1);
        return buf_[pos_++];
    }

    ByteView raw (std::size_t n)
    {
        need (n);
        ByteView r = buf_.subspan (pos_, n);
        pos_ += n;
        return r;
    }

    std::uint64_t uint (std::size_t n)
    {
        need (n);
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < n; ++i)
            v |= static_cast<std::uint64_t> (buf_[pos_ + i]) << (8 * i);
        pos_ += n;
        return v;
    }

    // Everything not yet consumed; used by Payload and by TLV sub-decoders.
    ByteView rest ()
    {
        ByteView r = buf_.subspan (pos_);
        pos_ = buf_.size ();
        return r;
    }

    ByteView peek_rest () const noexcept { return buf_.subspan (pos_); }

    // Peek at the next n bytes as a little endian integer, for optional fields
    // whose presence depends on a flag bit.
    std::uint64_t peek_uint (std::size_t n) const
    {
        if (remaining () < n) return 0;
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < n; ++i)
            v |= static_cast<std::uint64_t> (buf_[pos_ + i]) << (8 * i);
        return v;
    }

private:
    ByteView    buf_;
    std::size_t pos_ = 0;
};

}   // namespace decnet::packet

#endif  // DECNET_PACKET_BUFFER_H
