#include "decnet/routing/packets.h"

namespace decnet::routing {

// Register every routing packet class.
//
// In the Python these are class attributes the metaclass picks up as each
// class is defined.  Here they are one explicit function, called once
// before the first lookup, because static initializers in a static library
// only run if the linker had another reason to pull the object file in --
// see DN_PACKET_INDEX_REGISTERED.
//
// The masks matter: the flags byte mixes the packet type with per-packet
// bits, so a class claims every value whose type bits match.
void register_routing_packets ()
{
    auto &idx = RoutingPacketBase::raw_index ();

    idx.add_masked (0x02, 0xc7, &ShortData::make, "ShortData");
    idx.add_masked (0x06, 0xc7, &LongData::make,  "LongData");

    // Point to point init goes through a second lookup on the version byte
    // at offset 6.
    idx.add_nested_masked (0x01, 0x8f, &PtpInit34::index (), "PtpInit34");
    PtpInit34::index ().add (2, &PtpInit::make,  "PtpInit");    // Phase IV
    PtpInit34::index ().add (1, &PtpInit3::make, "PtpInit3");   // Phase III

    // The LAN hellos.
    idx.add_masked (0x0b, 0x8f, &RouterHello::make,  "RouterHello");
    idx.add_masked (0x0d, 0x8f, &EndnodeHello::make, "EndnodeHello");

    idx.add_masked (0x03, 0x8f, &PtpVerify::make, "PtpVerify");
    idx.add_masked (0x05, 0x8f, &PtpHello::make,  "PtpHello");
    idx.add        (0x08,       &NopMsg::make,    "NopMsg");

    // The routing messages.  Both code points go through a second lookup
    // whose key is the checksum residue, because 0x07 is a Phase III
    // message or a Phase IV level 1 message depending only on which seed
    // makes its checksum come out right.
    idx.add_nested_masked (0x07, 0x8f, &P34Routing::index (), "P34Routing");
    P34Routing::index ().add (0xfffe, &L1Routing::make,       "L1Routing");
    P34Routing::index ().add (0xffff, &PhaseIIIRouting::make, "PhaseIIIRouting");

    idx.add_nested_masked (0x09, 0x8f, &P4L2Routing::index (), "P4L2Routing");
    P4L2Routing::index ().add (0xfffe, &L2Routing::make, "L2Routing");
}

const char *ntype_string (unsigned t) noexcept
{
    switch (t) {
    case PHASE2:   return "Phase 2 node";
    case L2ROUTER: return "Area router";
    case L1ROUTER: return "L1 router";
    case ENDNODE:  return "Endnode";
    default:       return "Unknown";
    }
}

bool PtpHello::testdata_valid () const noexcept
{
    // the Python matches the field against ^\252*$, so an empty field passes
    // too.  Anything else means the neighbour is confused or the link is
    // corrupting data, and the circuit is taken down.
    for (std::uint8_t b : testdata)
        if (b != HELLO_FILL) return false;
    return true;
}

Bytes hello_testdata (std::size_t n)
{
    return Bytes (n, HELLO_FILL);
}

}   // namespace decnet::routing
