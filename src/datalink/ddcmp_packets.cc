// src/datalink/ddcmp_packets.cc -- DDCMP message framing.  Port of the
// message classes in ddcmp.py.

#include "decnet/datalink/ddcmp.h"

#include <cstdio>
#include <string>

namespace decnet::datalink::ddcmp {

namespace {

void put16 (Bytes &b, std::uint16_t v)
{
    b.push_back (static_cast<std::uint8_t> (v & 0xff));
    b.push_back (static_cast<std::uint8_t> (v >> 8));
}

std::uint8_t start_byte (MsgKind k)
{
    switch (k) {
    case MsgKind::data:        return SOH;
    case MsgKind::maintenance: return DLE;
    default:                   return ENQ;
    }
}

std::uint8_t ctl_type (MsgKind k)
{
    switch (k) {
    case MsgKind::ack:   return ACK;
    case MsgKind::nak:   return NAK;
    case MsgKind::rep:   return REP;
    case MsgKind::start: return STRT;
    case MsgKind::stack: return STACK;
    default:             return 0;
    }
}

}   // namespace

Bytes Message::encode_header () const
{
    Bytes h;
    h.reserve (HDRLEN);
    h.push_back (start_byte (kind));

    if (is_data ()) {
        // Fourteen bits of count, then the two flags, little endian.
        std::uint16_t w = static_cast<std::uint16_t> (count & MAXCOUNT);
        if (qsync)  w |= 0x4000;
        if (select) w |= 0x8000;
        put16 (h, w);
    } else {
        h.push_back (ctl_type (kind));
        std::uint8_t f = static_cast<std::uint8_t> (subtype & 0x3f);
        if (qsync)  f |= 0x40;
        if (select) f |= 0x80;
        h.push_back (f);
    }

    h.push_back (resp.value ());
    h.push_back (num.value ());
    h.push_back (addr);

    // The header CRC covers the six bytes before it.
    put16 (h, CRC16::compute (ByteView (h.data (), h.size ())));
    return h;
}

Bytes Message::encode () const
{
    Bytes out = encode_header ();
    if (!is_data ()) return out;

    out.insert (out.end (), payload.begin (), payload.end ());
    put16 (out, CRC16::compute (ByteView (payload.data (), payload.size ())));
    return out;
}

std::string Message::str () const
{
    char buf[128];
    const char *name = "?";
    switch (kind) {
    case MsgKind::data:        name = "data";  break;
    case MsgKind::maintenance: name = "maint"; break;
    case MsgKind::ack:         name = "ack";   break;
    case MsgKind::nak:         name = "nak";   break;
    case MsgKind::rep:         name = "rep";   break;
    case MsgKind::start:       name = "start"; break;
    case MsgKind::stack:       name = "stack"; break;
    }
    if (is_data ())
        std::snprintf (buf, sizeof buf, "%s num=%u resp=%u count=%u%s", name,
                       num.value (), resp.value (), count,
                       crcok ? "" : " BADCRC");
    else if (kind == MsgKind::nak)
        std::snprintf (buf, sizeof buf, "nak resp=%u reason=%u",
                       resp.value (), subtype);
    else
        std::snprintf (buf, sizeof buf, "%s num=%u resp=%u", name,
                       num.value (), resp.value ());
    return buf;
}

// ------------------------------------------------------------- builders

namespace {

Message control (MsgKind k)
{
    Message m;
    m.kind = k;
    m.type = ctl_type (k);
    m.addr = 1;
    // START and STACK say "no synchronisation needed, you may transmit":
    // they are what brings a dead link up, so they cannot depend on the
    // state the link does not have yet.
    if (k == MsgKind::start || k == MsgKind::stack) {
        m.qsync = true;
        m.select = true;
    }
    return m;
}

}   // namespace

Message make_ack (Seq resp)
{
    Message m = control (MsgKind::ack);
    m.resp = resp;
    return m;
}

Message make_nak (Seq resp, std::uint8_t reason)
{
    Message m = control (MsgKind::nak);
    m.resp = resp;
    m.subtype = reason;
    return m;
}

Message make_rep (Seq num)
{
    Message m = control (MsgKind::rep);
    m.num = num;
    return m;
}

Message make_start () { return control (MsgKind::start); }
Message make_stack () { return control (MsgKind::stack); }

Message make_data (Seq num, Seq resp, Bytes payload)
{
    Message m;
    m.kind = MsgKind::data;
    m.num = num;
    m.resp = resp;
    m.addr = 1;
    m.count = static_cast<std::uint16_t> (payload.size ());
    m.payload = std::move (payload);
    return m;
}

Message make_maintenance (Bytes payload)
{
    Message m;
    m.kind = MsgKind::maintenance;
    m.addr = 1;
    m.qsync = true;
    m.select = true;
    m.count = static_cast<std::uint16_t> (payload.size ());
    m.payload = std::move (payload);
    return m;
}

// ------------------------------------------------------------- decoding

HdrError decode_header (ByteView buf, Message &out, bool check)
{
    if (buf.size () < HDRLEN) return HdrError::too_short;

    Message m;
    switch (buf[0]) {
    case SOH: m.kind = MsgKind::data;        break;
    case DLE: m.kind = MsgKind::maintenance; break;
    case ENQ: m.kind = MsgKind::ack;         break;   // refined below
    default:  return HdrError::bad_start;
    }

    if (check) {
        CRC16 c;
        for (std::size_t i = 0; i < HDRLEN; ++i) c.update (buf[i]);
        if (!c.good ()) return HdrError::bad_crc;
    }

    if (m.kind == MsgKind::data || m.kind == MsgKind::maintenance) {
        std::uint16_t w = static_cast<std::uint16_t> (buf[1] | (buf[2] << 8));
        m.count  = static_cast<std::uint16_t> (w & MAXCOUNT);
        m.qsync  = (w & 0x4000) != 0;
        m.select = (w & 0x8000) != 0;
    } else {
        m.type    = buf[1];
        m.subtype = static_cast<std::uint8_t> (buf[2] & 0x3f);
        m.qsync   = (buf[2] & 0x40) != 0;
        m.select  = (buf[2] & 0x80) != 0;
        switch (m.type) {
        case ACK:   m.kind = MsgKind::ack;   break;
        case NAK:   m.kind = MsgKind::nak;   break;
        case REP:   m.kind = MsgKind::rep;   break;
        case STRT:  m.kind = MsgKind::start; break;
        case STACK: m.kind = MsgKind::stack; break;
        default:
            // A control type we do not know.  The header is well formed,
            // so this is a message for a DDCMP we are not, not noise.
            return HdrError::bad_start;
        }
    }

    m.resp = Seq (buf[3]);
    m.num  = Seq (buf[4]);
    m.addr = buf[5];
    out = std::move (m);
    return HdrError::none;
}

std::optional<std::size_t> find_header (ByteView buf)
{
    if (buf.size () < HDRLEN) return std::nullopt;
    for (std::size_t i = 0; i + HDRLEN <= buf.size (); ++i) {
        if (buf[i] != SOH && buf[i] != ENQ && buf[i] != DLE) continue;
        Message m;
        if (decode_header (buf.subspan (i, HDRLEN), m) == HdrError::none)
            return i;
    }
    return std::nullopt;
}

}   // namespace decnet::datalink::ddcmp
