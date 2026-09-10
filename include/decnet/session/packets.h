// decnet/session/packets.h -- session control message formats.
//
// Port of the packet classes in session.py.  Session control rides inside
// NSP's connect messages: the payload of a Connect Initiate is a session
// control connect message naming the object being asked for, and the
// payload of a Connect Confirm is whatever the object chose to send back.

#ifndef DECNET_SESSION_PACKETS_H
#define DECNET_SESSION_PACKETS_H

#include "decnet/packet/group.h"
#include "decnet/packet/packet.h"

#include <string>

namespace decnet::session {

using namespace decnet::packet;

// Disconnect and reject reason codes.  Ports of the constants at the top
// of session.py.
enum Reason : std::uint16_t {
    NO_OBJ    = 4,    // destination end user does not exist
    BAD_FMT   = 5,    // connect message format error
    BAD_NODE  = 10,   // invalid node name format
    BAD_AUTH  = 34,   // authorisation data not valid
    BAD_ACCT  = 36,   // account not valid
    OBJ_FAIL  = 38,   // object failed
    UNREACH   = 39,   // destination unreachable
    ABORT     = 9     // connection aborted
};

// Who a connection is from or to.  An end user is named in one of three
// ways, and which one is in force is the first byte.  Port of the EndUser
// family; it is written as a codec rather than an indexed packet because
// it appears inline inside another message, not on its own.
struct EndUser {
    enum Format : std::uint8_t {
        by_number = 0,    // an object number
        by_name   = 1,    // a name
        by_uic    = 2     // a name plus a group and user number
    };

    Format        fmt = by_number;
    std::uint8_t  num = 0;
    std::string   name;
    std::uint16_t group = 0, user = 0;

    static EndUser number (std::uint8_t n)
    { EndUser e; e.fmt = by_number; e.num = n; return e; }

    static EndUser named (std::string n)
    { EndUser e; e.fmt = by_name; e.name = std::move (n); return e; }

    // A format 0 end user must have a non-zero number -- zero is not an
    // object number -- and a format 1 must have a name.  pydecnet rejects
    // the invalid combinations, so a message built with one will be
    // refused by a real node.
    bool valid () const noexcept;

    std::string str () const;

    friend bool operator== (const EndUser &, const EndUser &) = default;
};

struct EndUserField {
    using value_type = EndUser;
    static void encode (Encoder &e, const value_type &v);
    static void decode (Decoder &d, value_type &v);
};

// The session control connect message, carried as the payload of an NSP
// Connect Initiate.  Port of session.SessionConnInit.
//
// The optional fields at the end are not described by the layout, because
// whether they are present is decided by flag bits earlier in the message;
// they are unpacked from the payload afterwards, as check() does in the
// Python.
struct ConnectData : Packet<ConnectData, Extra::allow> {
    EndUser      dstname;      // the object being asked for
    EndUser      srcname;      // who is asking
    bool         auth = false;
    bool         userdata = false;
    bool         proxy = false;
    bool         proxy_uic = false;
    bool         reserved = false;
    std::uint8_t scver = 0;
    bool         mbz2 = false;
    Bytes        payload;

    // Unpacked from the payload.
    std::string  rqstrid, passwrd, account;
    Bytes        connectdata;

    static constexpr std::uint8_t SCVER1 = 0;   // Session Control 1.0
    static constexpr std::uint8_t SCVER2 = 1;   // Session Control 2.0

    static constexpr auto layout = fields (
        field<EndUserField> (&ConnectData::dstname, "dstname"),
        field<EndUserField> (&ConnectData::srcname, "srcname"),
        bm<ConnectData> (bmf (&ConnectData::auth,      "auth",      0, 1),
                         bmf (&ConnectData::userdata,  "userdata",  1, 1),
                         bmf (&ConnectData::proxy,     "proxy",     2, 1),
                         bmf (&ConnectData::proxy_uic, "proxy_uic", 3, 1),
                         bmf (&ConnectData::reserved,  "reserved",  4, 1),
                         bmf (&ConnectData::scver,     "scver",     5, 2),
                         bmf (&ConnectData::mbz2,      "mbz2",      7, 1)),
        field<Payload> (&ConnectData::payload, "payload"));

    // Build the wire form, packing the optional fields into the payload
    // and setting the flags that say they are there.
    Bytes encode_message ();

    // Parse, then unpack those fields again.  Throws DecodeError on
    // anything malformed, which the caller turns into a format reject.
    static ConnectData parse_message (ByteView buf);
};

}   // namespace decnet::session

#endif  // DECNET_SESSION_PACKETS_H
