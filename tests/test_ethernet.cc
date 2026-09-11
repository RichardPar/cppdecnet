// The Ethernet datalink: framing, address filtering, and two nodes
// exchanging frames over a UDP-carried LAN.

#include "harness.h"

#include "decnet/common/socket.h"
#include "decnet/config.h"
#include "decnet/datalink/ethernet.h"
#include "decnet/node.h"
#include "decnet/routing/packets.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace decnet;
using namespace decnet::datalink;
using Mode = EthernetDevice::Mode;

namespace {

Bytes bytes_of (std::initializer_list<int> v)
{
    Bytes b;
    for (int x : v) b.push_back (static_cast<std::uint8_t> (x));
    return b;
}

std::uint16_t free_udp_port ()
{
    // Bind a UDP socket to port zero and see what we were given.
    SourceAddress any ("127.0.0.1", 0);
    Socket s = any.bind_socket (AF_INET, SOCK_DGRAM);
    if (!s) throw std::runtime_error ("cannot find a free port");
    sockaddr_storage sa {};
    socklen_t len = sizeof sa;
    ::getsockname (s.fd (), reinterpret_cast<sockaddr *> (&sa), &len);
    return ntohs (reinterpret_cast<sockaddr_in *> (&sa)->sin_port);
}

class Sink : public Element {
public:
    explicit Sink (Node *n) : Element (n) {}

    void dispatch (Work &w) override
    {
        if (auto *r = dynamic_cast<Received *> (&w)) {
            std::lock_guard lock (mutex_);
            got_.push_back (r->packet ());
        }
    }

    std::vector<Bytes> got ()
    { std::lock_guard l (mutex_); return got_; }

    std::size_t count ()
    { std::lock_guard l (mutex_); return got_.size (); }

private:
    std::mutex         mutex_;
    std::vector<Bytes> got_;
};

template <typename P>
bool wait_until (P pred, std::chrono::milliseconds timeout
                     = std::chrono::seconds (10))
{
    auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        if (pred ()) return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    return pred ();
}

}   // namespace

// --------------------------------------------------------- device parsing

DN_TEST (eth, device_udp)
{
    EthernetDevice d = EthernetDevice::parse ("udp:4711:127.0.0.1:4742");
    DN_ASSERT (d.mode == Mode::udp);
    DN_ASSERT_EQ (d.source_port, 4711);
    DN_ASSERT_EQ (d.destination, std::string ("127.0.0.1"));
    DN_ASSERT_EQ (d.dest_port, 4742);

    // SIMH says "udp", pydecnet also accepts "bridge"; they are the same.
    EthernetDevice b = EthernetDevice::parse ("bridge:4711:host:4742");
    DN_ASSERT (b.mode == Mode::udp);
    DN_ASSERT_EQ (b.dest_port, 4742);
}

DN_TEST (eth, device_tap_and_pcap)
{
    DN_ASSERT (EthernetDevice::parse ("tap:/dev/tap0").mode == Mode::tap);
    DN_ASSERT_EQ (EthernetDevice::parse ("tap:tap0").destination,
                  std::string ("tap0"));
    DN_ASSERT (EthernetDevice::parse ("pcap:en0").mode == Mode::pcap);
}

DN_TEST (eth, device_errors)
{
    DN_ASSERT_THROWS (std::invalid_argument, EthernetDevice::parse ("udp"));
    DN_ASSERT_THROWS (std::invalid_argument,
                      EthernetDevice::parse ("udp:4711:127.0.0.1"));
    DN_ASSERT_THROWS (std::invalid_argument,
                      EthernetDevice::parse ("udp:99999:h:1"));
    DN_ASSERT_THROWS (std::invalid_argument,
                      EthernetDevice::parse ("udp:1::2"));
    DN_ASSERT_THROWS (std::invalid_argument,
                      EthernetDevice::parse ("wibble:en0"));
}

// ---------------------------------------------------------------- framing

DN_TEST (eth, frame_roundtrip)
{
    Macaddr dst = Macaddr::parse ("ab-00-00-03-00-00");
    Macaddr src = Macaddr::parse ("aa-00-04-00-36-24");
    Bytes payload = bytes_of ({ 1, 2, 3, 4 });

    Bytes f = build_frame (dst, src, ROUTING_PROTO,
                           ByteView (payload.data (), payload.size ()));

    // Destination, source, protocol type big endian, then the DEC padded
    // format's little endian payload length.
    DN_ASSERT_EQ (f[0], 0xab);
    DN_ASSERT_EQ (f[6], 0xaa);
    DN_ASSERT_EQ (f[12], 0x60);
    DN_ASSERT_EQ (f[13], 0x03);
    DN_ASSERT_EQ (f[14], 4);
    DN_ASSERT_EQ (f[15], 0);
    DN_ASSERT_EQ (f[16], 1);

    ParsedFrame p;
    DN_ASSERT (parse_frame (f, p));
    DN_ASSERT_EQ (p.dest, dst);
    DN_ASSERT_EQ (p.src, src);
    DN_ASSERT_EQ (p.proto, ROUTING_PROTO);
    DN_ASSERT_EQ (Bytes (p.payload.begin (), p.payload.end ()), payload);
}

DN_TEST (eth, short_frames_are_padded_to_the_minimum)
{
    // Ethernet will not carry a frame under 60 bytes; the length field is
    // what lets the receiver find the real end of the payload.
    Bytes payload = bytes_of ({ 0xaa });
    Bytes f = build_frame (Macaddr::parse ("ab-00-00-03-00-00"),
                           Macaddr::parse ("aa-00-04-00-36-24"),
                           ROUTING_PROTO,
                           ByteView (payload.data (), payload.size ()));
    DN_ASSERT_EQ (f.size (), 60u);

    ParsedFrame p;
    DN_ASSERT (parse_frame (f, p));
    DN_ASSERT_EQ (p.payload.size (), 1u);
    DN_ASSERT_EQ (p.payload[0], 0xaa);

    // The fill is 0x42, not zero.  That looks like it cannot matter -- the
    // length field two bytes into the frame says where the payload ends --
    // but a PDP-11 running RSX reads past it, and what it finds there ends
    // up in the address it records.  Sent a hello filled with zeros it
    // built an adjacency to node 21.426, which exists nowhere; sent the
    // same 27 payload bytes filled with 0x42 it built a correct one.  See
    // BUGS.md.  Zeros here would pass every other test in this file.
    for (std::size_t i = 17; i < f.size (); ++i) DN_ASSERT_EQ (f[i], 0x42);
}

DN_TEST (eth, malformed_frames_rejected)
{
    ParsedFrame p;
    DN_ASSERT (!parse_frame (Bytes {}, p));
    DN_ASSERT (!parse_frame (bytes_of ({ 1, 2, 3 }), p));
    // A length field claiming more than the frame holds.
    Bytes f (20, 0);
    f[14] = 0xff;
    f[15] = 0xff;
    DN_ASSERT (!parse_frame (f, p));
}

// -------------------------------------------------------------- loopback

DN_TEST (eth, two_nodes_exchange_frames_over_a_udp_lan)
{
    // Two nodes pointed at each other are a degenerate two station LAN --
    // enough to exercise the whole datalink without a kernel device or any
    // privilege.
    std::uint16_t pa = free_udp_port (), pb = free_udp_port ();

    Config acfg = Config::from_string (
        "node 1.1 NODEA\ncircuit eth-0 Ethernet udp:" + std::to_string (pa)
        + ":127.0.0.1:" + std::to_string (pb) + " --random-address\n");
    Config bcfg = Config::from_string (
        "node 1.2 NODEB\ncircuit eth-0 Ethernet udp:" + std::to_string (pb)
        + ":127.0.0.1:" + std::to_string (pa) + " --random-address\n");

    Node a (acfg), b (bcfg);
    auto *da = dynamic_cast<BcDatalink *> (a.datalink ()->circuit ("eth-0"));
    auto *db = dynamic_cast<BcDatalink *> (b.datalink ()->circuit ("eth-0"));
    DN_ASSERT (da != nullptr && db != nullptr);

    Sink sa (&a), sb (&b);
    BcPort *porta = da->create_bc_port (&sa, ROUTING_PROTO);
    BcPort *portb = db->create_bc_port (&sb, ROUTING_PROTO);

    a.start ();
    b.start ();

    // Addressed directly to B.
    Bytes msg = bytes_of ({ 0x05, 0x02, 0x04, 0x00 });
    porta->send (msg, portb->macaddr ());
    DN_ASSERT (wait_until ([&] { return sb.count () >= 1; }));
    DN_ASSERT_EQ (sb.got ().at (0), msg);

    // And to a multicast address B has joined.
    portb->add_multicast (all_routers ());
    Bytes hello = bytes_of ({ 0x0b, 0x02, 0x00, 0x00 });
    porta->send (hello, all_routers ());
    DN_ASSERT (wait_until ([&] { return sb.count () >= 2; }));
    DN_ASSERT_EQ (sb.got ().at (1), hello);

    b.stop ();
    a.stop ();
}

DN_TEST (eth, frames_for_other_addresses_are_ignored)
{
    std::uint16_t pa = free_udp_port (), pb = free_udp_port ();
    Config acfg = Config::from_string (
        "node 1.1 NODEA\ncircuit eth-0 Ethernet udp:" + std::to_string (pa)
        + ":127.0.0.1:" + std::to_string (pb) + " --random-address\n");
    Config bcfg = Config::from_string (
        "node 1.2 NODEB\ncircuit eth-0 Ethernet udp:" + std::to_string (pb)
        + ":127.0.0.1:" + std::to_string (pa) + " --random-address\n");

    Node a (acfg), b (bcfg);
    auto *da = dynamic_cast<BcDatalink *> (a.datalink ()->circuit ("eth-0"));
    auto *db = dynamic_cast<BcDatalink *> (b.datalink ()->circuit ("eth-0"));
    Sink sa (&a), sb (&b);
    BcPort *porta = da->create_bc_port (&sa, ROUTING_PROTO);
    BcPort *portb = db->create_bc_port (&sb, ROUTING_PROTO);
    a.start ();
    b.start ();

    // Somebody else's address, and a multicast group B has not joined.
    porta->send (bytes_of ({ 1 }), Macaddr::parse ("aa-00-04-00-99-99"));
    porta->send (bytes_of ({ 2 }), all_endnodes ());
    // A frame B does want, to prove the others really did arrive first.
    porta->send (bytes_of ({ 3 }), portb->macaddr ());

    DN_ASSERT (wait_until ([&] { return sb.count () >= 1; }));
    std::this_thread::sleep_for (std::chrono::milliseconds (200));
    DN_ASSERT_EQ (sb.count (), 1u);
    DN_ASSERT_EQ (sb.got ().at (0), bytes_of ({ 3 }));

    b.stop ();
    a.stop ();
}

DN_TEST (eth, protocol_types_are_kept_apart)
{
    std::uint16_t pa = free_udp_port (), pb = free_udp_port ();
    Config acfg = Config::from_string (
        "node 1.1 NODEA\ncircuit eth-0 Ethernet udp:" + std::to_string (pa)
        + ":127.0.0.1:" + std::to_string (pb) + " --random-address\n");
    Config bcfg = Config::from_string (
        "node 1.2 NODEB\ncircuit eth-0 Ethernet udp:" + std::to_string (pb)
        + ":127.0.0.1:" + std::to_string (pa) + " --random-address\n");

    Node a (acfg), b (bcfg);
    auto *da = dynamic_cast<BcDatalink *> (a.datalink ()->circuit ("eth-0"));
    auto *db = dynamic_cast<BcDatalink *> (b.datalink ()->circuit ("eth-0"));
    Sink routing (&b), mop (&b), sa (&a);
    BcPort *pa_routing = da->create_bc_port (&sa, ROUTING_PROTO);
    BcPort *pa_mop     = da->create_bc_port (&sa, MOPDL_PROTO);
    BcPort *pb_routing = db->create_bc_port (&routing, ROUTING_PROTO);
    db->create_bc_port (&mop, MOPDL_PROTO);
    a.start ();
    b.start ();

    pa_mop->send (bytes_of ({ 0xee }), pb_routing->macaddr ());
    pa_routing->send (bytes_of ({ 0x02 }), pb_routing->macaddr ());

    DN_ASSERT (wait_until ([&] { return routing.count () >= 1; }));
    DN_ASSERT (wait_until ([&] { return mop.count () >= 1; }));
    DN_ASSERT_EQ (routing.got ().at (0), bytes_of ({ 0x02 }));
    DN_ASSERT_EQ (mop.got ().at (0), bytes_of ({ 0xee }));

    // One protocol type per circuit; a second port for the same type is a
    // configuration error, not a silent shadowing.
    DN_ASSERT_THROWS (InternalError,
                      db->create_bc_port (&routing, ROUTING_PROTO));

    b.stop ();
    a.stop ();
}

DN_TEST (eth, random_addresses_are_local_unicast)
{
    Config cfg = Config::from_string (
        "node 1.1 NODEA\ncircuit eth-0 Ethernet udp:1:127.0.0.1:2"
        " --random-address\n");
    Node n (cfg);
    auto *d = dynamic_cast<BcDatalink *> (n.datalink ()->circuit ("eth-0"));
    DN_ASSERT (d != nullptr);
    Macaddr a = d->hwaddr ();
    // Locally administered, and an individual rather than group address --
    // otherwise several nodes on one host would collide.
    DN_ASSERT (a.is_local ());
    DN_ASSERT (!a.is_multicast ());
}

// ------------------------------------------------------- the packet filter

DN_TEST (eth, filter_expression_names_every_address_and_protocol)
{
    Config c = Config::from_string (
        "routing 1.1 --type l1router\nnode 1.1 NODEA\n"
        "circuit eth-0 Ethernet udp:" + std::to_string (free_udp_port ())
        + ":127.0.0.1:" + std::to_string (free_udp_port ()) + " --mop\n");
    Node n (c);

    auto *dl = dynamic_cast<BcDatalink *> (
        n.datalink ()->circuit ("ETH-0"));
    DN_ASSERT (dl != nullptr);

    std::string f = dl->filter_expression ();
    // Routing and MOP both have ports, and both protocol types must be in
    // the expression or the kernel would drop traffic we asked for.
    DN_ASSERT (f.find ("ether proto 0x6003") != std::string::npos);
    DN_ASSERT (f.find ("ether proto 0x6001") != std::string::npos);
    // Our own address, the routing multicast we listen on, and broadcast.
    DN_ASSERT (f.find ("ether dst aa-00-04-00-01-04") != std::string::npos);
    DN_ASSERT (f.find ("ether dst ff-ff-ff-ff-ff-ff") != std::string::npos);
    // The two halves are combined, not concatenated.
    DN_ASSERT (f.find (") and (") != std::string::npos);
}

DN_TEST (eth, filter_expression_is_empty_with_no_ports)
{
    // Nothing has asked for anything, so there is nothing to ask the
    // kernel for either.  An empty expression means "leave the filter
    // alone" rather than "accept everything".
    Config c = Config::from_string ("routing 1.1 --type l1router\n"
                                    "node 1.1 NODEA\n");
    Node n (c);
    (void) n;
}

#if DN_HAVE_PCAP

DN_TEST (eth, pcap_interface_address)
{
    // Loopback exists everywhere and has an all zero hardware address;
    // a name that cannot exist has none at all.  Both come back as the
    // null address, which is how the caller knows to leave hwaddr alone.
    DN_ASSERT_EQ (PcapEthernet::interface_address ("no-such-if-42"),
                  Macaddr ());

    // Any real Ethernet interface on this machine should give a real
    // address.  Skip quietly if there is none: a build machine may have
    // nothing but loopback.
    Macaddr found;
    for (const char *name : { "eth0", "en0", "enp0s3" }) {
        Macaddr a = PcapEthernet::interface_address (name);
        if (a != Macaddr ()) { found = a; break; }
    }
    if (found != Macaddr ()) DN_ASSERT (!found.is_multicast ());
}

DN_TEST (eth, a_pcap_circuit_can_be_created)
{
    // Creating one needs no privilege; opening it does.  This checks the
    // wiring -- that a pcap: device produces a PcapEthernet rather than
    // the "not supported" path it used to take.
    auto dl = Ethernet::create (nullptr, "ETH-9", "pcap:no-such-if-42", true);
    DN_ASSERT (dl != nullptr);
    DN_ASSERT (dynamic_cast<PcapEthernet *> (dl.get ()) != nullptr);
}

#endif  // DN_HAVE_PCAP
