#include "decnet/nsp/packets.h"

#include "decnet/common/logging.h"

namespace decnet::nsp {

const char *version_string (unsigned v) noexcept
{
    switch (v) {
    case VER_PH3: return "3.2";
    case VER_PH2: return "3.1";
    case VER_PH4: return "4.0";
    case VER_41:  return "4.1";
    }
    return "?";
}

unsigned phase_of_version (unsigned v) noexcept
{
    switch (v) {
    case VER_PH2: return 2;
    case VER_PH3: return 3;
    default:      return 4;
    }
}

// --------------------------------------------------------------- AckNum

std::string AckNum::str () const
{
    static const char *const names[] = { "ACK", "NAK", "XACK", "XNAK" };
    return std::string (names[qual & 3]) + " " + num.str ();
}

void AckNumField::encode (Encoder &e, const value_type &v)
{
    // An absent acknowledgement occupies no space at all.
    if (!v) return;
    std::uint16_t w = static_cast<std::uint16_t> (
        0x8000 | (static_cast<unsigned> (v->qual) << 12) | v->num.value ());
    e.uint (w, 2);
}

void AckNumField::decode (Decoder &d, value_type &v)
{
    v.reset ();
    if (d.remaining () < 2) return;
    std::uint64_t w = d.peek_uint (2);
    if (!(w & 0x8000)) return;          // the field is not there

    // The field is present, so consume it whatever the qualifier says.
    (void) d.uint (2);
    unsigned qual = (w >> 12) & 7;
    if (qual > 3) {
        // A reserved qualifier: skip the field rather than inventing a
        // meaning for it, which is what pydecnet does.
        return;
    }
    AckNum a;
    a.qual = static_cast<AckNum::Qual> (qual);
    a.num  = Seq::wrap (w);
    v = a;
}

// --------------------------------------------------------- registration

void register_nsp_packets ()
{
    auto &idx = NspPacketBase::raw_index ();

    idx.add (AckData::flag,  &AckData::make,  "AckData");
    idx.add (AckOther::flag, &AckOther::make, "AckOther");
    idx.add (AckConn::flag,  &AckConn::make,  "AckConn");

    // A data segment's flags byte carries the begin and end of message
    // bits, so every combination of them is the same class.  pydecnet
    // enumerates the four; a mask says the same thing.
    idx.add_masked (DataSeg::flag, 0x9f, &DataSeg::make, "DataSeg");

    idx.add (IntMsg::flag,     &IntMsg::make,     "IntMsg");
    idx.add (LinkSvcMsg::flag, &LinkSvcMsg::make, "LinkSvcMsg");

    // Connect Initiate and its retransmission differ only in subtype.
    idx.add (ConnInit::flag,            &ConnInit::make, "ConnInit");
    idx.add (ConnInit::flag_retransmit, &ConnInit::make, "ConnInit");

    idx.add (ConnConf::flag, &ConnConf::make, "ConnConf");
    idx.add (DiscConf::flag, &DiscConf::make, "DiscConf");
    idx.add (DiscInit::flag, &DiscInit::make, "DiscInit");

    // PORT: a NOP control message (subtype 0) is accepted by pydecnet and
    // ignored.  Nothing sends one to us yet.
}

}   // namespace decnet::nsp
