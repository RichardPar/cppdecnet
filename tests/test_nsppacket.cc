// Port of tests/test_nsppacket.py: the NSP packet formats.

#include "harness.h"

#include "decnet/nsp/packets.h"

using namespace decnet;
using namespace decnet::nsp;

namespace {

Bytes bytes_of (std::initializer_list<int> v)
{
    Bytes b;
    for (int x : v) b.push_back (static_cast<std::uint8_t> (x));
    return b;
}

}   // namespace

DN_TEST (nsppkt, acknum_is_optional)
{
    // The field is present only when its top bit is set; absent, it takes
    // no room at all.
    AckData p;
    p.dstaddr = 3;
    p.srcaddr = 5;
    DN_ASSERT_EQ (p.encode (), bytes_of ({ 0x04, 3, 0, 5, 0 }));

    AckData q = AckData::parse (p.encode ());
    DN_ASSERT (!q.acknum.has_value ());
    DN_ASSERT (!q.acknum2.has_value ());
}

DN_TEST (nsppkt, acknum_roundtrip)
{
    AckData p;
    p.dstaddr = 3;
    p.srcaddr = 5;
    p.acknum = AckNum { Seq (9), AckNum::ACKQ };

    // 0x8009: present, qualifier 0, sequence number 9.
    DN_ASSERT_EQ (p.encode (), bytes_of ({ 0x04, 3, 0, 5, 0, 0x09, 0x80 }));

    AckData q = AckData::parse (p.encode ());
    DN_ASSERT (q.acknum.has_value ());
    DN_ASSERT_EQ (q.acknum->num.value (), 9u);
    DN_ASSERT (!q.acknum->is_nak ());
    DN_ASSERT (!q.acknum->is_cross ());
}

DN_TEST (nsppkt, acknum_qualifiers)
{
    AckData p;
    p.acknum  = AckNum { Seq (1), AckNum::NAK };
    p.acknum2 = AckNum { Seq (2), AckNum::XACK };
    AckData q = AckData::parse (p.encode ());

    DN_ASSERT (q.acknum->is_nak ());
    DN_ASSERT (!q.acknum->is_cross ());
    DN_ASSERT (q.acknum2->is_cross ());
    DN_ASSERT (!q.acknum2->is_nak ());
    DN_ASSERT_EQ (q.acknum2->num.value (), 2u);

    // A cross negative acknowledgement is both.
    AckNum xnak { Seq (3), AckNum::XNAK };
    DN_ASSERT (xnak.is_nak ());
    DN_ASSERT (xnak.is_cross ());
}

DN_TEST (nsppkt, second_acknum_needs_the_first)
{
    // Both fields are optional and positional, so a packet carrying only
    // the second is indistinguishable from one carrying only the first --
    // which is why the sender must not do that.  Decoding what a correct
    // sender produces is what matters here.
    AckData p;
    p.acknum = AckNum { Seq (7), AckNum::ACKQ };
    AckData q = AckData::parse (p.encode ());
    DN_ASSERT (q.acknum.has_value ());
    DN_ASSERT (!q.acknum2.has_value ());
}

DN_TEST (nsppkt, data_segment_flags)
{
    DataSeg p;
    p.dstaddr = 1;
    p.srcaddr = 2;
    p.set_segnum (Seq (5));
    p.payload = bytes_of ({ 'h', 'i' });
    // Begin and end of message live in the flags byte.
    p.msgflag = DataSeg::flag | 0x20 | 0x40;

    Bytes wire = p.encode ();
    auto q = NspPacketBase::parse_frame (wire);
    DN_ASSERT (q != nullptr);
    DN_ASSERT_EQ (std::string (q->packet_name ()), std::string ("DataSeg"));

    auto *d = dynamic_cast<DataSeg *> (q.get ());
    DN_ASSERT (d->bom ());
    DN_ASSERT (d->eom ());
    DN_ASSERT (!d->int_ls ());
    DN_ASSERT_EQ (d->segnum ().value (), 5u);
    DN_ASSERT_EQ (d->payload, p.payload);
    DN_ASSERT_EQ (d->msg_type (), DATA);
}

DN_TEST (nsppkt, every_bom_eom_combination_is_a_data_segment)
{
    // The class lookup is masked so that all four combinations land on
    // DataSeg rather than needing four registrations.
    for (int flags : { 0x00, 0x20, 0x40, 0x60 }) {
        Bytes wire = bytes_of ({ flags, 1, 0, 2, 0, 0, 5, 0, 'x' });
        auto p = NspPacketBase::parse_frame (wire);
        DN_ASSERT (p != nullptr);
        DN_ASSERT_EQ (std::string (p->packet_name ()), std::string ("DataSeg"));
    }
}

DN_TEST (nsppkt, delayed_ack_flag_shares_the_sequence_word)
{
    DataSeg p;
    p.set_segnum (Seq (0xabc));
    p.dly = true;
    Bytes wire = p.encode ();

    DataSeg q = DataSeg::parse (wire);
    DN_ASSERT_EQ (q.segnum ().value (), 0xabcu);
    DN_ASSERT (q.dly);

    // And a sequence number with the flag clear stays clear.
    p.dly = false;
    DN_ASSERT (!DataSeg::parse (p.encode ()).dly);
}

DN_TEST (nsppkt, interrupt_and_link_service_share_a_subchannel)
{
    IntMsg i;
    i.dstaddr = 1;
    i.srcaddr = 2;
    i.segnum = Seq (3);
    i.payload = bytes_of ({ 0xff });
    auto p = NspPacketBase::parse_frame (i.encode ());
    DN_ASSERT_EQ (std::string (p->packet_name ()), std::string ("IntMsg"));
    DN_ASSERT (dynamic_cast<IntMsg *> (p.get ())->int_ls ());
    DN_ASSERT (dynamic_cast<IntMsg *> (p.get ())->is_interrupt ());

    LinkSvcMsg l;
    l.dstaddr = 1;
    l.srcaddr = 2;
    l.segnum = Seq (4);
    l.fcval_int = LinkSvcMsg::DATA_REQ;
    l.fcmod = LinkSvcMsg::XON;
    l.fcval = 5;
    auto q = NspPacketBase::parse_frame (l.encode ());
    DN_ASSERT_EQ (std::string (q->packet_name ()), std::string ("LinkSvcMsg"));
    auto *ls = dynamic_cast<LinkSvcMsg *> (q.get ());
    DN_ASSERT (ls->int_ls ());
    DN_ASSERT (!ls->is_interrupt ());     // link service, not interrupt
    DN_ASSERT_EQ (ls->fcval, 5);
    DN_ASSERT_EQ (ls->fcmod, LinkSvcMsg::XON);
    DN_ASSERT (ls->valid ());
}

DN_TEST (nsppkt, link_service_flow_control_value_is_signed)
{
    // A negative request is how the other end takes credit back.
    LinkSvcMsg l;
    l.fcval = -3;
    Bytes wire = l.encode ();
    DN_ASSERT_EQ (wire.back (), 0xfd);
    DN_ASSERT_EQ (LinkSvcMsg::parse (wire).fcval, -3);
}

DN_TEST (nsppkt, reserved_link_service_values_are_rejected)
{
    LinkSvcMsg l;
    l.fcval_int = 2;                      // only 0 and 1 are defined
    DN_ASSERT (!l.valid ());
    l.fcval_int = 0;
    l.fcmod = 3;                          // reserved
    DN_ASSERT (!l.valid ());
}

DN_TEST (nsppkt, connect_initiate)
{
    ConnInit p;
    p.srcaddr = 7;
    p.fcopt   = SVC_SEG;
    p.info    = VER_PH4;
    p.segsize = 1459;
    p.payload = bytes_of ({ 1, 2, 3 });

    Bytes wire = p.encode ();
    DN_ASSERT_EQ (wire[0], 0x18);
    auto q = NspPacketBase::parse_frame (wire);
    DN_ASSERT_EQ (std::string (q->packet_name ()), std::string ("ConnInit"));

    auto *ci = dynamic_cast<ConnInit *> (q.get ());
    DN_ASSERT_EQ (ci->dstaddr, 0);        // not yet known
    DN_ASSERT_EQ (ci->srcaddr, 7);
    DN_ASSERT_EQ (ci->segsize, 1459);
    DN_ASSERT_EQ (ci->info, VER_PH4);
    DN_ASSERT_EQ (ci->payload, p.payload);
    DN_ASSERT (!ci->retransmitted ());
    DN_ASSERT_EQ (ci->msg_type (), CTL);
}

DN_TEST (nsppkt, retransmitted_connect_initiate_is_the_same_class)
{
    Bytes wire = bytes_of ({ 0x68, 0, 0, 7, 0, 0x01, 0x02, 0xb3, 0x05 });
    auto p = NspPacketBase::parse_frame (wire);
    DN_ASSERT (p != nullptr);
    DN_ASSERT_EQ (std::string (p->packet_name ()), std::string ("ConnInit"));
    DN_ASSERT (dynamic_cast<ConnInit *> (p.get ())->retransmitted ());
}

DN_TEST (nsppkt, connect_confirm)
{
    ConnConf p;
    p.dstaddr  = 7;
    p.srcaddr  = 9;
    p.info     = VER_PH4;
    p.segsize  = 576;
    p.data_ctl = bytes_of ({ 'o', 'k' });

    auto q = NspPacketBase::parse_frame (p.encode ());
    DN_ASSERT_EQ (std::string (q->packet_name ()), std::string ("ConnConf"));
    auto *cc = dynamic_cast<ConnConf *> (q.get ());
    DN_ASSERT_EQ (cc->dstaddr, 7);
    DN_ASSERT_EQ (cc->srcaddr, 9);
    DN_ASSERT_EQ (cc->data_ctl, p.data_ctl);
}

DN_TEST (nsppkt, disconnect_messages)
{
    DiscInit di;
    di.dstaddr  = 1;
    di.srcaddr  = 2;
    di.reason   = OBJ_FAIL;
    di.data_ctl = bytes_of ({ 'b', 'y', 'e' });
    auto p = NspPacketBase::parse_frame (di.encode ());
    DN_ASSERT_EQ (std::string (p->packet_name ()), std::string ("DiscInit"));
    DN_ASSERT_EQ (dynamic_cast<DiscInit *> (p.get ())->reason, OBJ_FAIL);

    DiscConf dc;
    dc.dstaddr = 1;
    dc.srcaddr = 2;
    dc.reason  = DiscConf::DISC_COMPLETE;
    auto q = NspPacketBase::parse_frame (dc.encode ());
    DN_ASSERT_EQ (std::string (q->packet_name ()), std::string ("DiscConf"));
    DN_ASSERT_EQ (dynamic_cast<DiscConf *> (q.get ())->reason, 42);
}

DN_TEST (nsppkt, connect_acknowledgement_tolerates_trailing_bytes)
{
    // VAXELN appends stray bytes; treating them as payload keeps the
    // packet rather than dropping it on a parse error.
    Bytes wire = bytes_of ({ 0x24, 7, 0, 0xde, 0xad });
    auto p = NspPacketBase::parse_frame (wire);
    DN_ASSERT (p != nullptr);
    DN_ASSERT_EQ (std::string (p->packet_name ()), std::string ("AckConn"));
    auto *ac = dynamic_cast<AckConn *> (p.get ());
    DN_ASSERT_EQ (ac->dstaddr, 7);
    DN_ASSERT_EQ (ac->payload, bytes_of ({ 0xde, 0xad }));
}

DN_TEST (nsppkt, version_and_phase_mapping)
{
    DN_ASSERT_EQ (std::string (version_string (VER_PH4)), std::string ("4.0"));
    DN_ASSERT_EQ (std::string (version_string (VER_41)), std::string ("4.1"));
    DN_ASSERT_EQ (phase_of_version (VER_PH2), 2u);
    DN_ASSERT_EQ (phase_of_version (VER_PH3), 3u);
    DN_ASSERT_EQ (phase_of_version (VER_41), 4u);
}

DN_TEST (nsppkt, wire_forms_match_python_byte_for_byte)
{
    // Each expected value here was produced by the Python V1.1.1 building the
    // same message.  A format this port agreed with only itself would be
    // worth very little.
    struct Case { const char *what; Bytes ours; Bytes theirs; };
    std::vector<Case> cases;

    ConnInit ci;
    ci.srcaddr = 7; ci.fcopt = SVC_SEG; ci.info = VER_PH4;
    ci.segsize = 1459; ci.payload = bytes_of ({ 1, 2, 3 });
    cases.push_back ({ "ConnInit", ci.encode (),
        bytes_of ({ 0x18, 0x00, 0x00, 0x07, 0x00, 0x05, 0x02, 0xb3, 0x05,
                    0x01, 0x02, 0x03 }) });

    ConnConf cc;
    cc.dstaddr = 7; cc.srcaddr = 9; cc.fcopt = SVC_SEG; cc.info = VER_PH4;
    cc.segsize = 576; cc.data_ctl = bytes_of ({ 'o', 'k' });
    cases.push_back ({ "ConnConf", cc.encode (),
        bytes_of ({ 0x28, 0x07, 0x00, 0x09, 0x00, 0x05, 0x02, 0x40, 0x02,
                    0x02, 0x6f, 0x6b }) });

    AckData ad;
    ad.dstaddr = 3; ad.srcaddr = 5;
    ad.acknum = AckNum { Seq (9), AckNum::ACKQ };
    cases.push_back ({ "AckData", ad.encode (),
        bytes_of ({ 0x04, 0x03, 0x00, 0x05, 0x00, 0x09, 0x80 }) });

    DataSeg ds;
    ds.dstaddr = 1; ds.srcaddr = 2; ds.set_segnum (Seq (5));
    ds.msgflag = DataSeg::flag | 0x20 | 0x40;
    ds.payload = bytes_of ({ 'h', 'i' });
    cases.push_back ({ "DataSeg", ds.encode (),
        bytes_of ({ 0x60, 0x01, 0x00, 0x02, 0x00, 0x05, 0x00, 0x68,
                    0x69 }) });

    LinkSvcMsg ls;
    ls.dstaddr = 1; ls.srcaddr = 2; ls.segnum = Seq (4);
    ls.fcval_int = 0; ls.fcmod = 2; ls.fcval = 5;
    cases.push_back ({ "LinkSvcMsg", ls.encode (),
        bytes_of ({ 0x10, 0x01, 0x00, 0x02, 0x00, 0x04, 0x00, 0x02,
                    0x05 }) });

    DiscInit di;
    di.dstaddr = 1; di.srcaddr = 2; di.reason = OBJ_FAIL;
    di.data_ctl = bytes_of ({ 'b', 'y', 'e' });
    cases.push_back ({ "DiscInit", di.encode (),
        bytes_of ({ 0x38, 0x01, 0x00, 0x02, 0x00, 0x26, 0x00, 0x03,
                    0x62, 0x79, 0x65 }) });

    for (const Case &c : cases) {
        DN_ASSERT_EQ (c.ours, c.theirs);
        // And each decodes back to the right class.
        auto p = NspPacketBase::parse_frame (c.theirs);
        DN_ASSERT (p != nullptr);
        DN_ASSERT_EQ (std::string (p->packet_name ()), std::string (c.what));
        DN_ASSERT_EQ (p->encode_packet (), c.theirs);
    }
}

DN_TEST (nsppkt, malformed_packets_return_null)
{
    DN_ASSERT (NspPacketBase::parse_frame (Bytes {}) == nullptr);
    DN_ASSERT (NspPacketBase::parse_frame (bytes_of ({ 0x04 })) == nullptr);
    // An unassigned message flag.
    DN_ASSERT (NspPacketBase::parse_frame (bytes_of ({ 0x0c, 1, 0, 2, 0 }))
               == nullptr);
}

DN_TEST (nsppkt, sequence_numbers_wrap_the_way_nsp_needs)
{
    // The property the retransmission logic depends on.
    DN_ASSERT (Seq (4095) < Seq (0));
    DN_ASSERT (Seq (4090) < Seq (5));
    DataSeg p;
    p.set_segnum (Seq (4095));
    DN_ASSERT_EQ (DataSeg::parse (p.encode ()).segnum ().value (), 4095u);
}
