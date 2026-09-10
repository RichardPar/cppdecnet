// Routing message encoding and decoding.  Port of the RoutingMessage,
// Ph4RoutingMessage, L1Routing, L2Routing and PhaseIIIRouting classes and
// the L1Segment/L2Segment types in routing_packets.py.

#include "decnet/common/logging.h"
#include "decnet/routing/packets.h"

namespace decnet::routing {

namespace {

// The one's complement sum DECnet routing messages use: add up the words,
// then fold the carries back in.  Two folds are enough, because one fold
// of a 32 bit sum can itself carry at most once.
std::uint16_t ones_complement_sum (const std::uint16_t *words,
                                   std::size_t count, unsigned seed)
{
    std::uint32_t s = seed;
    for (std::size_t i = 0; i < count; ++i) s += words[i];
    s = (s & 0xffff) + (s >> 16);
    s = (s & 0xffff) + (s >> 16);
    return static_cast<std::uint16_t> (s);
}

// Split a routing message's payload into little endian words.
std::vector<std::uint16_t> payload_words (ByteView b)
{
    if (b.size () < 4)
        throw MissingData ("routing message shorter than its header");
    ByteView payload = b.subspan (4);
    if (payload.empty () || (payload.size () & 1))
        throw FormatError ("invalid routing message payload length "
                           + std::to_string (payload.size ()));
    std::vector<std::uint16_t> words (payload.size () / 2);
    for (std::size_t i = 0; i < words.size (); ++i)
        words[i] = static_cast<std::uint16_t> (payload[2 * i])
                 | static_cast<std::uint16_t> (payload[2 * i + 1] << 8);
    return words;
}

}   // namespace

// -------------------------------------------------------- RoutingMessage

std::uint64_t RoutingMessage::index_key (ByteView b)
{
    // Sum the whole payload with the checksum word complemented.  For a
    // message whose checksum is right this leaves 0xffff minus the seed
    // the sender used, which is what says whether this is a Phase III or a
    // Phase IV message.  Anything else is a checksum error, and no class
    // will match it.
    std::vector<std::uint16_t> words = payload_words (b);
    words.back () = static_cast<std::uint16_t> (~words.back ());
    return ones_complement_sum (words.data (), words.size (), 0);
}

void RoutingMessage::encode_header (Bytes &out) const
{
    std::uint8_t flags = static_cast<std::uint8_t> (
        (control ? 1 : 0) | ((type & 7) << 1) | ((ext_type & 7) << 4)
        | (pf ? 0x80 : 0));
    out.push_back (flags);
    out.push_back (static_cast<std::uint8_t> (srcnode & 0xff));
    out.push_back (static_cast<std::uint8_t> (srcnode >> 8));
    out.push_back (0);          // reserved
}

std::vector<std::uint16_t> RoutingMessage::decode_header (ByteView b)
{
    std::vector<std::uint16_t> words = payload_words (b);

    std::uint8_t flags = b[0];
    control  = (flags & 1) != 0;
    type     = static_cast<std::uint8_t> ((flags >> 1) & 7);
    ext_type = static_cast<std::uint8_t> ((flags >> 4) & 7);
    pf       = (flags & 0x80) != 0;
    srcnode  = static_cast<std::uint16_t> (b[1] | (b[2] << 8));

    // The class was chosen by the residue, so by the time we get here the
    // checksum is known good; just drop the word.
    words.pop_back ();
    if (words.empty ())
        throw FormatError ("routing message with no entries");
    return words;
}

void RoutingMessage::append_body (Bytes &out,
                                  const std::vector<std::uint16_t> &words,
                                  unsigned seed)
{
    for (std::uint16_t w : words) {
        out.push_back (static_cast<std::uint8_t> (w & 0xff));
        out.push_back (static_cast<std::uint8_t> (w >> 8));
    }
    std::uint16_t sum = ones_complement_sum (words.data (), words.size (), seed);
    out.push_back (static_cast<std::uint8_t> (sum & 0xff));
    out.push_back (static_cast<std::uint8_t> (sum >> 8));
}

// ---------------------------------------------------- Ph4RoutingMessage

std::size_t Ph4RoutingMessage::decode_into (ByteView b)
{
    std::vector<std::uint16_t> words = decode_header (b);
    segments.clear ();

    std::size_t pos = 0;
    while (pos < words.size ()) {
        if (words.size () - pos < 2)
            throw FormatError ("truncated routing message segment header");
        RouteSegment seg;
        std::uint16_t count = words[pos];
        seg.startid = words[pos + 1];
        pos += 2;
        if (words.size () - pos < count)
            throw FormatError ("routing message segment runs off the end");
        seg.entries.assign (words.begin () + static_cast<long> (pos),
                            words.begin () + static_cast<long> (pos + count));
        pos += count;
        if (!valid_segment (seg))
            throw FormatError ("invalid routing message segment, start "
                               + std::to_string (seg.startid) + ", count "
                               + std::to_string (count));
        segments.push_back (std::move (seg));
    }
    return b.size ();
}

Bytes Ph4RoutingMessage::encode_packet () const
{
    Bytes out;
    encode_header (out);

    std::vector<std::uint16_t> words;
    for (const RouteSegment &s : segments) {
        words.push_back (static_cast<std::uint16_t> (s.entries.size ()));
        words.push_back (s.startid);
        words.insert (words.end (), s.entries.begin (), s.entries.end ());
    }
    // Phase IV seeds the checksum with 1; that is what distinguishes it
    // from a Phase III message at the same code point.
    append_body (out, words, 1);
    return out;
}

std::vector<RouteUpdate>
Ph4RoutingMessage::updates (unsigned circuit_cost) const
{
    std::vector<RouteUpdate> ret;
    for (const RouteSegment &s : segments) {
        unsigned id = s.startid;
        for (std::uint16_t e : s.entries) {
            // The hop count in the message is from the sender; add the
            // hop to reach it, and the cost of the circuit it came in on.
            ret.push_back (RouteUpdate { id, entry_hops (e) + 1,
                                         entry_cost (e) + circuit_cost });
            ++id;
        }
    }
    return ret;
}

bool L1Routing::valid_segment (const RouteSegment &s) const noexcept
{
    // Level 1 entries are node numbers within our area.
    return !s.entries.empty () && s.entries.size () + s.startid <= 1024;
}

bool L2Routing::valid_segment (const RouteSegment &s) const noexcept
{
    // Level 2 entries are area numbers, and there is no area zero.
    return !s.entries.empty () && s.startid != 0
        && s.entries.size () + s.startid <= 64;
}

// ------------------------------------------------------ PhaseIIIRouting

std::size_t PhaseIIIRouting::decode_into (ByteView b)
{
    // No segment headers: the whole body is one run of entries starting at
    // node 1.
    entries = decode_header (b);
    return b.size ();
}

Bytes PhaseIIIRouting::encode_packet () const
{
    Bytes out;
    encode_header (out);
    append_body (out, entries, 0);      // Phase III seeds the checksum with 0
    return out;
}

std::vector<RouteUpdate>
PhaseIIIRouting::updates (unsigned circuit_cost) const
{
    std::vector<RouteUpdate> ret;
    unsigned id = 1;
    for (std::uint16_t e : entries) {
        ret.push_back (RouteUpdate { id, entry_hops (e) + 1,
                                     entry_cost (e) + circuit_cost });
        ++id;
    }
    return ret;
}

}   // namespace decnet::routing
