// Tests for event records, ported from tests/test_event.py.
//
// Byte strings are PyDECnet's test vectors.  Each is decoded, checked,
// formatted and re-encoded.

#include "harness.h"

#include "decnet/events/events.h"
#include "decnet/nice/params.h"

#include <string>

using namespace decnet;
using namespace decnet::events;

namespace {

Bytes b (std::string_view s)
{
    return Bytes (reinterpret_cast<const std::uint8_t *> (s.data ()),
                  reinterpret_cast<const std::uint8_t *> (s.data ()) + s.size ());
}

bool has (const std::string &haystack, const std::string &needle)
{
    return haystack.find (needle) != std::string::npos;
}

// Every vector below starts with the same source node: 1.3, named GROK.
const std::string src = std::string ("\x03\x04\x04", 3) + "GROK";

}   // namespace

DN_TEST (event, decode_no_parameters)
{
    // The simplest event there is: 0.0, records lost, no entity, no
    // parameters, and a timestamp of zero with no milliseconds.
    Bytes buf = b (std::string ("\x01\x07\x00\x00\x00\x00\x00\x00\x00\x80", 10)
                   + src + std::string ("\xff", 1));
    Event e = Event::parse (buf);

    DN_ASSERT_EQ (e.id.cls, 0);
    DN_ASSERT_EQ (e.id.code, 0);
    DN_ASSERT (e.ms_absent);
    DN_ASSERT_EQ (e.source.id, Nodeid (1, 3));
    DN_ASSERT_EQ (e.source.name, std::string ("GROK"));
    DN_ASSERT (e.entity.is_none ());

    std::string s = e.str ();
    DN_ASSERT (has (s, "Event type 0.0"));
    DN_ASSERT (has (s, "Event records lost"));
    DN_ASSERT (has (s, "From node 1.3 (GROK)"));
    DN_ASSERT (has (s, "occurred 01-Jan-1977 00:00:00"));
    DN_ASSERT_EQ (e.encode (), buf);
}

DN_TEST (event, decode_milliseconds)
{
    // The same event with the millisecond field present.
    Bytes buf = b (std::string ("\x01\x07\x00\x00\x00\x00\x00\x00\xfa\x00", 10)
                   + src + std::string ("\xff", 1));
    Event e = Event::parse (buf);

    DN_ASSERT (!e.ms_absent);
    DN_ASSERT_EQ (e.milliseconds, 250);
    DN_ASSERT (has (e.str (), "occurred 01-Jan-1977 00:00:00.250"));
    DN_ASSERT_EQ (e.encode (), buf);
}

DN_TEST (event, decode_large_timestamp)
{
    // Half-day 32767 and 43198 seconds, which is where the timestamp
    // arithmetic would show a mistake if there were one.
    Bytes buf = b (std::string ("\x01\x07\x00\x00\xff\x7f\xbe\xa8\xdb\x03", 10)
                   + src + std::string ("\xff", 1));
    Event e = Event::parse (buf);

    DN_ASSERT (has (e.str (), "occurred 09-Nov-2021 23:59:58.987"));
    DN_ASSERT_EQ (e.encode (), buf);
}

DN_TEST (event, unknown_code_in_known_class)
{
    Bytes buf = b (std::string ("\x01\x07\x0f\x00\x00\x00\x00\x00\x00\x80", 10)
                   + src + std::string ("\xff", 1));
    Event e = Event::parse (buf);

    DN_ASSERT_EQ (e.id.cls, 0);
    DN_ASSERT_EQ (e.id.code, 15);
    // No definition, so no descriptive text -- but it still reads.
    DN_ASSERT (find_event (e.id) == nullptr);
    DN_ASSERT (has (e.str (), "Event type 0.15"));
    DN_ASSERT_EQ (e.encode (), buf);
}

DN_TEST (event, unknown_class)
{
    Bytes buf = b (std::string ("\x01\x07\x0f\x40\x00\x00\x00\x00\x00\x80", 10)
                   + src + std::string ("\xff", 1));
    Event e = Event::parse (buf);

    DN_ASSERT_EQ (e.id.cls, 256);
    DN_ASSERT_EQ (e.id.code, 15);
    DN_ASSERT (has (e.str (), "Event type 256.15"));
    DN_ASSERT_EQ (e.encode (), buf);
}

DN_TEST (event, unknown_class_with_node_entity)
{
    // The entity is a node with the executor bit set in its name length.
    Bytes buf = b (std::string ("\x01\x07\x0f\x40\x00\x00\x00\x00\x00\x80", 10)
                   + src + std::string ("\x00\x8c\x0c\x83", 4) + "ARK");
    Event e = Event::parse (buf);

    DN_ASSERT_EQ (e.entity.kind (), nice::Entity::node);
    DN_ASSERT_EQ (e.entity.as_node ().id, Nodeid (3, 140));
    DN_ASSERT (e.entity.as_node ().executor);
    DN_ASSERT (has (e.str (), "Node = 3.140 (ARK)"));
    DN_ASSERT_EQ (e.encode (), buf);
}

DN_TEST (event, unknown_parameters)
{
    // Six parameters of five different types, none of which this class
    // has definitions for.  They keep their numbers and their values.
    Bytes buf = b (std::string ("\x01\x07\x0f\x40\x00\x00\x00\x00\x00\x80", 10)
                   + src + std::string ("\xff", 1)
                   + std::string ("\x01\x00\x40\x06", 4) + "Foobar"
                   + std::string ("\x05\x00\x81\x0f", 4)
                   + std::string ("\x07\x00\x11\xfe", 4)
                   + std::string ("\x08\x00\x22\xab\x31", 5)
                   + std::string ("\x09\x00\x31\xaa", 4)
                   + std::string ("\x02\x01\xc2\x02\x12\x00\x20\x06"
                                  "\xaa\x00\x04\x00\x12\x08", 14));
    Event e = Event::parse (buf);

    DN_ASSERT_EQ (e.params.size (), 6u);
    DN_ASSERT_EQ (e.params.find (1)->value.as_string (), std::string ("Foobar"));
    DN_ASSERT_EQ (e.params.find (5)->value.as_uint (), 15u);
    DN_ASSERT_EQ (e.params.find (7)->value.as_int (), -2);
    DN_ASSERT_EQ (e.params.find (8)->value.as_uint (), 0x31abu);
    DN_ASSERT_EQ (e.params.find (9)->value.as_uint (), 0252u);

    std::string s = e.str ();
    DN_ASSERT (has (s, "Parameter #1 = Foobar"));
    DN_ASSERT (has (s, "Parameter #5 = #15"));
    DN_ASSERT (has (s, "Parameter #7 = -2"));
    DN_ASSERT (has (s, "Parameter #8 = 31ab"));
    DN_ASSERT (has (s, "Parameter #9 = 252"));
    DN_ASSERT (has (s, "Parameter #258 = 18 aa-00-04-00-12-08"));

    // PyDECnet cannot re-encode unknown parameters in the right order;
    // this port keeps them in a map keyed by number, so it can.
    DN_ASSERT_EQ (e.encode (), buf);
}

DN_TEST (event, node_entity)
{
    Bytes buf = b (std::string ("\x01\x07\x01\x00\x00\x00\x00\x00\x00\x80", 10)
                   + src + std::string ("\x00\x05\x08\x03", 4) + "ARK");
    Event e = Event::parse (buf);

    DN_ASSERT_EQ (e.id.code, 1);
    DN_ASSERT_EQ (e.entity.kind (), nice::Entity::node);
    DN_ASSERT_EQ (e.entity.as_node ().id, Nodeid (2, 5));
    DN_ASSERT (!e.entity.as_node ().executor);

    std::string s = e.str ();
    DN_ASSERT (has (s, "Event type 0.1"));
    DN_ASSERT (has (s, "Automatic node counters"));
    DN_ASSERT (has (s, "Node = 2.5 (ARK)"));
    DN_ASSERT_EQ (e.encode (), buf);
}

DN_TEST (event, string_entities)
{
    // Line, circuit and module all encode the same way and differ only in
    // the label they print under.
    struct { const char *code; const char *name; const char *label; } cases[] = {
        { "\x01", "DMC-0", "Line = DMC-0" },
        { "\x03", "DMC-0", "Circuit = DMC-0" },
        { "\x04", "AX.25", "Module = AX.25" },
    };
    for (const auto &c : cases) {
        Bytes buf = b (std::string ("\x01\x07\x08\x00\x00\x00\x00\x00\x00\x80", 10)
                       + src + std::string (c.code, 1)
                       + std::string ("\x05", 1) + c.name);
        Event e = Event::parse (buf);
        DN_ASSERT_EQ (e.entity.as_string (), std::string (c.name));
        DN_ASSERT (has (e.str (), c.label));
        DN_ASSERT (has (e.str (), "Automatic counters"));
        DN_ASSERT_EQ (e.encode (), buf);
    }
}

DN_TEST (event, area_entity)
{
    Bytes buf = b (std::string ("\x01\x07\x08\x00\x00\x00\x00\x00\x00\x80", 10)
                   + src + std::string ("\x05\x00\x33", 3));
    Event e = Event::parse (buf);

    DN_ASSERT_EQ (e.entity.kind (), nice::Entity::area);
    DN_ASSERT_EQ (e.entity.as_area (), 51u);
    DN_ASSERT (has (e.str (), "Area = 51"));
    DN_ASSERT_EQ (e.encode (), buf);
}

DN_TEST (event, unknown_entity)
{
    // Entity code 9 has no name.  It is read as a counted string, which is
    // what every entity but node and area is, and printed by number.
    Bytes buf = b (std::string ("\x01\x07\x08\x00\x00\x00\x00\x00\x00\x80", 10)
                   + src + std::string ("\x09\x05", 2) + "DMC-0");
    Event e = Event::parse (buf);

    DN_ASSERT_EQ (e.entity.kind (), 9);
    DN_ASSERT_EQ (e.entity.str (), std::string ("Entity #9 = DMC-0"));
    DN_ASSERT_EQ (e.entity.value (), std::string ("DMC-0"));
    DN_ASSERT (has (e.str (), "Entity #9 = DMC-0"));
    DN_ASSERT_EQ (e.encode (), buf);
}

DN_TEST (event, routing_event_decode)
{
    // 4.7, circuit down with a packet header, a reason and a status.
    Bytes buf = b (std::string ("\x01\x07\x07\x01\x00\x00\x00\x00\x00\x80", 10)
                   + src + std::string ("\x03\x05", 2) + "DMC-0"
                   + std::string ("\x00\x00\xc4\x21\x42\x02\x10\x00"
                                  "\x02\x03\x00\x01\x17", 13)
                   + std::string ("\x05\x00\x81\x0b", 4)
                   + std::string ("\x07\x00\x81\x07", 4));
    Event e = Event::parse (buf);

    DN_ASSERT_EQ (e.id.cls, 4);
    DN_ASSERT_EQ (e.id.code, 7);
    DN_ASSERT_EQ (e.params.find (0)->value.as_list ().size (), 4u);
    DN_ASSERT_EQ (e.params.find (5)->value.as_uint (), 11u);
    DN_ASSERT_EQ (e.params.find (7)->value.as_uint (), 7u);

    std::string s = e.str ();
    DN_ASSERT (has (s, "Event type 4.7"));
    DN_ASSERT (has (s, "Circuit down, circuit fault"));
    DN_ASSERT (has (s, "Circuit = DMC-0"));
    DN_ASSERT (has (s, " Packet header = 42 16 3 23"));
    DN_ASSERT (has (s, "Reason = Adjacency listener received invalid data"));
    // Status 7 has no label in the table, so it prints as a number.
    DN_ASSERT (has (s, "Status = #7"));
    DN_ASSERT_EQ (e.encode (), buf);
}

DN_TEST (event, routing_event_encode)
{
    // The same event built from scratch, to check that what we generate
    // matches what PyDECnet generates for the same values.
    Event e { { 4, 7 }, nice::Entity::make_circuit ("DMC-0") };
    e.halfday = e.seconds = e.milliseconds = 0;
    e.ms_absent = true;
    e.source = nice::NiceNode (Nodeid (1, 3), "GROK");
    e.param (0, nice::Value::cm ({ nice::Value::h (0x42, 1),
                                   nice::Value::du (16, 2),
                                   nice::Value::du (3, 2),
                                   nice::Value::du (23, 1) }));
    e.coded (5, 11);
    e.coded (7, 7);

    Bytes want = b (std::string ("\x01\x07\x07\x01\x00\x00\x00\x00\x00\x80", 10)
                    + src + std::string ("\x03\x05", 2) + "DMC-0"
                    + std::string ("\x00\x00\xc4\x21\x42\x02\x10\x00"
                                   "\x02\x03\x00\x01\x17", 13)
                    + std::string ("\x05\x00\x81\x0b", 4)
                    + std::string ("\x07\x00\x81\x07", 4));
    DN_ASSERT_EQ (e.encode (), want);
    DN_ASSERT_EQ (e.level (), logging::Level::warning);
}

DN_TEST (event, counters)
{
    // A counter's type lives in the top nibble of its parameter number,
    // and a mapped counter carries a bitmap of qualifiers before its value.
    Event e { { 0, 1 }, nice::Entity::make_node ({ Nodeid (2, 5), "ARK" }) };
    e.ms_absent = true;
    e.counter (600, 1234, 4);

    nice::Counter c;
    c.value = 9;
    c.bytes = 1;
    c.mapped = true;
    c.map = 0x0006;              // bits 1 and 2
    e.params.set_counter (1020, c);

    Event r = Event::parse (e.encode ());
    DN_ASSERT_EQ (r.params.find (600, true)->count.value, 1234u);
    DN_ASSERT_EQ (r.params.find (600, true)->count.bytes, 4u);
    DN_ASSERT (r.params.find (1020, true)->count.mapped);
    DN_ASSERT_EQ (r.params.find (1020, true)->count.map, 0x0006);
    DN_ASSERT_EQ (r.encode (), e.encode ());
}

DN_TEST (event, counter_saturation)
{
    // A counter that hits the top of its field says so rather than
    // printing a number that is certainly wrong.
    nice::Counter c;
    c.bytes = 1;
    c.value = 255;
    DN_ASSERT_EQ (c.format (), std::string (">254"));
    c.value = 254;
    DN_ASSERT_EQ (c.format (), std::string ("254"));
    c.bytes = 2;
    c.value = 70000;
    DN_ASSERT_EQ (c.format (), std::string (">65534"));
}

DN_TEST (event, timestamp_now_round_trips)
{
    Event e { { 4, 10 }, nice::Entity::make_circuit ("MUL-0") };
    // The clock reading has to land somewhere sane: the base date is 1977
    // and this is not a time machine.
    DN_ASSERT (e.halfday > 30000);
    DN_ASSERT (e.seconds < 12 * 60 * 60);
    Event r = Event::parse (e.encode ());
    DN_ASSERT_EQ (r.halfday, e.halfday);
    DN_ASSERT_EQ (r.seconds, e.seconds);
    DN_ASSERT_EQ (r.milliseconds, e.milliseconds);
    DN_ASSERT_EQ (r.timestamp (), e.timestamp ());
}
