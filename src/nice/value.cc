#include "decnet/nice/value.h"

#include "decnet/common/exceptions.h"

#include <cstdio>

namespace decnet::nice {

namespace {

// Sign extend an n byte little endian value read as unsigned.
std::int64_t sign_extend (std::uint64_t v, unsigned bytes)
{
    if (bytes >= 8) return static_cast<std::int64_t> (v);
    std::uint64_t sign = std::uint64_t (1) << (8 * bytes - 1);
    if (v & sign) return static_cast<std::int64_t> (v | ~((sign << 1) - 1));
    return static_cast<std::int64_t> (v);
}

void check_bytes (unsigned bytes)
{
    // The low nibble of the type code holds the count, so 1 to 15.
    if (bytes < 1 || bytes > 15)
        throw FieldOverflow ("NICE byte count " + std::to_string (bytes)
                             + " out of range");
}

}   // namespace

// -------------------------------------------------------- construction

Value Value::du (std::uint64_t v, unsigned bytes)
{
    check_bytes (bytes);
    Value r; r.kind_ = Kind::du; r.bytes_ = bytes; r.data_ = v; return r;
}

Value Value::ds (std::int64_t v, unsigned bytes)
{
    check_bytes (bytes);
    Value r; r.kind_ = Kind::ds; r.bytes_ = bytes; r.data_ = v; return r;
}

Value Value::h (std::uint64_t v, unsigned bytes)
{
    check_bytes (bytes);
    Value r; r.kind_ = Kind::h; r.bytes_ = bytes; r.data_ = v; return r;
}

Value Value::o (std::uint64_t v, unsigned bytes)
{
    check_bytes (bytes);
    Value r; r.kind_ = Kind::o; r.bytes_ = bytes; r.data_ = v; return r;
}

Value Value::c (std::uint64_t v, unsigned bytes)
{
    check_bytes (bytes);
    Value r; r.kind_ = Kind::c; r.bytes_ = bytes; r.data_ = v; return r;
}

Value Value::ai (std::string s)
{
    if (s.size () > 255)
        throw FieldOverflow ("NICE ASCII image longer than 255 bytes");
    Value r; r.kind_ = Kind::ai; r.bytes_ = 0; r.data_ = std::move (s);
    return r;
}

Value Value::hi (Bytes b)
{
    if (b.size () > 255)
        throw FieldOverflow ("NICE hex image longer than 255 bytes");
    Value r; r.kind_ = Kind::hi; r.bytes_ = 0; r.data_ = std::move (b);
    return r;
}

Value Value::cm (List items)
{
    if (items.size () > 15)
        throw FieldOverflow ("NICE coded multiple longer than 15 items");
    Value r; r.kind_ = Kind::cm;
    r.bytes_ = static_cast<unsigned> (items.size ());
    r.data_ = std::move (items);
    return r;
}

// -------------------------------------------------------------- queries

std::uint8_t Value::type_code () const noexcept
{
    switch (kind_) {
    case Kind::du: return static_cast<std::uint8_t> (0x00 + bytes_);
    case Kind::ds: return static_cast<std::uint8_t> (0x10 + bytes_);
    case Kind::h:  return static_cast<std::uint8_t> (0x20 + bytes_);
    case Kind::o:  return static_cast<std::uint8_t> (0x30 + bytes_);
    case Kind::ai: return 0x40;
    case Kind::hi: return 0x20;     // the image form of the hex group
    case Kind::c:  return static_cast<std::uint8_t> (0x80 + bytes_);
    case Kind::cm: return static_cast<std::uint8_t> (0xc0 + bytes_);
    }
    return 0;
}

std::uint64_t Value::as_uint () const { return std::get<std::uint64_t> (data_); }
std::int64_t  Value::as_int  () const { return std::get<std::int64_t>  (data_); }
const std::string &Value::as_string () const { return std::get<std::string> (data_); }
const Bytes       &Value::as_bytes  () const { return std::get<Bytes> (data_); }
const Value::List &Value::as_list   () const { return std::get<List> (data_); }

// ------------------------------------------------------------- encoding

void Value::encode (Encoder &e) const
{
    e.byte (type_code ());
    switch (kind_) {
    case Kind::du: case Kind::h: case Kind::o: case Kind::c:
        e.uint (as_uint (), bytes_);
        break;
    case Kind::ds:
        e.uint (static_cast<std::uint64_t> (as_int ()), bytes_);
        break;
    case Kind::ai: {
        const std::string &s = as_string ();
        e.byte (static_cast<std::uint8_t> (s.size ()));
        e.raw (ByteView (reinterpret_cast<const std::uint8_t *> (s.data ()),
                         s.size ()));
        break;
    }
    case Kind::hi: {
        const Bytes &b = as_bytes ();
        e.byte (static_cast<std::uint8_t> (b.size ()));
        e.raw (ByteView (b.data (), b.size ()));
        break;
    }
    case Kind::cm:
        // Each element carries its own type code.
        for (const Value &v : as_list ()) v.encode (e);
        break;
    }
}

Bytes Value::encode () const
{
    Bytes out;
    Encoder e (out);
    encode (e);
    return out;
}

Value Value::decode (Decoder &d)
{
    std::uint8_t code = d.byte ();

    // The coded group: the byte count is five bits, not four.
    if (code & 0x80) {
        unsigned n = code & 0x1f;
        if (n == 0)
            throw DecodeError ("invalid NICE type code "
                               + std::to_string (code));
        if ((code & 0xc0) == 0xc0) {
            List items;
            items.reserve (n);
            for (unsigned i = 0; i < n; ++i) items.push_back (decode (d));
            return cm (std::move (items));
        }
        return c (d.uint (n), n);
    }

    unsigned n = code & 0x0f;
    switch (code & 0xf0) {
    case 0x00:
        if (!n) throw DecodeError ("invalid NICE type code 0x00");
        return du (d.uint (n), n);
    case 0x10:
        if (!n) throw DecodeError ("invalid NICE type code 0x10");
        return ds (sign_extend (d.uint (n), n), n);
    case 0x20:
        if (!n) {
            // The image form: a count byte, then that many bytes.
            std::size_t len = d.byte ();
            ByteView b = d.raw (len);
            return hi (Bytes (b.begin (), b.end ()));
        }
        return h (d.uint (n), n);
    case 0x30:
        if (!n) throw DecodeError ("invalid NICE type code 0x30");
        return o (d.uint (n), n);
    case 0x40: {
        if (n) throw DecodeError ("invalid NICE type code "
                                  + std::to_string (code));
        std::size_t len = d.byte ();
        ByteView b = d.raw (len);
        return ai (std::string (reinterpret_cast<const char *> (b.data ()),
                                b.size ()));
    }
    default:
        throw DecodeError ("invalid NICE type code " + std::to_string (code));
    }
}

Value Value::parse (ByteView buf)
{
    Decoder d (buf);
    Value v = decode (d);
    if (!d.empty ())
        throw ExtraData (std::to_string (d.remaining ())
                         + " byte(s) after NICE value");
    return v;
}

// ----------------------------------------------------------- formatting

std::string Value::format (Labels labels) const
{
    char buf[64];
    switch (kind_) {
    case Kind::du:
        return std::to_string (as_uint ());
    case Kind::ds:
        return std::to_string (as_int ());
    case Kind::h:
        // Full width with leading zeroes, which is what H1.format does:
        // for hex values the width carries information.
        std::snprintf (buf, sizeof buf, "%0*llx", static_cast<int> (bytes_ * 2),
                       static_cast<unsigned long long> (as_uint ()));
        return buf;
    case Kind::o: {
        int digits = static_cast<int> ((bytes_ * 8 + 2) / 3);
        std::snprintf (buf, sizeof buf, "%0*llo", digits,
                       static_cast<unsigned long long> (as_uint ()));
        return buf;
    }
    case Kind::ai:
        return as_string ();
    case Kind::hi: {
        std::string out;
        for (std::size_t i = 0; i < as_bytes ().size (); ++i) {
            if (i) out += '-';
            std::snprintf (buf, sizeof buf, "%02x", as_bytes ()[i]);
            out += buf;
        }
        return out;
    }
    case Kind::c: {
        std::uint64_t v = as_uint ();
        if (v < labels.size () && labels[v]) return labels[v];
        // The fallback when there is no label for the value.
        return "#" + std::to_string (v);
    }
    case Kind::cm: {
        std::string out;
        bool first = true;
        for (const Value &v : as_list ()) {
            if (!first) out += cm_delimiter;
            first = false;
            out += v.format ();
        }
        return out;
    }
    }
    return {};
}

bool operator== (const Value &a, const Value &b)
{
    return a.kind_ == b.kind_ && a.bytes_ == b.bytes_ && a.data_ == b.data_;
}

}   // namespace decnet::nice
