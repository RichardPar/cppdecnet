// tests/test_ddcmp.cc -- DDCMP message framing.
//
// The encoded forms are checked against bytes the Python's own ddcmp module
// produced for the same values, which is the practice the rest of this
// port follows: a format we only agree with ourselves about is not worth
// much.  The vectors were generated with:
//
//     from decnet.ddcmp import StartMsg, AckMsg, DataMsg, ...
//     m = AckMsg (); m.resp = Seq (5); bytes (m.encode ()).hex (' ')

#include "harness.h"

#include "decnet/common/socket.h"
#include "decnet/config.h"
#include "decnet/datalink/ddcmp.h"
#include "decnet/node.h"
#include "decnet/routing/routing.h"

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace decnet;
using namespace decnet::datalink;
using namespace decnet::datalink::ddcmp;

namespace {

Bytes hex (const std::string &s)
{
    Bytes b;
    for (std::size_t i = 0; i + 1 < s.size (); ) {
        if (s[i] == ' ') { ++i; continue; }
        b.push_back (static_cast<std::uint8_t> (
            std::stoul (s.substr (i, 2), nullptr, 16)));
        i += 2;
    }
    return b;
}

Bytes bytes_of (const char *s)
{
    Bytes b;
    for (const char *p = s; *p; ++p)
        b.push_back (static_cast<std::uint8_t> (*p));
    return b;
}

}   // namespace

// ------------------------------------------------- against the Python

DN_TEST (ddcmp, control_messages_match_python)
{
    DN_ASSERT_EQ (make_start ().encode (), hex ("05 06 c0 00 00 01 75 95"));
    DN_ASSERT_EQ (make_stack ().encode (), hex ("05 07 c0 00 00 01 48 55"));
    DN_ASSERT_EQ (make_ack (Seq (5)).encode (),
                  hex ("05 01 00 05 00 01 ec 54"));
    DN_ASSERT_EQ (make_nak (Seq (7), R_CRC).encode (),
                  hex ("05 02 02 07 00 01 08 2c"));
    DN_ASSERT_EQ (make_rep (Seq (9)).encode (),
                  hex ("05 03 00 00 09 01 83 c5"));
}

DN_TEST (ddcmp, a_data_message_matches_python)
{
    Message m = make_data (Seq (1), Seq (0), bytes_of ("hello"));
    DN_ASSERT_EQ (m.encode (),
                  hex ("81 05 00 00 01 01 12 41 68 65 6c 6c 6f d2 34"));
}

DN_TEST (ddcmp, a_maintenance_message_matches_python)
{
    // A maintenance message carries no sequence numbers and always says
    // "no synchronisation needed", which is why its flag byte is c0.
    Message m = make_maintenance (bytes_of ("abc"));
    DN_ASSERT_EQ (m.encode (), hex ("90 03 c0 00 00 01 a4 90 61 62 63 38 97"));
}

// ------------------------------------------------------------ decoding

DN_TEST (ddcmp, headers_decode_to_what_was_encoded)
{
    Message in = make_data (Seq (17), Seq (200), bytes_of ("payload"));
    Bytes wire = in.encode ();

    Message out;
    DN_ASSERT (decode_header (ByteView (wire.data (), wire.size ()), out)
               == HdrError::none);
    DN_ASSERT (out.kind == MsgKind::data);
    DN_ASSERT_EQ (out.num.value (), 17u);
    DN_ASSERT_EQ (out.resp.value (), 200u);
    DN_ASSERT_EQ (out.count, 7u);
}

DN_TEST (ddcmp, every_control_type_survives_a_round_trip)
{
    struct { Message m; MsgKind kind; } cases[] = {
        { make_start (),            MsgKind::start },
        { make_stack (),            MsgKind::stack },
        { make_ack (Seq (3)),       MsgKind::ack   },
        { make_nak (Seq (4), R_BUF), MsgKind::nak  },
        { make_rep (Seq (5)),       MsgKind::rep   },
    };
    for (auto &c : cases) {
        Bytes wire = c.m.encode ();
        Message out;
        DN_ASSERT (decode_header (ByteView (wire.data (), wire.size ()), out)
                   == HdrError::none);
        DN_ASSERT (out.kind == c.kind);
    }
}

DN_TEST (ddcmp, a_damaged_header_is_rejected)
{
    Bytes wire = make_ack (Seq (5)).encode ();
    // Flip a bit in the sequence number; the header CRC must notice.
    wire[3] ^= 0x01;
    Message out;
    DN_ASSERT (decode_header (ByteView (wire.data (), wire.size ()), out)
               == HdrError::bad_crc);

    // And a byte that is not one of the three starts is not a header at
    // all, which is a different answer: on a stream it means "keep
    // looking", not "the link is in trouble".
    Bytes junk = wire;
    junk[0] = 0x42;
    DN_ASSERT (decode_header (ByteView (junk.data (), junk.size ()), out)
               == HdrError::bad_start);

    Bytes runt (4, 0);
    DN_ASSERT (decode_header (ByteView (runt.data (), runt.size ()), out)
               == HdrError::too_short);
}

// ---------------------------------------------------- resynchronisation

DN_TEST (ddcmp, a_receiver_finds_a_header_in_a_stream_of_noise)
{
    // This is what resynchronisation is: a header that passes its own CRC
    // is a frame start, and nothing else is trusted -- least of all a
    // length field, which after a loss of sync came out of the noise.
    Bytes stream;
    const std::uint8_t noise[] = { 0x00, 0x81, 0x13, 0xff, 0x05, 0x9a, 0x90 };
    for (std::uint8_t b : noise) stream.push_back (b);
    std::size_t at = stream.size ();

    Bytes msg = make_ack (Seq (42)).encode ();
    stream.insert (stream.end (), msg.begin (), msg.end ());

    auto found = find_header (ByteView (stream.data (), stream.size ()));
    DN_ASSERT (found.has_value ());
    DN_ASSERT_EQ (*found, at);

    // Note the noise deliberately contains 0x81, 0x05 and 0x90 -- all
    // three start bytes.  Looking for the byte alone is not enough, and a
    // receiver that stopped there would frame on garbage.
    Message out;
    DN_ASSERT (decode_header (ByteView (stream.data () + *found,
                                        stream.size () - *found), out)
               == HdrError::none);
    DN_ASSERT_EQ (out.resp.value (), 42u);
}

DN_TEST (ddcmp, nothing_is_found_in_pure_noise)
{
    Bytes junk;
    for (int i = 0; i < 64; ++i)
        junk.push_back (static_cast<std::uint8_t> (i * 7 + 3));
    DN_ASSERT (!find_header (ByteView (junk.data (), junk.size ())).has_value ());
}

// -------------------------------------------------------- sequence rules

DN_TEST (ddcmp, sequence_numbers_wrap_at_256)
{
    Seq s (254);
    ++s; DN_ASSERT_EQ (s.value (), 255u);
    ++s; DN_ASSERT_EQ (s.value (), 0u);
    DN_ASSERT_EQ (Seq (250).distance (Seq (3)), 9u);
}

DN_TEST (ddcmp, an_ack_covers_the_window_it_should)
{
    // DDCMP allows up to modulus - 1 outstanding, so this cannot be the
    // RFC 1982 half-modulus comparison the rest of the port uses.  An ack
    // for n covers everything from lo up to and including n.
    DN_ASSERT (Seq (5).in_window (Seq (3), Seq (7)));
    DN_ASSERT (Seq (7).in_window (Seq (3), Seq (7)));     // inclusive at hi
    DN_ASSERT (!Seq (3).in_window (Seq (3), Seq (7)));    // exclusive at lo
    DN_ASSERT (!Seq (8).in_window (Seq (3), Seq (7)));

    // And it works across the wrap, which is the case that matters.
    DN_ASSERT (Seq (2).in_window (Seq (254), Seq (3)));
    DN_ASSERT (!Seq (250).in_window (Seq (254), Seq (3)));
}

// ------------------------------------------------------------- protocol
//
// Two engines wired to each other.  No sockets, no timers and no
// scheduling: the wire is a vector, delivery happens when the test says
// so, and a message is lost by not delivering it.  That makes the hard
// parts -- the startup handshake, a lost message, a NAK, a wrapped
// sequence number -- ordinary straight line tests.

namespace {

struct Peer {
    std::vector<Message> wire;        // what this end has transmitted
    std::vector<Bytes>   up;          // what it handed to its client
    int                  ups = 0, downs = 0;
    double               timer = 0;
    std::unique_ptr<Protocol> proto;

    Peer ()
    {
        Protocol::Hooks h;
        h.send      = [this] (const Message &m) { wire.push_back (m); };
        h.deliver   = [this] (Bytes b) { up.push_back (std::move (b)); };
        h.up        = [this] { ++ups; };
        h.down      = [this] { ++downs; };
        h.set_timer = [this] (double t) { timer = t; };
        proto = std::make_unique<Protocol> (std::move (h));
    }

    bool running () const
    { return proto->state () == Protocol::State::running; }
};

// Deliver everything one end has sent to the other, and clear the wire.
void flush (Peer &from, Peer &to)
{
    std::vector<Message> msgs;
    msgs.swap (from.wire);
    for (const Message &m : msgs) to.proto->receive (m);
}

// Bring a pair up, the way a real link does: both ends send Start.
void bring_up (Peer &a, Peer &b)
{
    a.proto->connected ();
    b.proto->connected ();
    for (int i = 0; i < 6 && !(a.running () && b.running ()); ++i) {
        flush (a, b);
        flush (b, a);
    }
}

}   // namespace

DN_TEST (ddcmp, the_startup_handshake_brings_both_ends_up)
{
    Peer a, b;
    bring_up (a, b);

    DN_ASSERT (a.running ());
    DN_ASSERT (b.running ());
    DN_ASSERT_EQ (a.ups, 1);
    DN_ASSERT_EQ (b.ups, 1);
}

DN_TEST (ddcmp, data_crosses_a_running_link_and_is_acknowledged)
{
    Peer a, b;
    bring_up (a, b);

    a.proto->send (bytes_of ("one"));
    a.proto->send (bytes_of ("two"));
    DN_ASSERT_EQ (a.proto->unacked (), 2u);

    flush (a, b);
    DN_ASSERT_EQ (b.up.size (), 2u);
    DN_ASSERT_EQ (b.up[0], bytes_of ("one"));
    DN_ASSERT_EQ (b.up[1], bytes_of ("two"));

    // B's acknowledgement clears A's window.
    flush (b, a);
    DN_ASSERT_EQ (a.proto->unacked (), 0u);
}

DN_TEST (ddcmp, an_out_of_sequence_message_is_ignored_not_delivered)
{
    Peer a, b;
    bring_up (a, b);

    a.proto->send (bytes_of ("first"));
    a.proto->send (bytes_of ("second"));
    DN_ASSERT_EQ (a.wire.size (), 2u);

    // Lose the first: deliver only the second.  DDCMP does not deliver
    // out of order, and does not NAK either -- the far end finds out from
    // the ack, which still names the message before the gap.
    b.proto->receive (a.wire[1]);
    DN_ASSERT_EQ (b.up.size (), 0u);
    DN_ASSERT_EQ (b.proto->last_received ().value (), 0u);

    // Now the first arrives; both come through in order.
    b.proto->receive (a.wire[0]);
    DN_ASSERT_EQ (b.up.size (), 1u);
    DN_ASSERT_EQ (b.up[0], bytes_of ("first"));
}

DN_TEST (ddcmp, a_nak_retransmits_from_the_error_onward)
{
    Peer a, b;
    bring_up (a, b);

    a.proto->send (bytes_of ("aaa"));
    a.proto->send (bytes_of ("bbb"));
    a.proto->send (bytes_of ("ccc"));
    a.wire.clear ();

    // B says "I have message 1, the next one was damaged".
    a.proto->receive (make_nak (Seq (1), R_CRC));

    // One is acknowledged; two and three go again.
    DN_ASSERT_EQ (a.proto->last_acked ().value (), 1u);
    DN_ASSERT_EQ (a.wire.size (), 2u);
    DN_ASSERT_EQ (a.wire[0].num.value (), 2u);
    DN_ASSERT_EQ (a.wire[1].num.value (), 3u);
    DN_ASSERT_EQ (a.wire[0].payload, bytes_of ("bbb"));
}

DN_TEST (ddcmp, a_timeout_asks_rather_than_retransmits)
{
    Peer a, b;
    bring_up (a, b);

    a.proto->send (bytes_of ("data"));
    a.wire.clear ();
    a.proto->timeout ();

    // Most ARQ protocols resend the data.  DDCMP sends REP -- "where have
    // you got to?" -- so a lost acknowledgement costs one small message
    // instead of the whole window.
    DN_ASSERT_EQ (a.wire.size (), 1u);
    DN_ASSERT (a.wire[0].kind == MsgKind::rep);
    DN_ASSERT_EQ (a.wire[0].num.value (), 1u);
}

DN_TEST (ddcmp, a_rep_is_answered_with_an_ack_or_a_nak)
{
    Peer a, b;
    bring_up (a, b);

    a.proto->send (bytes_of ("x"));
    flush (a, b);
    b.wire.clear ();

    // "Have you got 1?"  Yes -> an ack.
    b.proto->receive (make_rep (Seq (1)));
    DN_ASSERT_EQ (b.wire.size (), 1u);
    DN_ASSERT (b.wire[0].kind == MsgKind::ack);
    b.wire.clear ();

    // "Have you got 5?"  No -> a NAK saying where we really are.
    b.proto->receive (make_rep (Seq (5)));
    DN_ASSERT_EQ (b.wire.size (), 1u);
    DN_ASSERT (b.wire[0].kind == MsgKind::nak);
    DN_ASSERT_EQ (b.wire[0].subtype, R_REP);
    DN_ASSERT_EQ (b.wire[0].resp.value (), 1u);
}

DN_TEST (ddcmp, the_window_holds_back_what_it_cannot_send)
{
    Peer a, b;
    bring_up (a, b);

    // qmax defaults to 7, so the eighth waits.
    for (int i = 0; i < 10; ++i) a.proto->send (bytes_of ("m"));
    DN_ASSERT_EQ (a.proto->unacked (), 7u);
    DN_ASSERT_EQ (a.proto->queued (), 3u);

    // Acknowledging makes room, and the rest go out.
    a.proto->receive (make_ack (Seq (7)));
    DN_ASSERT_EQ (a.proto->queued (), 0u);
    DN_ASSERT_EQ (a.proto->unacked (), 3u);
}

DN_TEST (ddcmp, a_stale_acknowledgement_is_rejected)
{
    Peer a, b;
    bring_up (a, b);
    a.proto->send (bytes_of ("one"));

    // Sequence numbers wrap, so an ack that is merely old looks like one
    // acknowledging almost the whole space.  It must not be believed.
    a.proto->receive (make_ack (Seq (255)));
    DN_ASSERT_EQ (a.proto->last_acked ().value (), 0u);
    DN_ASSERT_EQ (a.proto->unacked (), 1u);
}

DN_TEST (ddcmp, sequence_numbers_survive_the_wrap)
{
    Peer a, b;
    bring_up (a, b);

    // Push 300 messages through, which takes the numbering past 255 twice
    // over.  Nothing may be delivered twice or out of order.
    for (int i = 0; i < 300; ++i) {
        a.proto->send (Bytes { static_cast<std::uint8_t> (i & 0xff) });
        flush (a, b);
        flush (b, a);
    }
    DN_ASSERT_EQ (b.up.size (), 300u);
    for (int i = 0; i < 300; ++i)
        DN_ASSERT_EQ (b.up[static_cast<std::size_t> (i)][0],
                      static_cast<std::uint8_t> (i & 0xff));
    DN_ASSERT_EQ (a.proto->unacked (), 0u);
}

DN_TEST (ddcmp, a_start_while_running_restarts_the_link)
{
    Peer a, b;
    bring_up (a, b);
    DN_ASSERT_EQ (a.downs, 0);

    // The far end rebooted.  We must drop what we thought we knew and
    // tell the layer above, or we would keep numbering from where we were
    // and the far end would reject every message.
    a.proto->receive (make_start ());
    DN_ASSERT_EQ (a.downs, 1);
    DN_ASSERT (a.proto->state () == Protocol::State::istart);
    DN_ASSERT_EQ (a.proto->last_sent ().value (), 0u);
}

DN_TEST (ddcmp, a_payload_crc_error_draws_a_nak)
{
    Peer a, b;
    bring_up (a, b);
    b.wire.clear ();

    Message bad = make_data (Seq (1), Seq (0), bytes_of ("damaged"));
    bad.crcok = false;
    b.proto->receive_error (R_CRC, &bad);

    DN_ASSERT_EQ (b.wire.size (), 1u);
    DN_ASSERT (b.wire[0].kind == MsgKind::nak);
    DN_ASSERT_EQ (b.wire[0].subtype, R_CRC);
}

DN_TEST (ddcmp, sending_while_down_is_discarded_not_queued)
{
    Peer a;
    // Never brought up: a datalink does not buffer for a dead link, and
    // routing will notice the circuit is down and stop offering.
    a.proto->send (bytes_of ("nowhere"));
    DN_ASSERT_EQ (a.proto->queued (), 0u);
    DN_ASSERT_EQ (a.wire.size (), 0u);
}

// --------------------------------------------------------- the datalink
//
// The engine tests above prove the protocol.  These prove the wiring: the
// device string, the factory, and two real nodes brought up over a UDP
// carried DDCMP circuit -- which is the first time the protocol runs on
// actual sockets, on actual threads, driven by actual timers.

namespace {

template <typename P>
bool wait_until (P pred, std::chrono::milliseconds timeout
                     = std::chrono::seconds (15))
{
    auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        if (pred ()) return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    return pred ();
}

}   // namespace

DN_TEST (ddcmp, the_device_string_is_parsed_like_python)
{
    DdcmpDevice d = DdcmpDevice::parse ("udp:1234:localhost:5678");
    DN_ASSERT (d.mode == DdcmpDevice::Mode::udp);
    DN_ASSERT_EQ (d.source_port, 1234u);
    DN_ASSERT_EQ (d.destination, std::string ("localhost"));
    DN_ASSERT_EQ (d.dest_port, 5678u);

    DdcmpDevice t = DdcmpDevice::parse ("tcp:1:host:2");
    DN_ASSERT (t.mode == DdcmpDevice::Mode::tcp);

    DdcmpDevice s = DdcmpDevice::parse ("serial:/dev/ttyUSB0:19200");
    DN_ASSERT (s.mode == DdcmpDevice::Mode::serial);
    DN_ASSERT_EQ (s.destination, std::string ("/dev/ttyUSB0"));
    DN_ASSERT_EQ (s.speed, 19200u);

    // A form nobody meant is refused rather than half understood.
    DN_ASSERT_THROWS (std::invalid_argument, DdcmpDevice::parse ("udp:1:host"));
    DN_ASSERT_THROWS (std::invalid_argument, DdcmpDevice::parse ("smoke:1:h:2"));
}

DN_TEST (ddcmp, two_nodes_come_up_over_a_udp_ddcmp_circuit)
{
    // The whole stack on a DDCMP circuit: the startup handshake runs on
    // real sockets, the routing layer sees the circuit come up, and the
    // adjacency forms.
    int pa = 27801, pb = 27802;
    Config ca = Config::from_string (
        "routing 1.1 --type l1router\nnode 1.1 NODEA\nnode 1.2 NODEB\n"
        "circuit ddc-0 DDCMP udp:" + std::to_string (pa) + ":127.0.0.1:"
        + std::to_string (pb) + " --t3 2\n");
    Config cb = Config::from_string (
        "routing 1.2 --type l1router\nnode 1.2 NODEB\nnode 1.1 NODEA\n"
        "circuit ddc-0 DDCMP udp:" + std::to_string (pb) + ":127.0.0.1:"
        + std::to_string (pa) + " --t3 2\n");

    Node a (ca), b (cb);
    a.start ();
    b.start ();

    DN_ASSERT (wait_until ([&] {
        return a.routing ()->adjacency_count () == 1
            && b.routing ()->adjacency_count () == 1;
    }));

    b.stop ();
    a.stop ();
}

DN_TEST (ddcmp, a_bad_device_string_costs_its_circuit_and_no_more)
{
    // A device nobody can make sense of must cost that circuit and
    // nothing else.  The first version of this test gave an endnode a
    // single bad circuit and expected the node to start; it does not, and
    // it should not -- an endnode with no circuit has nothing to be.  The
    // property worth having is that the *other* circuits survive.
    Config c = Config::from_string (
        "routing 1.1 --type l1router\nnode 1.1 NODEA\n"
        "circuit good-0 Multinet 127.0.0.1:27811:connect\n"
        "circuit bad-0 DDCMP nonsense\n");
    Node n (c);
    n.start ();
    DN_ASSERT_EQ (n.routing ()->circuits ().size (), 1u);
    n.stop ();
}
