// Tests for NICE data value coding (the data type classes in nice_coding.py).
//
// The wire forms here are the ones the Phase IV Network Management
// specification defines: every value carries its own type code, so these
// tests are as much about the code byte as about the payload.

#include "harness.h"

#include "decnet/nice/value.h"

using namespace decnet;
using namespace decnet::nice;

namespace {

Bytes bytes_of (std::initializer_list<int> v)
{
    Bytes b;
    for (int x : v) b.push_back (static_cast<std::uint8_t> (x));
    return b;
}

// A round trip through the wire form, which every case below wants.
Value roundtrip (const Value &v)
{
    return Value::parse (v.encode ());
}

}   // namespace

DN_TEST (nice, du_unsigned_decimal)
{
    Value v = Value::du (0x1234, 2);
    DN_ASSERT_EQ (v.type_code (), 0x02);
    DN_ASSERT_EQ (v.encode (), bytes_of ({ 0x02, 0x34, 0x12 }));
    DN_ASSERT_EQ (v.format (), std::string ("4660"));
    DN_ASSERT (roundtrip (v) == v);

    // One byte is the default width.
    DN_ASSERT_EQ (Value::du (7).encode (), bytes_of ({ 0x01, 7 }));
}

DN_TEST (nice, ds_signed_decimal)
{
    Value v = Value::ds (-2, 1);
    DN_ASSERT_EQ (v.type_code (), 0x11);
    DN_ASSERT_EQ (v.encode (), bytes_of ({ 0x11, 0xfe }));
    DN_ASSERT_EQ (v.format (), std::string ("-2"));

    // Sign extension on decode is the part worth checking.
    Value r = roundtrip (v);
    DN_ASSERT_EQ (r.as_int (), -2);
    DN_ASSERT_EQ (Value::parse (bytes_of ({ 0x12, 0x00, 0x80 })).as_int (),
                  -32768);
    DN_ASSERT_EQ (Value::parse (bytes_of ({ 0x12, 0xff, 0x7f })).as_int (),
                  32767);
}

DN_TEST (nice, h_hex_keeps_leading_zeroes)
{
    // The width carries information for a hex value, so the format keeps
    // every digit -- H1.format's reason for overriding.
    Value v = Value::h (0x0a, 2);
    DN_ASSERT_EQ (v.type_code (), 0x22);
    DN_ASSERT_EQ (v.encode (), bytes_of ({ 0x22, 0x0a, 0x00 }));
    DN_ASSERT_EQ (v.format (), std::string ("000a"));
    DN_ASSERT (roundtrip (v) == v);
}

DN_TEST (nice, o_octal_is_zero_padded)
{
    // A one byte octal value shows three digits: (8 + 2) / 3.
    Value v = Value::o (7, 1);
    DN_ASSERT_EQ (v.type_code (), 0x31);
    DN_ASSERT_EQ (v.format (), std::string ("007"));
    // Two bytes: (16 + 2) / 3 == 6 digits.
    DN_ASSERT_EQ (Value::o (0777, 2).format (), std::string ("000777"));
    DN_ASSERT (roundtrip (v) == v);
}

DN_TEST (nice, ai_ascii_image)
{
    Value v = Value::ai ("SAMPLE");
    DN_ASSERT_EQ (v.type_code (), 0x40);
    DN_ASSERT_EQ (v.encode (),
                  bytes_of ({ 0x40, 6, 'S', 'A', 'M', 'P', 'L', 'E' }));
    DN_ASSERT_EQ (v.format (), std::string ("SAMPLE"));
    DN_ASSERT (roundtrip (v) == v);

    // Empty is legal.
    DN_ASSERT_EQ (Value::ai ("").encode (), bytes_of ({ 0x40, 0 }));
    DN_ASSERT_EQ (roundtrip (Value::ai ("")).as_string (), std::string (""));
}

DN_TEST (nice, hi_hex_image)
{
    // Type code 0x20: the image form of the hex group, which is how a
    // hardware address travels in a NICE response.
    Value v = Value::hi (bytes_of ({ 0xaa, 0x00, 0x04, 0x00, 0x36, 0x24 }));
    DN_ASSERT_EQ (v.type_code (), 0x20);
    DN_ASSERT_EQ (v.encode (), bytes_of ({ 0x20, 6, 0xaa, 0x00, 0x04,
                                           0x00, 0x36, 0x24 }));
    DN_ASSERT_EQ (v.format (), std::string ("aa-00-04-00-36-24"));
    DN_ASSERT (roundtrip (v) == v);
}

DN_TEST (nice, c_coded_uses_labels)
{
    static const char *const states[] = { "on", "off", "service" };
    Value v = Value::c (1, 1);
    DN_ASSERT_EQ (v.type_code (), 0x81);
    DN_ASSERT_EQ (v.encode (), bytes_of ({ 0x81, 1 }));
    DN_ASSERT_EQ (v.format (Labels (states)), std::string ("off"));

    // A value with no label falls back to "#n", as C1.format does.
    DN_ASSERT_EQ (Value::c (9, 1).format (Labels (states)), std::string ("#9"));
    DN_ASSERT_EQ (Value::c (9, 1).format (), std::string ("#9"));
    DN_ASSERT (roundtrip (v) == v);
}

DN_TEST (nice, cm_coded_multiple)
{
    // Each element carries its own type code; the count is in the low
    // bits of the CM code.
    Value v = Value::cm ({ Value::du (3, 1), Value::ai ("ETH") });
    DN_ASSERT_EQ (v.type_code (), 0xc2);
    DN_ASSERT_EQ (v.encode (),
                  bytes_of ({ 0xc2, 0x01, 3, 0x40, 3, 'E', 'T', 'H' }));
    DN_ASSERT_EQ (v.format (), std::string ("3 ETH"));

    Value r = roundtrip (v);
    DN_ASSERT (r == v);
    DN_ASSERT_EQ (r.as_list ().size (), 2u);
    DN_ASSERT_EQ (r.as_list ()[1].as_string (), std::string ("ETH"));
}

DN_TEST (nice, cm_nests)
{
    Value inner = Value::cm ({ Value::du (1, 1), Value::du (2, 1) });
    Value outer = Value::cm ({ inner, Value::ai ("x") });
    DN_ASSERT (roundtrip (outer) == outer);
    DN_ASSERT_EQ (outer.format (), std::string ("1 2 x"));
}

DN_TEST (nice, invalid_type_codes_rejected)
{
    // A zero byte count in a counted group is not a valid encoding.
    DN_ASSERT_THROWS (DecodeError, Value::parse (bytes_of ({ 0x00, 0 })));
    DN_ASSERT_THROWS (DecodeError, Value::parse (bytes_of ({ 0x10, 0 })));
    DN_ASSERT_THROWS (DecodeError, Value::parse (bytes_of ({ 0x30, 0 })));
    DN_ASSERT_THROWS (DecodeError, Value::parse (bytes_of ({ 0x80, 0 })));
    // 0x41 is the AI code with a count, which has no meaning.
    DN_ASSERT_THROWS (DecodeError, Value::parse (bytes_of ({ 0x41, 0 })));
    // 0x50 and up in the low group are unassigned.
    DN_ASSERT_THROWS (DecodeError, Value::parse (bytes_of ({ 0x51, 0 })));
}

DN_TEST (nice, truncated_values_rejected)
{
    DN_ASSERT_THROWS (MissingData, Value::parse (bytes_of ({ 0x02, 0x34 })));
    DN_ASSERT_THROWS (MissingData, Value::parse (bytes_of ({ 0x40, 4, 'a' })));
    DN_ASSERT_THROWS (MissingData, Value::parse (bytes_of ({ 0xc2, 0x01, 3 })));
    DN_ASSERT_THROWS (MissingData, Value::parse (Bytes {}));
}

DN_TEST (nice, extra_data_after_a_value_rejected)
{
    DN_ASSERT_THROWS (ExtraData,
                      Value::parse (bytes_of ({ 0x01, 7, 0xff })));
}

DN_TEST (nice, byte_count_limits)
{
    DN_ASSERT_THROWS (FieldOverflow, Value::du (1, 0));
    DN_ASSERT_THROWS (FieldOverflow, Value::du (1, 16));
    DN_ASSERT_THROWS (FieldOverflow, Value::ai (std::string (256, 'x')));
    DN_ASSERT_THROWS (FieldOverflow, Value::cm (Value::List (16)));
}
