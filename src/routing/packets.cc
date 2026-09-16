#include "decnet/routing/packets.h"

namespace decnet::routing {

// Register all routing packet classes.  See DN_PACKET_INDEX_REGISTERED.
// Masks are needed because the flags byte mixes type and flag bits.
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

    // Routing messages use a nested lookup keyed on the checksum residue.
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
    // Test data must be all 0252 (empty allowed); otherwise the circuit is
    // taken down.
    for (std::uint8_t b : testdata)
        if (b != HELLO_FILL) return false;
    return true;
}

Bytes hello_testdata (std::size_t n)
{
    return Bytes (n, HELLO_FILL);
}

}   // namespace decnet::routing
