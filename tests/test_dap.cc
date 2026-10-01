// Tests for the DAP messages.  The byte strings are what PyDECnet's
// dap_packets.py encodes for the same messages.

#include "harness.h"

#include "decnet/common/exceptions.h"
#include "decnet/dap/messages.h"

using namespace decnet;
using namespace decnet::dap;

namespace {

template <typename T>
const T &as (const Message &m)
{
    const T *p = std::get_if<T> (&m);
    if (!p) throw std::runtime_error (std::string ("decoded as ")
                                      + type_name (type_of (m)));
    return *p;
}

template <typename T>
T one (const Bytes &b)
{
    auto v = decode (b);
    if (v.size () != 1) throw std::runtime_error ("not exactly one message");
    return as<T> (v[0]);
}

}   // namespace

// ------------------------------------------------------------------- EX-n

DN_TEST (dap, ext_fields_use_seven_bits_a_byte)
{
    Bytes o;
    Ext ().encode (o, 3);
    DN_ASSERT_EQ (o, (Bytes { 0x00 }));

    o.clear ();
    Ext ().set (8).encode (o, 3);               // bit 8 is in the second byte
    DN_ASSERT_EQ (o, (Bytes { 0x80, 0x02 }));

    o.clear ();
    DN_ASSERT_THROWS (FieldOverflow, Ext ().set (21).encode (o, 3));

    Ext e;
    e.set (0).set (1);
    DN_ASSERT_EQ (e.field (0, 2), 3u);
}

// --------------------------------------------------------- against PyDECnet

DN_TEST (dap, config_matches_python)
{
    Config c;
    c.bufsiz = 65535;
    c.ostype = 192;
    c.filesys = 13;
    for (unsigned b : { Config::cap_fo_seq, Config::cap_seq_xfer,
                        Config::cap_blocking, Config::cap_len2, Config::cap_dir,
                        Config::cap_dattim_xa, Config::cap_fprot_xa,
                        Config::cap_seq_ra, Config::cap_glob, Config::cap_name })
        c.syscap.set (b);
    Bytes want { 0x01, 0x00, 0xff, 0xff, 0xc0, 0x0d, 0x07, 0x00, 0x00, 0x00,
                 0x00, 0xa2, 0x80, 0xd0, 0xf0, 0xa0, 0x28 };
    DN_ASSERT_EQ (encode (c), want);

    Config d = one<Config> (want);
    DN_ASSERT_EQ (d.bufsiz, 65535);
    DN_ASSERT_EQ (d.ostype, 192);
    DN_ASSERT (d.syscap[Config::cap_blocking]);
    DN_ASSERT (d.syscap[Config::cap_name]);
    DN_ASSERT (!d.syscap[Config::cap_append]);
}

DN_TEST (dap, access_matches_python)
{
    Access a;
    a.accfunc = Access::directory;
    a.filespec = "[USER]*.*;*";
    a.display.set (Access::d_main).set (Access::d_date).set (Access::d_fprot)
             .set (Access::d_name);
    Bytes want { 0x03, 0x00, 0x06, 0x00, 0x0b, 0x5b, 0x55, 0x53, 0x45, 0x52,
                 0x5d, 0x2a, 0x2e, 0x2a, 0x3b, 0x2a, 0x00, 0x00, 0xb1, 0x02 };
    // PyDECnet sends FAC and SHR as zero where the default is GET.
    a.fac = Ext ();
    a.shr = Ext ();
    DN_ASSERT_EQ (encode (a), want);
    Access d = one<Access> (want);
    DN_ASSERT_EQ (d.filespec, std::string ("[USER]*.*;*"));
    DN_ASSERT (d.display[Access::d_name]);
}

DN_TEST (dap, attributes_match_python)
{
    Attributes a;
    for (unsigned m : { Attributes::m_bls, Attributes::m_rfm, Attributes::m_alq,
                        Attributes::m_hbk, Attributes::m_ebk, Attributes::m_ffb })
        a.menu.set (m);
    a.rfm = Attributes::fb_slf;
    a.alq = 8;
    a.hbk = a.ebk = 3;
    a.ffb = 100;
    Bytes want { 0x02, 0x00, 0xd4, 0x80, 0x38, 0x05, 0x00, 0x02, 0x01, 0x08,
                 0x01, 0x03, 0x01, 0x03, 0x64, 0x00 };
    DN_ASSERT_EQ (encode (a), want);

    Attributes d = one<Attributes> (want);
    DN_ASSERT_EQ (d.rfm, Attributes::fb_slf);
    DN_ASSERT_EQ (d.ebk, 3u);
    DN_ASSERT_EQ (*d.size (), 2u * 512 + 100);
    DN_ASSERT (!d.text ());
}

DN_TEST (dap, missing_fields_take_their_defaults)
{
    // Data type ASCII, variable records, carriage return: the message stops
    // there, so the block size is the default 512 and the byte size 8.
    Bytes b { 0x02, 0x00, 0x0d, 0x01, 0x02, 0x02 };
    Attributes a = one<Attributes> (b);
    DN_ASSERT (a.datatype[Attributes::dt_ascii]);
    DN_ASSERT_EQ (a.rfm, Attributes::fb_var);
    DN_ASSERT (a.text ());
    DN_ASSERT_EQ (a.bls, 512);
    DN_ASSERT_EQ (a.bsz, 8);
    DN_ASSERT (!a.size ());

    // A bare type byte is a whole message too.
    Control c = one<Control> (Bytes { 0x04 });
    DN_ASSERT_EQ (c.ctlfunc, Control::get);
}

DN_TEST (dap, small_messages_match_python)
{
    Control c;
    c.ctlfunc = Control::get;
    c.menu.set (Control::m_rac);
    c.rac = Control::rb_seqf;
    DN_ASSERT_EQ (encode (c), (Bytes { 0x04, 0x00, 0x01, 0x01, 0x03 }));

    Status s;
    s.maccode = Status::transfer_error;
    s.miccode = Status::rms_eof;
    DN_ASSERT_EQ (encode (s), (Bytes { 0x09, 0x00, 0x27, 0x50 }));
    DN_ASSERT (one<Status> (encode (s)).eof ());

    DN_ASSERT_EQ (encode (AccessComplete {}),
                  (Bytes { 0x07, 0x00, 0x01, 0x00, 0x00, 0x00 }));
    DN_ASSERT_EQ (encode (Data { 0, Bytes { 'h', 'e', 'l', 'l', 'o' } }),
                  (Bytes { 0x08, 0x00, 0x00, 0x68, 0x65, 0x6c, 0x6c, 0x6f }));
    DN_ASSERT_EQ (encode (Ack {}), (Bytes { 0x06, 0x00 }));
}

DN_TEST (dap, date_protection_and_name_match_python)
{
    DateTime d;
    d.menu.set (DateTime::m_cdt).set (DateTime::m_rdt);
    d.cdt = "30-SEP-26 13:24:26";
    d.rdt = "01-JAN-99 00:00:00";
    Bytes dw = encode (d);
    DN_ASSERT_EQ (dw.size (), 39u);
    DN_ASSERT_EQ (one<DateTime> (dw).rdt, std::string ("01-JAN-99 00:00:00"));

    // Owner richard, mode 640.
    Bytes pw { 0x0e, 0x00, 0x1d, 0x07, 0x72, 0x69, 0x63, 0x68, 0x61, 0x72,
               0x64, 0x04, 0x0e, 0x0f };
    Protection p = one<Protection> (pw);
    DN_ASSERT_EQ (p.owner, std::string ("richard"));
    DN_ASSERT_EQ (p.vms (), std::string ("(,RWD,R,)"));
    DN_ASSERT_EQ (encode (p), pw);

    Name n;
    n.nametype.set (Name::filename);
    n.namespec = "LOGIN.COM";
    DN_ASSERT_EQ (encode (n), (Bytes { 0x0f, 0x00, 0x02, 0x09, 0x4c, 0x4f,
                                       0x47, 0x49, 0x4e, 0x2e, 0x43, 0x4f,
                                       0x4d }));
}

DN_TEST (dap, blocked_messages_are_split)
{
    // A Name then an Attributes in one session message, each with a length
    // field, as PyDECnet's DapSession.send blocks them.
    Bytes b { 0x0f, 0x02, 0x0b, 0x02, 0x09, 0x4c, 0x4f, 0x47, 0x49, 0x4e,
              0x2e, 0x43, 0x4f, 0x4d, 0x02, 0x02, 0x0e, 0xd4, 0x80, 0x38,
              0x05, 0x00, 0x02, 0x01, 0x08, 0x01, 0x03, 0x01, 0x03, 0x64,
              0x00 };
    auto v = decode (b);
    DN_ASSERT_EQ (v.size (), 2u);
    DN_ASSERT_EQ (as<Name> (v[0]).namespec, std::string ("LOGIN.COM"));
    DN_ASSERT_EQ (as<Attributes> (v[1]).ffb, 100);

    // A length running past the end is an error, not a crash.
    Bytes bad { 0x0f, 0x02, 0x20, 0x02 };
    DN_ASSERT_THROWS (MissingData, decode (bad));
}

DN_TEST (dap, unsupported_messages_are_kept_whole)
{
    auto v = decode (Bytes { 0x0c, 0x00, 0x01, 0x02 });   // summary
    DN_ASSERT_EQ (v.size (), 1u);
    DN_ASSERT_EQ (as<Unknown> (v[0]).type, 12);
    DN_ASSERT_EQ (as<Unknown> (v[0]).body, (Bytes { 0x01, 0x02 }));
}

DN_TEST (dap, status_text)
{
    Status s;
    s.maccode = Status::open_error;
    s.miccode = 062;
    DN_ASSERT_EQ (s.str (), std::string ("Open error: file not found."));
    s.maccode = Status::transfer_error;
    s.miccode = Status::rms_eof;
    DN_ASSERT_EQ (s.str (), std::string ("Transfer error: end-of-file."));
}
