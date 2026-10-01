// dap/messages.cc -- Data Access Protocol messages.
//
// Port of dap_packets.py.  Written out by hand rather than with the packet
// layout framework, because DAP's truncation and menu rules do not fit it:
// see the header.

#include "decnet/dap/messages.h"

#include "decnet/common/exceptions.h"

#include <algorithm>

namespace decnet::dap {

// ==================================================================== Ext

unsigned Ext::field (unsigned lo, unsigned width) const
{
    unsigned v = 0;
    for (unsigned i = 0; i < width; ++i)
        if ((*this)[lo + i]) v |= 1u << i;
    return v;
}

void Ext::encode (Bytes &out, unsigned maxlen) const
{
    unsigned top = 0;
    for (unsigned i = 0; i < N; ++i) if (b_[i]) top = i;
    unsigned n = top / 7 + 1;
    if (n > maxlen)
        throw FieldOverflow ("EX field needs " + std::to_string (n)
                             + " bytes, at most " + std::to_string (maxlen));
    for (unsigned i = 0; i < n; ++i) {
        std::uint8_t byte = 0;
        for (unsigned j = 0; j < 7; ++j)
            if (b_[i * 7 + j]) byte |= static_cast<std::uint8_t> (1u << j);
        if (i + 1 < n) byte |= 0x80;
        out.push_back (byte);
    }
}

// ================================================================= Reader

// Reads fields in order.  Once the message runs out, every later field
// takes its default: DAP allows a sender to stop after the last field
// that differs from its default.  A field cut off part way is an error.
class Reader {
public:
    explicit Reader (ByteView b) : b_ (b) {}

    bool done () const noexcept { return pos_ >= b_.size (); }

    std::uint64_t b (unsigned n, std::uint64_t dflt = 0)
    {
        if (done ()) return dflt;
        need (n);
        std::uint64_t v = 0;
        for (unsigned i = 0; i < n; ++i)
            v |= static_cast<std::uint64_t> (b_[pos_++]) << (8 * i);
        return v;
    }

    Ext ex (unsigned maxlen, Ext dflt = {})
    {
        if (done ()) return dflt;
        Ext e;
        for (unsigned i = 0;; ++i) {
            if (i >= maxlen)
                throw FieldOverflow ("EX field longer than "
                                     + std::to_string (maxlen) + " bytes");
            need (1);
            std::uint8_t byte = b_[pos_++];
            for (unsigned j = 0; j < 7; ++j)
                if (byte & (1u << j) && i * 7 + j < Ext::N)
                    e.b_.set (i * 7 + j);
            if (!(byte & 0x80)) break;
        }
        return e;
    }

    // I-n: a count byte, then that many bytes.
    Bytes image (unsigned maxlen)
    {
        if (done ()) return {};
        std::uint8_t n = b_[pos_++];
        if (n > maxlen)
            throw FieldOverflow ("image field of " + std::to_string (n)
                                 + " bytes, at most " + std::to_string (maxlen));
        need (n);
        Bytes v (b_.begin () + static_cast<std::ptrdiff_t> (pos_),
                 b_.begin () + static_cast<std::ptrdiff_t> (pos_ + n));
        pos_ += n;
        return v;
    }

    std::string text (unsigned maxlen)
    {
        Bytes v = image (maxlen);
        return std::string (v.begin (), v.end ());
    }

    // An integer sent as an image field, least significant byte first.
    std::uint64_t image_int (unsigned maxlen)
    {
        Bytes v = image (maxlen);
        if (v.size () > 8) throw FieldOverflow ("integer too large");
        std::uint64_t r = 0;
        for (std::size_t i = 0; i < v.size (); ++i)
            r |= static_cast<std::uint64_t> (v[i]) << (8 * i);
        return r;
    }

    // AV-n: exactly n bytes of text, NUL padded.
    std::string fixed (unsigned n)
    {
        if (done ()) return {};
        need (n);
        std::string s (b_.begin () + static_cast<std::ptrdiff_t> (pos_),
                       b_.begin () + static_cast<std::ptrdiff_t> (pos_ + n));
        pos_ += n;
        s.erase (std::find (s.begin (), s.end (), '\0'), s.end ());
        return s;
    }

    Bytes rest ()
    {
        Bytes v (b_.begin () + static_cast<std::ptrdiff_t> (std::min (pos_, b_.size ())),
                 b_.end ());
        pos_ = b_.size ();
        return v;
    }

private:
    void need (std::size_t n) const
    {
        if (pos_ + n > b_.size ())
            throw MissingData ("DAP field cut off: need "
                               + std::to_string (n) + " byte(s)");
    }

    ByteView    b_;
    std::size_t pos_ = 0;
};

// ================================================================= Writer

namespace {

void put_b (Bytes &o, std::uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; ++i)
        o.push_back (static_cast<std::uint8_t> (v >> (8 * i)));
}

void put_image (Bytes &o, ByteView v, unsigned maxlen)
{
    if (v.size () > maxlen)
        throw FieldOverflow ("image field of " + std::to_string (v.size ())
                             + " bytes, at most " + std::to_string (maxlen));
    o.push_back (static_cast<std::uint8_t> (v.size ()));
    o.insert (o.end (), v.begin (), v.end ());
}

void put_text (Bytes &o, const std::string &s, unsigned maxlen)
{
    put_image (o, ByteView (reinterpret_cast<const std::uint8_t *> (s.data ()),
                            s.size ()), maxlen);
}

void put_image_int (Bytes &o, std::uint64_t v, unsigned maxlen)
{
    Bytes b;
    while (v) { b.push_back (static_cast<std::uint8_t> (v)); v >>= 8; }
    put_image (o, b, maxlen);
}

void put_fixed (Bytes &o, const std::string &s, unsigned n)
{
    if (s.size () > n) throw FieldOverflow ("text longer than "
                                            + std::to_string (n) + " bytes");
    o.insert (o.end (), s.begin (), s.end ());
    o.insert (o.end (), n - s.size (), 0);
}

// ------------------------------------------------------------- per message

Config decode_config (Reader &r)
{
    Config m;
    m.bufsiz  = static_cast<std::uint16_t> (r.b (2));
    m.ostype  = static_cast<std::uint8_t> (r.b (1));
    m.filesys = static_cast<std::uint8_t> (r.b (1));
    for (auto &v : m.version) v = static_cast<std::uint8_t> (r.b (1));
    m.syscap  = r.ex (12);
    return m;
}

void encode_body (Bytes &o, const Config &m)
{
    put_b (o, m.bufsiz, 2);
    put_b (o, m.ostype, 1);
    put_b (o, m.filesys, 1);
    for (auto v : m.version) put_b (o, v, 1);
    m.syscap.encode (o, 12);
}

Attributes decode_attributes (Reader &r)
{
    using A = Attributes;
    A m;
    m.menu = r.ex (6);
    auto has = [&] (unsigned bit) { return m.menu[bit]; };
    if (has (A::m_datatype)) m.datatype = r.ex (2, m.datatype);
    if (has (A::m_org))      m.org = static_cast<std::uint8_t> (r.b (1));
    if (has (A::m_rfm))      m.rfm = static_cast<std::uint8_t> (r.b (1, A::fb_fix));
    if (has (A::m_rat))      m.rat = r.ex (3);
    if (has (A::m_bls))      m.bls = static_cast<std::uint16_t> (r.b (2, 512));
    if (has (A::m_mrs))      m.mrs = static_cast<std::uint16_t> (r.b (2));
    if (has (A::m_alq))      m.alq = r.image_int (5);
    if (has (A::m_bks))      m.bks = static_cast<std::uint8_t> (r.b (1));
    if (has (A::m_fsz))      m.fsz = static_cast<std::uint8_t> (r.b (1));
    if (has (A::m_mrn))      m.mrn = r.image_int (5);
    if (has (A::m_runsys))   m.runsys = r.text (40);
    if (has (A::m_deq))      m.deq = static_cast<std::uint16_t> (r.b (2));
    if (has (A::m_fop))      m.fop = r.ex (6);
    if (has (A::m_bsz))      m.bsz = static_cast<std::uint8_t> (r.b (1, 8));
    if (has (A::m_dev))      m.dev = r.ex (6);
    // PyDECnet's layout omits SDC; without it a server that sends one
    // would have every later field misread.
    if (has (A::m_sdc))      m.sdc = r.ex (6);
    if (has (A::m_lrl))      m.lrl = static_cast<std::uint16_t> (r.b (2));
    if (has (A::m_hbk))      m.hbk = r.image_int (5);
    if (has (A::m_ebk))      m.ebk = r.image_int (5);
    if (has (A::m_ffb))      m.ffb = static_cast<std::uint16_t> (r.b (2));
    if (has (A::m_sbn))      m.sbn = r.image_int (5);
    return m;
}

void encode_body (Bytes &o, const Attributes &m)
{
    using A = Attributes;
    m.menu.encode (o, 6);
    auto has = [&] (unsigned bit) { return m.menu[bit]; };
    if (has (A::m_datatype)) m.datatype.encode (o, 2);
    if (has (A::m_org))      put_b (o, m.org, 1);
    if (has (A::m_rfm))      put_b (o, m.rfm, 1);
    if (has (A::m_rat))      m.rat.encode (o, 3);
    if (has (A::m_bls))      put_b (o, m.bls, 2);
    if (has (A::m_mrs))      put_b (o, m.mrs, 2);
    if (has (A::m_alq))      put_image_int (o, m.alq, 5);
    if (has (A::m_bks))      put_b (o, m.bks, 1);
    if (has (A::m_fsz))      put_b (o, m.fsz, 1);
    if (has (A::m_mrn))      put_image_int (o, m.mrn, 5);
    if (has (A::m_runsys))   put_text (o, m.runsys, 40);
    if (has (A::m_deq))      put_b (o, m.deq, 2);
    if (has (A::m_fop))      m.fop.encode (o, 6);
    if (has (A::m_bsz))      put_b (o, m.bsz, 1);
    if (has (A::m_dev))      m.dev.encode (o, 6);
    if (has (A::m_sdc))      m.sdc.encode (o, 6);
    if (has (A::m_lrl))      put_b (o, m.lrl, 2);
    if (has (A::m_hbk))      put_image_int (o, m.hbk, 5);
    if (has (A::m_ebk))      put_image_int (o, m.ebk, 5);
    if (has (A::m_ffb))      put_b (o, m.ffb, 2);
    if (has (A::m_sbn))      put_image_int (o, m.sbn, 5);
}

Access decode_access (Reader &r)
{
    Access m;
    m.accfunc  = static_cast<std::uint8_t> (r.b (1));
    m.accopt   = r.ex (5);
    m.filespec = r.text (255);
    m.fac      = r.ex (3, m.fac);
    m.shr      = r.ex (3, m.shr);
    m.display  = r.ex (4);
    m.password = r.text (40);
    return m;
}

void encode_body (Bytes &o, const Access &m)
{
    // Every field, as PyDECnet sends it: some servers are not keen on the
    // shortened form.
    put_b (o, m.accfunc, 1);
    m.accopt.encode (o, 5);
    put_text (o, m.filespec, 255);
    m.fac.encode (o, 3);
    m.shr.encode (o, 3);
    m.display.encode (o, 4);
    if (!m.password.empty ()) put_text (o, m.password, 40);
}

Control decode_control (Reader &r)
{
    Control m;
    m.ctlfunc = static_cast<std::uint8_t> (r.b (1, Control::get));
    m.menu = r.ex (4);
    if (m.menu[Control::m_rac]) m.rac = static_cast<std::uint8_t> (r.b (1));
    if (m.menu[Control::m_key]) m.key = r.image (255);
    if (m.menu[Control::m_krf]) m.krf = static_cast<std::uint8_t> (r.b (1));
    if (m.menu[Control::m_rop]) m.rop = r.ex (6);
    return m;
}

void encode_body (Bytes &o, const Control &m)
{
    put_b (o, m.ctlfunc, 1);
    m.menu.encode (o, 4);
    if (m.menu[Control::m_rac]) put_b (o, m.rac, 1);
    if (m.menu[Control::m_key]) put_image (o, m.key, 255);
    if (m.menu[Control::m_krf]) put_b (o, m.krf, 1);
    if (m.menu[Control::m_rop]) m.rop.encode (o, 6);
}

AccessComplete decode_access_complete (Reader &r)
{
    AccessComplete m;
    m.cmpfunc = static_cast<std::uint8_t> (r.b (1));
    m.fop     = r.ex (6);
    m.check   = static_cast<std::uint16_t> (r.b (2));
    return m;
}

void encode_body (Bytes &o, const AccessComplete &m)
{
    put_b (o, m.cmpfunc, 1);
    m.fop.encode (o, 6);
    put_b (o, m.check, 2);
}

Status decode_status (Reader &r)
{
    Status m;
    auto v = static_cast<unsigned> (r.b (2));
    m.miccode = v & 07777;
    m.maccode = v >> 12;
    m.rest = r.rest ();
    return m;
}

void encode_body (Bytes &o, const Status &m)
{
    put_b (o, (m.miccode & 07777) | ((m.maccode & 017) << 12), 2);
    o.insert (o.end (), m.rest.begin (), m.rest.end ());
}

DateTime decode_date_time (Reader &r)
{
    DateTime m;
    m.menu = r.ex (6);
    if (m.menu[DateTime::m_cdt]) m.cdt = r.fixed (18);
    if (m.menu[DateTime::m_rdt]) m.rdt = r.fixed (18);
    if (m.menu[DateTime::m_edt]) m.edt = r.fixed (18);
    if (m.menu[DateTime::m_rvn]) m.rvn = static_cast<std::uint16_t> (r.b (2));
    // DAP 7 adds backup and other dates after these; they are not used.
    return m;
}

void encode_body (Bytes &o, const DateTime &m)
{
    m.menu.encode (o, 6);
    if (m.menu[DateTime::m_cdt]) put_fixed (o, m.cdt, 18);
    if (m.menu[DateTime::m_rdt]) put_fixed (o, m.rdt, 18);
    if (m.menu[DateTime::m_edt]) put_fixed (o, m.edt, 18);
    if (m.menu[DateTime::m_rvn]) put_b (o, m.rvn, 2);
}

Protection decode_protection (Reader &r)
{
    Protection m;
    m.menu = r.ex (6);
    if (m.menu[Protection::m_owner]) m.owner = r.text (40);
    if (m.menu[Protection::m_sys])   m.sys = r.ex (3);
    if (m.menu[Protection::m_own])   m.own = r.ex (3);
    if (m.menu[Protection::m_grp])   m.grp = r.ex (3);
    if (m.menu[Protection::m_wld])   m.wld = r.ex (3);
    return m;
}

void encode_body (Bytes &o, const Protection &m)
{
    m.menu.encode (o, 6);
    if (m.menu[Protection::m_owner]) put_text (o, m.owner, 40);
    if (m.menu[Protection::m_sys])   m.sys.encode (o, 3);
    if (m.menu[Protection::m_own])   m.own.encode (o, 3);
    if (m.menu[Protection::m_grp])   m.grp.encode (o, 3);
    if (m.menu[Protection::m_wld])   m.wld.encode (o, 3);
}

Name decode_name (Reader &r)
{
    Name m;
    m.nametype = r.ex (3);
    m.namespec = r.text (200);
    return m;
}

void encode_body (Bytes &o, const Name &m)
{
    m.nametype.encode (o, 3);
    put_text (o, m.namespec, 200);
}

void encode_body (Bytes &o, const Continue &m) { put_b (o, m.confunc, 1); }
void encode_body (Bytes &, const Ack &) {}

void encode_body (Bytes &o, const Data &m)
{
    put_image_int (o, m.recnum, 8);
    o.insert (o.end (), m.payload.begin (), m.payload.end ());
}

void encode_body (Bytes &o, const Unknown &m)
{
    o.insert (o.end (), m.body.begin (), m.body.end ());
}

// One message's body, after its header.
Message decode_body (std::uint8_t type, ByteView body)
{
    Reader r (body);
    switch (type) {
    case t_config:          return decode_config (r);
    case t_attributes:      return decode_attributes (r);
    case t_access:          return decode_access (r);
    case t_control:         return decode_control (r);
    case t_continue:        return Continue { static_cast<std::uint8_t> (
                                r.b (1, Continue::resume)) };
    case t_ack:             return Ack {};
    case t_access_complete: return decode_access_complete (r);
    case t_data: {
        Data d;
        d.recnum = r.image_int (8);
        d.payload = r.rest ();
        return d;
    }
    case t_status:          return decode_status (r);
    case t_date_time:       return decode_date_time (r);
    case t_protection:      return decode_protection (r);
    case t_name:            return decode_name (r);
    default:
        return Unknown { type, Bytes (body.begin (), body.end ()) };
    }
}

}   // namespace

// ============================================================== messages

std::optional<std::uint64_t> Attributes::size () const
{
    if (!has (m_ebk)) return std::nullopt;
    if (ebk == 0) return 0;
    // EBK is the block holding the end of file, counted from one; FFB the
    // first free byte in it.
    return (ebk - 1) * 512 + (has (m_ffb) ? ffb : 0);
}

const char *Attributes::rfm_name (std::uint8_t rfm)
{
    switch (rfm) {
    case fb_udf: return "Undefined";
    case fb_fix: return "Fixed";
    case fb_var: return "Variable";
    case fb_vfc: return "VFC";
    case fb_stm: return "Stream";
    case fb_slf: return "Stream_LF";
    case fb_scr: return "Stream_CR";
    default:     return "Unknown";
    }
}

const char *Config::ostype_name (std::uint8_t t)
{
    static const char *const names[] = {
        "Illegal", "RT-11", "RSTS/E", "RSX-11S", "RSX-11M", "RSX-11D",
        "IAS", "VAX/VMS", "TOPS-20", "TOPS-10", "RTS-8", "OS-8",
        "RSX-11M-PLUS", "COPOS/11", "P/OS", "VAXELN", "CP/M", "MS-DOS",
        "ULTRIX-32", "ULTRIX-11"
    };
    if (t < std::size (names)) return names[t];
    if (t == 192) return "PyDECnet";
    return "Unknown";
}

std::string Protection::vms () const
{
    auto one = [] (const Ext &deny) {
        std::string s;
        if (!deny[no_read])  s += 'R';
        if (!deny[no_write]) s += 'W';
        if (!deny[no_exec])  s += 'E';
        if (!deny[no_del])   s += 'D';
        return s;
    };
    return "(" + (menu[m_sys] ? one (sys) : "") + ","
               + (menu[m_own] ? one (own) : "") + ","
               + (menu[m_grp] ? one (grp) : "") + ","
               + (menu[m_wld] ? one (wld) : "") + ")";
}

std::uint8_t type_of (const Message &m)
{
    return std::visit ([] (const auto &v) -> std::uint8_t {
        using T = std::decay_t<decltype (v)>;
        if constexpr (std::is_same_v<T, Config>)          return t_config;
        if constexpr (std::is_same_v<T, Attributes>)      return t_attributes;
        if constexpr (std::is_same_v<T, Access>)          return t_access;
        if constexpr (std::is_same_v<T, Control>)         return t_control;
        if constexpr (std::is_same_v<T, Continue>)        return t_continue;
        if constexpr (std::is_same_v<T, Ack>)             return t_ack;
        if constexpr (std::is_same_v<T, AccessComplete>)  return t_access_complete;
        if constexpr (std::is_same_v<T, Data>)            return t_data;
        if constexpr (std::is_same_v<T, Status>)          return t_status;
        if constexpr (std::is_same_v<T, DateTime>)        return t_date_time;
        if constexpr (std::is_same_v<T, Protection>)      return t_protection;
        if constexpr (std::is_same_v<T, Name>)            return t_name;
        if constexpr (std::is_same_v<T, Unknown>)         return v.type;
    }, m);
}

const char *type_name (std::uint8_t type)
{
    static const char *const names[] = {
        "Unknown", "Configuration", "Attributes", "Access", "Control",
        "Continue Transfer", "Acknowledge", "Access Complete", "Data",
        "Status", "Key Definition", "Allocation Attributes",
        "Summary Attributes", "Date and Time", "Protection", "Name",
        "Access Control List"
    };
    return type < std::size (names) ? names[type] : "Unknown";
}

Bytes encode (const Message &m)
{
    Bytes out;
    out.push_back (type_of (m));
    out.push_back (0);                  // FLAGS: no stream, length or bitcnt
    std::visit ([&] (const auto &v) { encode_body (out, v); }, m);
    return out;
}

std::vector<Message> decode (ByteView buf)
{
    std::vector<Message> out;
    std::size_t pos = 0;
    while (pos < buf.size ()) {
        std::uint8_t type = buf[pos];
        std::size_t hdr = pos + 1;
        std::size_t end = buf.size ();
        if (hdr < buf.size ()) {
            // FLAGS, EX-5.  Only the first byte's bits are defined.
            std::uint8_t flags = buf[hdr++];
            while ((buf[hdr - 1] & 0x80) && hdr < buf.size ()) ++hdr;
            if (flags & 0x01) ++hdr;                    // STREAMID
            if (flags & 0x02) {                         // LENGTH
                if (hdr >= buf.size ()) throw MissingData ("DAP length missing");
                std::size_t len = buf[hdr++];
                if (flags & 0x04) {                     // LEN256
                    if (hdr >= buf.size ())
                        throw MissingData ("DAP length missing");
                    len |= static_cast<std::size_t> (buf[hdr++]) << 8;
                }
                // The length counts what follows the length fields.
                end = hdr + len;
                if (end > buf.size ())
                    throw MissingData ("DAP message shorter than its length");
            }
            if (flags & 0x08) ++hdr;                    // BITCNT
            if (flags & 0x60)
                throw DecodeError ("DAP segmented or system specific "
                                   "messages are not supported");
            if (hdr > end) throw MissingData ("DAP header cut off");
        }
        out.push_back (decode_body (type, buf.subspan (hdr, end - hdr)));
        pos = end;
    }
    return out;
}

}   // namespace decnet::dap
