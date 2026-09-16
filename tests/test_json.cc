// The JSON used by the application protocol.

#include "harness.h"

#include "decnet/common/json.h"

using namespace decnet;
using namespace decnet::json;

DN_TEST (json, encode_a_flat_object)
{
    Object o;
    o.set ("handle", 1);
    o.set ("type", "data");
    o.set ("data", "hi");
    // Key order is preserved, so an encoded message is reproducible.
    DN_ASSERT_EQ (o.encode (),
                  std::string ("{\"handle\":1,\"type\":\"data\",\"data\":\"hi\"}"));
}

DN_TEST (json, high_bytes_are_escaped_the_way_python_writes_them)
{
    // Bytes travel as latin-1, with non-printable characters escaped as
    // \uXXXX, matching Python's json.dumps.
    Object o;
    o.set ("handle", 1);
    o.set ("type", "data");
    o.set_bytes ("data", Bytes { 0x00, 0x01, 0xaa, 0xff });

    DN_ASSERT_EQ (o.encode (),
        std::string ("{\"handle\":1,\"type\":\"data\","
                     "\"data\":\"\\u0000\\u0001\\u00aa\\u00ff\"}"));
}

DN_TEST (json, parse_what_python_produces)
{
    // Verbatim output from python3 json.dumps for the same values.
    std::string text =
        "{\"handle\":1,\"type\":\"data\","
        "\"data\":\"\\u0000\\u0001\\u00aa\\u00ff\"}";
    Object o = Object::parse (text);

    DN_ASSERT_EQ (o.num ("handle"), 1);
    DN_ASSERT_EQ (o.str ("type"), std::string ("data"));
    DN_ASSERT_EQ (o.bytes ("data"), (Bytes { 0x00, 0x01, 0xaa, 0xff }));
}

DN_TEST (json, every_byte_value_survives_a_round_trip)
{
    // The protocol has to carry arbitrary bytes, so check all 256 of them
    // rather than a sample.
    Bytes all;
    for (int i = 0; i < 256; ++i) all.push_back (static_cast<std::uint8_t> (i));

    Object o;
    o.set_bytes ("data", all);
    Object q = Object::parse (o.encode ());
    DN_ASSERT_EQ (q.bytes ("data"), all);
}

DN_TEST (json, strings_with_json_metacharacters)
{
    Object o;
    o.set ("data", "quote\" backslash\\ newline\n tab\t");
    Object q = Object::parse (o.encode ());
    DN_ASSERT_EQ (q.str ("data"),
                  std::string ("quote\" backslash\\ newline\n tab\t"));
}

DN_TEST (json, value_types)
{
    Object o;
    o.set ("n", static_cast<std::int64_t> (-42));
    o.set ("t", true);
    o.set ("f", false);
    o.set ("s", "text");
    Object q = Object::parse (o.encode ());

    DN_ASSERT_EQ (q.num ("n"), -42);
    DN_ASSERT (q.get ("t")->as_bool ());
    DN_ASSERT (!q.get ("f")->as_bool ());
    DN_ASSERT_EQ (q.str ("s"), std::string ("text"));
    DN_ASSERT (q.has ("n"));
    DN_ASSERT (!q.has ("missing"));
}

DN_TEST (json, null_and_empty)
{
    DN_ASSERT_EQ (Object ().encode (), std::string ("{}"));
    DN_ASSERT_EQ (Object::parse ("{}").size (), 0u);

    Object o;
    o.set ("nothing", Value ());
    DN_ASSERT_EQ (o.encode (), std::string ("{\"nothing\":null}"));
    DN_ASSERT (Object::parse (o.encode ()).get ("nothing")->is_null ());
}

DN_TEST (json, defaults_for_absent_fields)
{
    Object o = Object::parse ("{\"a\":1}");
    DN_ASSERT_EQ (o.num ("missing", 7), 7);
    DN_ASSERT_EQ (o.str ("missing", "dflt"), std::string ("dflt"));
    DN_ASSERT (o.bytes ("missing").empty ());
}

DN_TEST (json, malformed_input_is_rejected)
{
    DN_ASSERT_THROWS (ParseError, Object::parse (""));
    DN_ASSERT_THROWS (ParseError, Object::parse ("{"));
    DN_ASSERT_THROWS (ParseError, Object::parse ("{\"a\"}"));
    DN_ASSERT_THROWS (ParseError, Object::parse ("{\"a\":}"));
    DN_ASSERT_THROWS (ParseError, Object::parse ("{\"a\":1,}"));
    DN_ASSERT_THROWS (ParseError, Object::parse ("[1,2]"));
    DN_ASSERT_THROWS (ParseError, Object::parse ("{\"a\":1} trailing"));
    DN_ASSERT_THROWS (ParseError, Object::parse ("{\"a\":\"unterminated"));
}

DN_TEST (json, log_records_from_an_application)
{
    // Application log record: level, message and {} arguments.  The only
    // array in the protocol.
    std::string text =
        "{\"level\": 10, \"message\": \"Starting MIRROR with arguments {}\","
        " \"args\": [\"[]\"]}";
    Object o = Object::parse (text);
    DN_ASSERT_EQ (o.num ("level"), 10);
    DN_ASSERT_EQ (o.format_message (),
                  std::string ("Starting MIRROR with arguments []"));

    // Several arguments, in order.
    Object m = Object::parse (
        "{\"message\":\"{} to {} on {}\",\"args\":[\"a\",2,true]}");
    DN_ASSERT_EQ (m.format_message (), std::string ("a to 2 on true"));

    // More placeholders than arguments: the extra ones stay as they are,
    // rather than the message being lost.
    Object f = Object::parse ("{\"message\":\"{} and {}\",\"args\":[\"x\"]}");
    DN_ASSERT_EQ (f.format_message (), std::string ("x and {}"));

    // No args at all is just the message.
    Object n = Object::parse ("{\"message\":\"plain\"}");
    DN_ASSERT_EQ (n.format_message (), std::string ("plain"));
}

DN_TEST (json, arrays_round_trip)
{
    Object o = Object::parse ("{\"args\":[\"a\",1,true,null]}");
    DN_ASSERT (o.get ("args")->is_array ());
    DN_ASSERT_EQ (o.get ("args")->as_array ().size (), 4u);
    DN_ASSERT_EQ (o.encode (),
                  std::string ("{\"args\":[\"a\",1,true,null]}"));
    DN_ASSERT_EQ (Object::parse ("{\"a\":[]}").get ("a")->as_array ().size (),
                  0u);
}

DN_TEST (json, shapes_this_protocol_does_not_use_are_rejected)
{
    // Nesting and fractional numbers would mean the far end is not
    // speaking this protocol; better to say so than to half-accept it.
    DN_ASSERT_THROWS (ParseError, Object::parse ("{\"a\":{\"b\":1}}"));
    DN_ASSERT_THROWS (ParseError, Object::parse ("{\"a\":1.5}"));
    DN_ASSERT_THROWS (ParseError, Object::parse ("{\"a\":[1,}"));
    // And a character that cannot have come from a latin-1 byte.
    DN_ASSERT_THROWS (ParseError, Object::parse ("{\"a\":\"\\u0100\"}"));
}

DN_TEST (json, a_real_message_from_the_protocol)
{
    // What session control sends an application for an inbound connection.
    std::string text =
        "{\"handle\":140234,\"data\":\"\",\"type\":\"connect\","
        "\"destination\":\"1.2\",\"srcuser\":\"TEST\",\"dstuser\":\"25\"}";
    Object o = Object::parse (text);
    DN_ASSERT_EQ (o.str ("type"), std::string ("connect"));
    DN_ASSERT_EQ (o.num ("handle"), 140234);
    DN_ASSERT_EQ (o.str ("destination"), std::string ("1.2"));
    DN_ASSERT (o.bytes ("data").empty ());

    // And what the application answers with.
    Object reply;
    reply.set ("handle", o.num ("handle"));
    reply.set ("type", "accept");
    reply.set_bytes ("data", Bytes { 0xff, 0xff });
    DN_ASSERT_EQ (reply.encode (),
        std::string ("{\"handle\":140234,\"type\":\"accept\","
                     "\"data\":\"\\u00ff\\u00ff\"}"));
}
