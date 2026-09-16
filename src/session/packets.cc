#include "decnet/session/packets.h"

namespace decnet::session {

// --------------------------------------------------------------- EndUser

bool EndUser::valid () const noexcept
{
    switch (fmt) {
    case by_number: return num != 0;
    case by_name:   return !name.empty () && name.size () <= 16;
    case by_uic:    return !name.empty () && name.size () <= 16;
    }
    return false;
}

std::string EndUser::str () const
{
    switch (fmt) {
    case by_number: return std::to_string (num);
    case by_name:   return name;
    case by_uic:
        return "[" + std::to_string (group) + "," + std::to_string (user)
             + "]" + name;
    }
    return "?";
}

void EndUserField::encode (Encoder &e, const value_type &v)
{
    e.byte (static_cast<std::uint8_t> (v.fmt));
    e.byte (v.num);
    if (v.fmt == EndUser::by_uic) {
        e.uint (v.group, 2);
        e.uint (v.user, 2);
    }
    if (v.fmt == EndUser::by_name || v.fmt == EndUser::by_uic) {
        if (v.name.size () > 16)
            throw FieldOverflow ("end user name longer than 16 characters");
        e.byte (static_cast<std::uint8_t> (v.name.size ()));
        e.raw (ByteView (reinterpret_cast<const std::uint8_t *> (v.name.data ()),
                         v.name.size ()));
    }
}

void EndUserField::decode (Decoder &d, value_type &v)
{
    v = EndUser {};
    std::uint8_t f = d.byte ();
    if (f > 2) throw DecodeError ("invalid end user format " + std::to_string (f));
    v.fmt = static_cast<EndUser::Format> (f);
    v.num = d.byte ();

    if (v.fmt == EndUser::by_uic) {
        v.group = static_cast<std::uint16_t> (d.uint (2));
        v.user  = static_cast<std::uint16_t> (d.uint (2));
    }
    if (v.fmt == EndUser::by_name || v.fmt == EndUser::by_uic) {
        std::size_t n = d.byte ();
        if (n > 16) throw FieldOverflow ("end user name longer than 16");
        ByteView b = d.raw (n);
        v.name.assign (reinterpret_cast<const char *> (b.data ()), b.size ());
    }
    if (!v.valid ())
        throw DecodeError ("invalid end user: " + v.str ());
}

// ----------------------------------------------------------- ConnectData

namespace {

void put_string (Bytes &out, const std::string &s, std::size_t max)
{
    if (s.size () > max)
        throw FieldOverflow ("session control string too long");
    out.push_back (static_cast<std::uint8_t> (s.size ()));
    out.insert (out.end (), s.begin (), s.end ());
}

std::string get_string (Decoder &d, std::size_t max)
{
    std::size_t n = d.byte ();
    if (n > max) throw FieldOverflow ("session control string too long");
    ByteView b = d.raw (n);
    return std::string (reinterpret_cast<const char *> (b.data ()), b.size ());
}

}   // namespace

Bytes ConnectData::encode_message ()
{
    // Flags are derived from the fields present.
    payload.clear ();
    auth = !(rqstrid.empty () && passwrd.empty () && account.empty ());
    if (auth) {
        put_string (payload, rqstrid, 39);
        put_string (payload, passwrd, 39);
        put_string (payload, account, 39);
    }
    userdata = !connectdata.empty ();
    if (userdata) {
        if (connectdata.size () > 16)
            throw FieldOverflow ("connect data longer than 16 bytes");
        payload.push_back (static_cast<std::uint8_t> (connectdata.size ()));
        payload.insert (payload.end (), connectdata.begin (),
                        connectdata.end ());
    }
    return encode ();
}

ConnectData ConnectData::parse_message (ByteView buf)
{
    ConnectData c = parse (buf);
    c.rqstrid.clear ();
    c.passwrd.clear ();
    c.account.clear ();
    c.connectdata.clear ();

    if (c.payload.empty ()) {
        // The flags promised fields that are not there.
        if (c.auth || c.userdata)
            throw MissingData ("session control connect message truncated");
        return c;
    }
    Decoder d (ByteView (c.payload.data (), c.payload.size ()));
    if (c.auth) {
        c.rqstrid = get_string (d, 39);
        c.passwrd = get_string (d, 39);
        c.account = get_string (d, 39);
    }
    if (c.userdata) {
        std::size_t n = d.byte ();
        if (n > 16) throw FieldOverflow ("connect data longer than 16 bytes");
        ByteView b = d.raw (n);
        c.connectdata.assign (b.begin (), b.end ());
    }
    // Anything left over is not an error in PyDECnet, only a debug note.
    return c;
}

}   // namespace decnet::session
