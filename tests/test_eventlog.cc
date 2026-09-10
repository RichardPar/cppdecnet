// The event logger: event lists, filters, sinks, and a record travelling
// from one node to another over a real logical link.
//
// Ported from the filtering and sink code in event_logger.py.  There is no
// upstream unit test for this part, so the checks are against the DNA
// Network Management specification's description of an event-list and
// against what the two ends of a remote sink have to agree on.

#include "harness.h"

#include "decnet/common/socket.h"
#include "decnet/config.h"
#include "decnet/events/logger.h"
#include "decnet/node.h"
#include "decnet/routing/routing.h"
#include "decnet/session/session.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

using namespace decnet;
using namespace decnet::events;

namespace {

std::uint16_t free_port ()
{
    SourceAddress any ("127.0.0.1", 0);
    Socket s = any.create_server ();
    if (!s) throw std::runtime_error ("cannot find a free port");
    sockaddr_storage sa {};
    socklen_t len = sizeof sa;
    ::getsockname (s.fd (), reinterpret_cast<sockaddr *> (&sa), &len);
    return ntohs (reinterpret_cast<sockaddr_in *> (&sa)->sin_port);
}

template <typename P>
bool wait_until (P pred, std::chrono::milliseconds timeout
                     = std::chrono::seconds (20))
{
    auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        if (pred ()) return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    return pred ();
}

bool in (const EventSet &s, unsigned cls, unsigned code)
{
    return s.count (EventId { static_cast<std::uint16_t> (cls),
                              static_cast<std::uint8_t> (code) }) != 0;
}

}   // namespace

// ------------------------------------------------------------ event lists

DN_TEST (eventlog, parse_single_event)
{
    EventSet s = parse_events ("4.7");
    DN_ASSERT_EQ (s.size (), 1u);
    DN_ASSERT (in (s, 4, 7));
}

DN_TEST (eventlog, parse_range)
{
    EventSet s = parse_events ("4.7-10");
    DN_ASSERT_EQ (s.size (), 4u);
    DN_ASSERT (in (s, 4, 7));
    DN_ASSERT (in (s, 4, 10));
    DN_ASSERT (!in (s, 4, 11));
}

DN_TEST (eventlog, parse_class_carries_forward)
{
    // "3.1,4.1-12,5.2,4,7": once a class is named it applies to every
    // entry after it until another class appears.  The trailing "4,7" are
    // codes in class 5, not classes of their own.
    EventSet s = parse_events ("3.1,4.1-12,5.2,4,7");
    DN_ASSERT (in (s, 3, 1));
    DN_ASSERT (in (s, 4, 1));
    DN_ASSERT (in (s, 4, 12));
    DN_ASSERT (in (s, 5, 2));
    DN_ASSERT (in (s, 5, 4));
    DN_ASSERT (in (s, 5, 7));
    DN_ASSERT (!in (s, 4, 13));
}

DN_TEST (eventlog, parse_wildcards)
{
    // A star selects every code in the class -- but only the ones this
    // build can actually raise survive into a filter.
    EventSet s = parse_events ("4.*");
    DN_ASSERT_EQ (s.size (), 32u);

    // "*.*" is every event we know about.
    DN_ASSERT_EQ (parse_events ("*.*"), filterable_events ());
}

DN_TEST (eventlog, parse_rejects_nonsense)
{
    struct { const char *s; const char *why; } bad[] = {
        { "7",       "no class has been named yet" },
        { "4.32",    "codes stop at 31" },
        { "4.10-5",  "the range runs backwards" },
        { "4.x",     "not a number" },
        { "512.1",   "class out of range" },
        { "4.1-",    "range with no end" },
    };
    for (const auto &c : bad) {
        bool threw = false;
        try { parse_events (c.s); }
        catch (const std::invalid_argument &) { threw = true; }
        if (!threw)
            ::dntest::fail (__FILE__, __LINE__,
                            std::string ("accepted \"") + c.s + "\": "
                            + c.why);
    }
}

DN_TEST (eventlog, filterable_events_excludes_other_vendors)
{
    const EventSet &k = filterable_events ();
    DN_ASSERT (in (k, 4, 7));
    DN_ASSERT (in (k, 0, 0));
    // Class 33 is DECnet/E's.  We can read a record from one, but we never
    // raise one, so there is nothing to filter on.
    DN_ASSERT (!in (k, 33, 0));
    DN_ASSERT (!in (k, 128, 1));
}

// ----------------------------------------------------------------- filters

DN_TEST (eventlog, filter_matches_by_number)
{
    EventFilter f;
    f.set_filter (parse_events ("4.7,4.10"));

    Event up { { 4, 10 }, nice::Entity::make_circuit ("MUL-0") };
    Event adj { { 4, 15 }, nice::Entity::make_circuit ("MUL-0") };
    DN_ASSERT (f.contains (up));
    DN_ASSERT (!f.contains (adj));
}

DN_TEST (eventlog, filter_ignores_events_we_cannot_raise)
{
    // Enabling an event this build never generates does nothing: filtering
    // applies to locally generated events only.
    EventFilter f;
    f.set_filter (parse_events ("33.0"));
    DN_ASSERT (f.empty ());
}

DN_TEST (eventlog, filter_can_be_qualified_by_entity)
{
    // A qualified entry matches only events about that one entity.
    EventFilter f;
    Entity mul0 = nice::Entity::make_circuit ("MUL-0");
    f.set_filter (parse_events ("4.10"), mul0.key ());

    Event on_mul0 { { 4, 10 }, mul0 };
    Event on_eth0 { { 4, 10 }, nice::Entity::make_circuit ("ETH-0") };
    DN_ASSERT (f.contains (on_mul0));
    DN_ASSERT (!f.contains (on_eth0));

    // A line called MUL-0 is a different thing from a circuit called MUL-0.
    Event on_line { { 4, 10 }, nice::Entity::make_line ("MUL-0") };
    DN_ASSERT (!f.contains (on_line));
}

DN_TEST (eventlog, filter_entries_can_be_removed)
{
    EventFilter f;
    f.set_filter (parse_events ("4.*"));
    Event e { { 4, 10 }, nice::Entity::make_circuit ("MUL-0") };
    DN_ASSERT (f.contains (e));

    f.set_filter (parse_events ("4.10"), { }, false);
    DN_ASSERT (!f.contains (e));
    DN_ASSERT (f.contains (Event { { 4, 15 },
                                   nice::Entity::make_circuit ("MUL-0") }));
}

DN_TEST (eventlog, filter_format_collapses_runs)
{
    EventFilter f;
    f.set_filter (parse_events ("4.7-10,4.15"));
    DN_ASSERT_EQ (f.format (), std::string ("4.7-10,15"));

    EventFilter g;
    g.set_filter (parse_events ("2.0-1,4.0"));
    DN_ASSERT_EQ (g.format (), std::string ("2.0-1,4.0"));
}

// ------------------------------------------------------------------- sinks

DN_TEST (eventlog, default_configuration_is_a_console_with_everything)
{
    Config cfg = Config::from_string ("routing 1.1 --type l1router\n"
                                      "node 1.1 NODEA\n");
    Node n (cfg);

    EventFilter *f = n.event_logger ()->local_filter ("console");
    DN_ASSERT (f != nullptr);
    DN_ASSERT (f->contains (Event { { 4, 10 },
                                    nice::Entity::make_circuit ("MUL-0") }));
}

DN_TEST (eventlog, an_event_list_narrows_the_console)
{
    Config cfg = Config::from_string ("routing 1.1 --type l1router\n"
                                      "node 1.1 NODEA\n"
                                      "logging console --events 4.7-10\n");
    Node n (cfg);

    EventFilter *f = n.event_logger ()->local_filter ("console");
    DN_ASSERT (f != nullptr);
    DN_ASSERT (f->contains (Event { { 4, 10 },
                                    nice::Entity::make_circuit ("MUL-0") }));
    DN_ASSERT (!f->contains (Event { { 4, 15 },
                                     nice::Entity::make_circuit ("MUL-0") }));
}

DN_TEST (eventlog, the_monitor_sink_delivers_to_its_callback)
{
    Config cfg = Config::from_string ("routing 1.1 --type l1router\n"
                                      "node 1.1 NODEA\n"
                                      "logging monitor\n");
    Node n (cfg);

    std::vector<EventId> seen;
    n.event_logger ()->register_monitor (
        [&seen] (const Event &e) { seen.push_back (e.id); },
        parse_events ("4.10"));

    Event up { { 4, 10 }, nice::Entity::make_circuit ("MUL-0") };
    Event adj { { 4, 15 }, nice::Entity::make_circuit ("MUL-0") };
    n.logevent (up);
    n.logevent (adj);

    DN_ASSERT_EQ (seen.size (), 1u);
    DN_ASSERT (seen[0] == (EventId { 4, 10 }));
}

DN_TEST (eventlog, the_file_sink_writes_counted_records)
{
    std::string path = "/tmp/dn-eventlog-test.dat";
    std::remove (path.c_str ());

    Config cfg = Config::from_string ("routing 1.1 --type l1router\n"
                                      "node 1.1 NODEA\n"
                                      "logging file --sink-file " + path
                                      + " --events 4.10\n");
    {
        Node n (cfg);
        n.event_logger ()->start ();
        Event up { { 4, 10 }, nice::Entity::make_circuit ("MUL-0") };
        up.ms_absent = true;
        n.logevent (up);
        // This one is not in the filter, so it must not appear.
        Event adj { { 4, 15 }, nice::Entity::make_circuit ("MUL-0") };
        n.logevent (adj);
        n.event_logger ()->stop ();
    }

    std::FILE *f = std::fopen (path.c_str (), "rb");
    DN_ASSERT (f != nullptr);
    Bytes contents;
    int c;
    while ((c = std::fgetc (f)) != EOF)
        contents.push_back (static_cast<std::uint8_t> (c));
    std::fclose (f);
    std::remove (path.c_str ());

    // Two bytes of little endian length, then exactly that many bytes.
    DN_ASSERT (contents.size () > 2);
    std::size_t len = contents[0] | (std::size_t (contents[1]) << 8);
    DN_ASSERT_EQ (contents.size (), len + 2);

    Event back = Event::parse (ByteView (contents.data () + 2, len));
    DN_ASSERT (back.id == (EventId { 4, 10 }));
    DN_ASSERT_EQ (back.entity.as_string (), std::string ("MUL-0"));
    DN_ASSERT_EQ (back.source.id, Nodeid (1, 1));
}

// ------------------------------------------------------------ remote sinks

namespace {

// Two endnodes on a Multinet circuit.  A logs to B's event logger.
struct Pair {
    std::uint16_t port = free_port ();
    Config acfg, bcfg;
    std::unique_ptr<Node> a, b;

    Pair ()
        : acfg (Config::from_string (
              "routing 1.1 --type l1router\nnode 1.1 NODEA\nnode 1.2 NODEB\n"
              "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
              + ":listen --t3 2\n"
              // Destined for NODEB's monitor sink: the record says which
              // of the far end's sinks asked for it, and NODEB has only
              // that one.
              "logging monitor --sink-node NODEB --events 4.10,4.15\n")),
          bcfg (Config::from_string (
              "routing 1.2 --type endnode\nnode 1.2 NODEB\nnode 1.1 NODEA\n"
              "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
              + ":connect --t3 2\n"
              "logging monitor\n"))
    {
        a = std::make_unique<Node> (acfg);
        b = std::make_unique<Node> (bcfg);
    }

    void start ()
    {
        a->start ();
        b->start ();
        wait_until ([&] {
            return a->routing ()->adjacency_count () == 1
                && b->routing ()->adjacency_count () == 1;
        });
    }
    void stop () { if (b) b->stop (); if (a) a->stop (); }
};

}   // namespace

DN_TEST (eventlog, a_remote_sink_is_configured_with_three_filters)
{
    Config cfg = Config::from_string (
        "routing 1.1 --type l1router\nnode 1.1 NODEA\nnode 1.2 NODEB\n"
        "logging console --sink-node NODEB --events 4.10\n"
        "logging file --sink-node NODEB --events 4.15\n");
    Node n (cfg);

    RemoteSink *s = n.event_logger ()->remote_sink ("NODEB");
    DN_ASSERT (s != nullptr);
    // One connection, but a filter per sink type at the far end.
    DN_ASSERT (s->filter (0).contains (
                   Event { { 4, 10 }, nice::Entity::make_circuit ("MUL-0") }));
    DN_ASSERT (s->filter (1).contains (
                   Event { { 4, 15 }, nice::Entity::make_circuit ("MUL-0") }));
    DN_ASSERT (!s->filter (0).contains (
                   Event { { 4, 15 }, nice::Entity::make_circuit ("MUL-0") }));
}

DN_TEST (eventlog, an_event_travels_to_a_remote_sink)
{
    Pair p;

    // Collect what arrives at B before anything can be sent.
    std::mutex m;
    std::vector<Event> got;
    p.b->event_logger ()->register_monitor (
        [&] (const Event &e) {
            std::lock_guard lock (m);
            got.push_back (e);
        },
        parse_events ("4.10,4.15"));

    p.start ();

    // A raises an event that its filter selects for the remote sink.
    Event up { { 4, 10 }, nice::Entity::make_circuit ("MUL-0") };
    up.param (events::param::adjacent_node,
              events::node_value (p.a->nicenode (Nodeid (1, 2))));
    p.a->logevent (up);

    // B raises circuit-up events of its own, so wait for the one whose
    // source is A: that is the record that came down the wire.
    auto from_a = [&] () -> const Event * {
        for (const Event &e : got)
            if (e.source.id == Nodeid (1, 1)) return &e;
        return nullptr;
    };
    DN_ASSERT (wait_until ([&] {
        std::lock_guard lock (m);
        return from_a () != nullptr;
    }));

    std::lock_guard lock (m);
    const Event &e = *from_a ();
    // The record says where it came from and what it was about, and the
    // node that raised it is A even though B is displaying it.
    DN_ASSERT (e.id == (EventId { 4, 10 }));
    DN_ASSERT_EQ (e.source.id, Nodeid (1, 1));
    DN_ASSERT_EQ (e.source.name, std::string ("NODEA"));
    DN_ASSERT_EQ (e.entity.as_string (), std::string ("MUL-0"));
    // The node parameter came back as a two byte address and a name, and
    // the definition table is what turns 1026 into "1.2".
    std::vector<std::string> shown = e.params.format (find_event (e.id)->params);
    DN_ASSERT_EQ (shown.size (), 1u);
    DN_ASSERT_EQ (shown[0], std::string ("Adjacent node = 1.2 (NODEB)"));
    // A's monitor filter picked it, so the record asks for the monitor at
    // the far end and for nothing else.  That is what routes it to B's
    // monitor sink rather than to a console B does not have.
    DN_ASSERT (e.monitor);
    DN_ASSERT (!e.console);
    DN_ASSERT (!e.file);

    p.stop ();
}

DN_TEST (eventlog, events_queue_while_the_sink_is_unreachable)
{
    // No circuit at all, so the connection can never be made.  The events
    // must not be lost or throw; they wait.
    Config cfg = Config::from_string (
        "routing 1.1 --type l1router\nnode 1.1 NODEA\nnode 1.2 NODEB\n"
        "logging console --sink-node NODEB --events 4.*\n");
    Node n (cfg);
    n.event_logger ()->start ();

    RemoteSink *s = n.event_logger ()->remote_sink ("NODEB");
    DN_ASSERT (s != nullptr);
    for (int i = 0; i < 10; ++i) {
        Event e { { 4, 10 }, nice::Entity::make_circuit ("MUL-0") };
        n.logevent (e);
    }
    DN_ASSERT_EQ (s->queued (), 10u);
    n.event_logger ()->stop ();
}

DN_TEST (eventlog, a_full_queue_reports_the_loss)
{
    Config cfg = Config::from_string (
        "routing 1.1 --type l1router\nnode 1.1 NODEA\nnode 1.2 NODEB\n"
        "logging console --sink-node NODEB --events 4.*\n");
    Node n (cfg);
    n.event_logger ()->start ();

    RemoteSink *s = n.event_logger ()->remote_sink ("NODEB");
    for (int i = 0; i < 200; ++i) {
        Event e { { 4, 10 }, nice::Entity::make_circuit ("MUL-0") };
        n.logevent (e);
    }
    // The queue is bounded, and the last entry says records were lost
    // rather than the overflow being silent.
    DN_ASSERT_EQ (s->queued (), 50u);
    n.event_logger ()->stop ();
}
