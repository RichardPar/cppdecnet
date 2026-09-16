// Port of tests/test_packet.py: the layout machinery.
//
// Round trip, field order, overflow, truncation, extra data and layout
// inheritance.

#include "harness.h"

#include "decnet/packet/packet.h"

using namespace decnet;
using namespace decnet::packet;

namespace {

// A packet with one of each basic field type.
struct Simple : Packet<Simple> {
    std::uint8_t  code = 0;
    std::uint16_t count = 0;
    Bytes         name;
    Nodeid        src;

    static constexpr auto layout = fields (
        field<B<1>>        (&Simple::code,  "code"),
        field<B<2>>        (&Simple::count, "count"),
        field<I<16>>       (&Simple::name,  "name"),
        field<NodeidField> (&Simple::src,   "src"));
};

// A subclass adding fields to a base layout, the way PyDECnet packet
// classes extend a common header.
struct Header : Packet<Header> {
    std::uint8_t flags = 0;

    static constexpr auto layout = fields (
        field<B<1>> (&Header::flags, "flags"));
};

struct Extended : Packet<Extended, Extra::allow> {
    std::uint8_t flags = 0;
    std::uint32_t serial = 0;
    Bytes        payload;

    static constexpr auto layout = extend (
        fields (field<B<1>> (&Extended::flags, "flags")),
        field<B<4>>   (&Extended::serial,  "serial"),
        field<Payload> (&Extended::payload, "payload"));
};

// Reserved bytes and extensible integers.
struct Reserved : Packet<Reserved> {
    std::uint32_t big = 0;

    static constexpr auto layout = fields (
        reserved<RES<3>, Reserved> (),
        field<EX<4>> (&Reserved::big, "big"));
};

// A one byte field holding a wider value, so that an out of range value
// reaches the codec's overflow check instead of being truncated first.
struct Small : Packet<Small> {
    std::uint16_t v = 0;
    static constexpr auto layout =
        fields (field<B<1, std::uint16_t>> (&Small::v, "v"));
};

Bytes bytes_of (std::initializer_list<int> v)
{
    Bytes b;
    for (int x : v) b.push_back (static_cast<std::uint8_t> (x));
    return b;
}

}   // namespace

DN_TEST (packet, roundtrip)
{
    Simple p;
    p.code  = 7;
    p.count = 0x1234;
    p.name  = bytes_of ({ 'A', 'B', 'C' });
    p.src   = Nodeid::parse ("9.54");

    Bytes wire = p.encode ();
    // code, count little endian, image count + data, node id little endian.
    DN_ASSERT_EQ (wire, bytes_of ({ 7, 0x34, 0x12, 3, 'A', 'B', 'C',
                                    0x36, 0x24 }));

    Simple q = Simple::parse (wire);
    DN_ASSERT_EQ (q.code, p.code);
    DN_ASSERT_EQ (q.count, p.count);
    DN_ASSERT_EQ (q.name, p.name);
    DN_ASSERT_EQ (q.src, p.src);
}

DN_TEST (packet, rejects_extra_data)
{
    Bytes wire = bytes_of ({ 7, 0x34, 0x12, 0, 0x36, 0x24, 0xff });
    DN_ASSERT_THROWS (ExtraData, Simple::parse (wire));
}

DN_TEST (packet, rejects_truncation)
{
    Bytes wire = bytes_of ({ 7, 0x34 });
    DN_ASSERT_THROWS (DecodeError, Simple::parse (wire));
}

DN_TEST (packet, try_parse_returns_nullopt)
{
    // The receive paths drop malformed packets rather than unwind.
    DN_ASSERT (!Simple::try_parse (bytes_of ({ 7 })).has_value ());
    DN_ASSERT (Simple::try_parse (bytes_of ({ 7, 0, 0, 0, 0x36, 0x24 }))
               .has_value ());
}

DN_TEST (packet, image_overflow_rejected)
{
    Simple p;
    p.name.assign (20, 'x');            // I<16> allows 16
    DN_ASSERT_THROWS (FieldOverflow, p.encode ());
}

DN_TEST (packet, integer_overflow_rejected)
{
    Small p;
    p.v = 256;
    DN_ASSERT_THROWS (FieldOverflow, p.encode ());
}

DN_TEST (packet, layout_inheritance_and_payload)
{
    Extended p;
    p.flags  = 0x81;
    p.serial = 0xdeadbeef;
    p.payload = bytes_of ({ 1, 2, 3 });

    Bytes wire = p.encode ();
    DN_ASSERT_EQ (wire, bytes_of ({ 0x81, 0xef, 0xbe, 0xad, 0xde, 1, 2, 3 }));

    Extended q = Extended::parse (wire);
    DN_ASSERT_EQ (q.flags, p.flags);
    DN_ASSERT_EQ (q.serial, p.serial);
    DN_ASSERT_EQ (q.payload, p.payload);
}

DN_TEST (packet, reserved_and_extensible)
{
    Reserved p;
    p.big = 300;                       // needs two extensible bytes
    Bytes wire = p.encode ();
    DN_ASSERT_EQ (wire, bytes_of ({ 0, 0, 0, 0xac, 0x02 }));

    Reserved q = Reserved::parse (wire);
    DN_ASSERT_EQ (q.big, 300u);

    // Small values take one byte.
    p.big = 5;
    DN_ASSERT_EQ (p.encode (), bytes_of ({ 0, 0, 0, 5 }));
}

DN_TEST (packet, decode_returns_consumed_length)
{
    // Layering: a caller parses the header and hands the rest downward.
    Bytes wire = bytes_of ({ 0x42, 0x99, 0x88 });
    Header h;
    std::size_t used = h.decode (ByteView (wire.data (), 1));
    DN_ASSERT_EQ (used, 1u);
    DN_ASSERT_EQ (h.flags, 0x42);
}
