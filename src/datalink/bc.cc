#include "decnet/datalink/bc.h"

#include "decnet/common/exceptions.h"
#include "decnet/common/logging.h"
#include "decnet/node.h"

#include <algorithm>
#include <cstdio>
#include <random>

namespace decnet::datalink {

Macaddr all_routers ()  { return Macaddr::parse ("ab-00-00-03-00-00"); }
Macaddr all_endnodes () { return Macaddr::parse ("ab-00-00-04-00-00"); }

// -------------------------------------------------------------- framing

Bytes build_frame (Macaddr dest, Macaddr src, std::uint16_t proto,
                   ByteView payload, bool pad)
{
    Bytes f;
    f.reserve (std::max (ETH_MIN_FRAME, ETH_HDR_LEN + 2 + payload.size ()));
    ByteView d = dest.view (), s = src.view ();
    f.insert (f.end (), d.begin (), d.end ());
    f.insert (f.end (), s.begin (), s.end ());
    // The protocol type is the one big endian field in the header.
    f.push_back (static_cast<std::uint8_t> (proto >> 8));
    f.push_back (static_cast<std::uint8_t> (proto & 0xff));
    if (pad) {
        // DEC's padded format: a two byte little endian payload length,
        // which is what lets a receiver find the end of a short packet in
        // a frame padded out to the minimum size.
        f.push_back (static_cast<std::uint8_t> (payload.size () & 0xff));
        f.push_back (static_cast<std::uint8_t> (payload.size () >> 8));
    }
    f.insert (f.end (), payload.begin (), payload.end ());
    // Ethernet will not carry a frame shorter than 60 bytes.  Fill with
    // 0x42 rather than zero, which is what pydecnet does (`FILL = b'\x42'
    // * 60` in ethernet.py).
    //
    // This looks like it cannot matter, since the DEC padded format gives
    // the payload length two bytes into the frame and a receiver has no
    // business reading past it.  On 10-Sep-2026 it mattered. Against a
    // PDP-11 running RSX, our hello and pydecnet's were identical for
    // every one of the 27 payload bytes and differed only in this fill.
    // pydecnet's was accepted and ours produced an adjacency to a node
    // that does not exist -- the PDP recorded its designated router as
    // 21.426, from bytes we never sent. So something on that end reads
    // beyond the length it was given, and the fill is what it finds.
    // Matching pydecnet costs nothing and is what interoperates.
    if (f.size () < ETH_MIN_FRAME) f.resize (ETH_MIN_FRAME, 0x42);
    return f;
}

bool parse_frame (ByteView frame, ParsedFrame &out, bool pad)
{
    if (frame.size () < ETH_HDR_LEN + (pad ? 2u : 0u)) return false;

    std::array<std::uint8_t, 6> a {};
    std::copy (frame.begin (), frame.begin () + 6, a.begin ());
    out.dest = Macaddr (a);
    std::copy (frame.begin () + 6, frame.begin () + 12, a.begin ());
    out.src = Macaddr (a);
    out.proto = static_cast<std::uint16_t> ((frame[12] << 8) | frame[13]);

    if (pad) {
        std::size_t len = static_cast<std::size_t> (frame[14])
                        | (static_cast<std::size_t> (frame[15]) << 8);
        if (ETH_HDR_LEN + 2 + len > frame.size ()) return false;
        out.payload = frame.subspan (ETH_HDR_LEN + 2, len);
    } else {
        out.payload = frame.subspan (ETH_HDR_LEN);
    }
    return true;
}

// --------------------------------------------------------------- BcPort

BcPort::BcPort (BcDatalink *dl, Element *owner, std::uint16_t proto, bool pad)
    : Port (dl, owner), proto_ (proto), pad_ (pad), macaddr_ (dl->hwaddr ())
{
}

Macaddr BcPort::macaddr () const { return macaddr_; }

void BcPort::set_macaddr (Macaddr addr)
{
    if (addr.is_multicast ())
        throw std::invalid_argument ("address " + addr.str ()
                                     + " is not an individual address");
    macaddr_ = addr;
    static_cast<BcDatalink *> (datalink_)->filter_changed ();
}

void BcPort::add_multicast (Macaddr addr)
{
    if (!addr.is_multicast ())
        throw std::invalid_argument ("address " + addr.str ()
                                     + " is not a multicast address");
    const auto &b = addr.bytes ();
    multicast_.insert (Bytes (b.begin (), b.end ()));
    DN_TRACE ("multicast address {} added on {}", addr.str (),
              datalink_->name ());
    static_cast<BcDatalink *> (datalink_)->filter_changed ();
}

void BcPort::remove_multicast (Macaddr addr)
{
    const auto &b = addr.bytes ();
    multicast_.erase (Bytes (b.begin (), b.end ()));
    static_cast<BcDatalink *> (datalink_)->filter_changed ();
}

bool BcPort::accepts (Macaddr dest) const
{
    if (promisc_) return true;
    if (dest == macaddr_) return true;
    const auto &b = dest.bytes ();
    return multicast_.count (Bytes (b.begin (), b.end ())) != 0;
}

void BcPort::send (Bytes msg, Macaddr dest)
{
    auto *dl = static_cast<BcDatalink *> (datalink_);
    Bytes frame = build_frame (dest, macaddr_, proto_,
                               ByteView (msg.data (), msg.size ()), pad_);
    counters_.bytes_sent += msg.size ();
    ++counters_.pkts_sent;
    dl->send_frame (frame);
}

void BcPort::send (Bytes)
{
    // A broadcast circuit needs somewhere to send to.  Nothing calls this;
    // the routing sublayer uses the two argument form.
    DN_DEBUG ("send with no destination on broadcast circuit {}",
              datalink_->name ());
}

// ----------------------------------------------------------- BcDatalink

namespace {

Macaddr random_macaddr ()
{
    // A locally administered unicast address: bit 0 of the first byte
    // clear (individual), bit 1 set (local).  pydecnet does the same, so
    // that several nodes sharing one host do not collide.
    static thread_local std::mt19937_64 gen { std::random_device {} () };
    std::uint64_t r = gen ();
    std::array<std::uint8_t, 6> b {};
    for (int i = 0; i < 6; ++i)
        b[static_cast<std::size_t> (i)] =
            static_cast<std::uint8_t> ((r >> (8 * i)) & 0xff);
    b[0] = static_cast<std::uint8_t> ((b[0] & 0xfc) | 0x02);
    return Macaddr (b);
}

}   // namespace

BcDatalink::BcDatalink (Element *owner, std::string name, bool random_address)
    : Datalink (owner, std::move (name))
{
    if (random_address) hwaddr_ = random_macaddr ();
}

BcPort *BcDatalink::create_bc_port (Element *owner, std::uint16_t proto,
                                    bool pad)
{
    for (const auto &p : ports_)
        if (p->proto () == proto)
            throw InternalError ("protocol type already in use on "
                                 + name_);
    ports_.push_back (std::make_unique<BcPort> (this, owner, proto, pad));
    filter_changed ();
    return ports_.back ().get ();
}

Port *BcDatalink::create_port (Element *)
{
    throw InternalError ("broadcast circuit " + name_
                         + " needs a protocol type; use create_bc_port");
}

std::string BcDatalink::filter_expression () const
{
    // "(ether dst A or ether dst B or ...) and (ether proto X or ...)".
    // A promiscuous port wants everything with a protocol type we handle,
    // so the destination half is dropped when there is one.
    std::string protos, dests;
    bool promisc = false;
    std::set<Bytes> seen;

    auto add_dest = [&] (const Bytes &b) {
        if (b.size () != 6 || !seen.insert (b).second) return;
        std::array<std::uint8_t, 6> a {};
        std::copy (b.begin (), b.end (), a.begin ());
        if (!dests.empty ()) dests += " or ";
        dests += "ether dst " + Macaddr (a).str ();
    };

    for (const auto &port : ports_) {
        char buf[32];
        std::snprintf (buf, sizeof buf, "ether proto 0x%04x", port->proto ());
        if (!protos.empty ()) protos += " or ";
        protos += buf;
        if (port->promisc_) promisc = true;
        const auto &m = port->macaddr_.bytes ();
        add_dest (Bytes (m.begin (), m.end ()));
        for (const Bytes &mc : port->multicast_) add_dest (mc);
    }
    if (protos.empty ()) return { };
    // Broadcast is always wanted: a station that does not know our address
    // yet reaches us that way.
    add_dest (Bytes (6, 0xff));

    if (promisc) return "(" + protos + ")";
    return "(" + dests + ") and (" + protos + ")";
}

void BcDatalink::receive_frame (ByteView frame)
{
    // Parse only the header here.  Whether the payload carries a length
    // field depends on the port, and which port this is depends on the
    // protocol type, which is in the header.  Routing and MOP use the DEC
    // padded format; loopback does not.
    ParsedFrame p;
    if (!parse_frame (frame, p, false)) {
        DN_TRACE ("malformed frame on {}, {} bytes", name_, frame.size ());
        return;
    }
    // Our own transmissions come back on a shared medium; ignore them.
    // Each port sends under its own source address, not the circuit's, so
    // all of them have to be checked -- on a real interface every frame we
    // put on the wire is also captured coming off it.
    if (p.src == hwaddr_) return;
    for (const auto &port : ports_)
        if (p.src == port->macaddr_) return;

    for (auto &port : ports_) {
        if (port->proto () != p.proto) continue;
        if (!port->accepts (p.dest)) continue;
        if (port->pad_) {
            // Re-read the payload with its length field.
            ParsedFrame q;
            if (!parse_frame (frame, q, true)) {
                DN_TRACE ("short padded frame on {}", name_);
                return;
            }
            p.payload = q.payload;
        }
        port->counters_.bytes_recv += p.payload.size ();
        ++port->counters_.pkts_recv;
        if (p.dest.is_multicast ()) {
            port->counters_.mcbytes_recv += p.payload.size ();
            ++port->counters_.mcpkts_recv;
        }
        if (node ())
            node ()->add_work (std::make_unique<Received> (
                port->owner (), Bytes (p.payload.begin (), p.payload.end ()),
                p.src));
        return;
    }
}

}   // namespace decnet::datalink
