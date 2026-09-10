#include "decnet/mop/packets.h"

namespace decnet::mop {

Macaddr console_multicast () { return Macaddr::parse ("ab-00-00-02-00-00"); }
Macaddr loop_multicast ()    { return Macaddr::parse ("cf-00-00-00-00-00"); }

// ----------------------------------------------------------- SoftwareId

std::string SoftwareId::str () const
{
    if (!is_code) return text;
    switch (code) {
    case  0: return "(no software id)";
    case -1: return "(maintenance system)";
    case -2: return "(operating system)";
    }
    return "(unknown code " + std::to_string (code) + ")";
}

void SoftwareIdField::encode (Encoder &e, const value_type &v)
{
    if (v.is_code) {
        if (v.code < -2 || v.code > 0)
            throw FieldOverflow ("software id code outside -2..0");
        e.byte (static_cast<std::uint8_t> (v.code));
        return;
    }
    if (v.text.size () > 127)
        throw FieldOverflow ("software id longer than 127 characters");
    e.byte (static_cast<std::uint8_t> (v.text.size ()));
    e.raw (ByteView (reinterpret_cast<const std::uint8_t *> (v.text.data ()),
                     v.text.size ()));
}

void SoftwareIdField::decode (Decoder &d, value_type &v)
{
    v = SoftwareId {};
    std::uint8_t first = d.byte ();
    auto signed_first = static_cast<std::int8_t> (first);
    if (signed_first <= 0) {
        // A length of zero is the "no software id" code, not an empty
        // string, which is why this is signed.
        v.is_code = true;
        v.code = signed_first;
        return;
    }
    ByteView b = d.raw (first);
    v.text.assign (reinterpret_cast<const char *> (b.data ()), b.size ());
}

// ---------------------------------------------------------------- SysId

std::vector<std::string> SysId::services () const
{
    std::vector<std::string> out;
    if (loop)     out.push_back ("loop");
    if (dump)     out.push_back ("dump");
    if (ploader)  out.push_back ("primary loader");
    if (sloader)  out.push_back ("secondary loader");
    if (boot)     out.push_back ("boot");
    if (carrier)  out.push_back ("console carrier");
    if (counters) out.push_back ("counters");
    return out;
}

// --------------------------------------------------------- registration

void register_mop_packets ()
{
    auto &idx = MopPacketBase::raw_index ();
    idx.add (REQUEST_ID,       &RequestId::make,       "RequestId");
    idx.add (SYSTEM_ID,        &SysId::make,           "SysId");
    idx.add (REQUEST_COUNTERS, &RequestCounters::make, "RequestCounters");
    idx.add (COUNTERS,         &Counters::make,        "Counters");
    idx.add (CONSOLE_REQUEST,  &ConsoleRequest::make,  "ConsoleRequest");
    idx.add (CONSOLE_RELEASE,  &ConsoleRelease::make,  "ConsoleRelease");
    idx.add (CONSOLE_COMMAND,  &ConsoleCommand::make,  "ConsoleCommand");
    idx.add (CONSOLE_RESPONSE, &ConsoleResponse::make, "ConsoleResponse");
    // PORT: the load and dump messages (codes 0 to 4, 6, 8, 10) are not
    // defined.  pydecnet does not implement load or dump either.
}

}   // namespace decnet::mop
