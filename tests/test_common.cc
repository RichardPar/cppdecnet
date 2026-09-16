// Port of tests/test_common.py: the value types every layer shares.

#include "harness.h"

#include "decnet/common/crc.h"
#include "decnet/common/types.h"

using namespace decnet;

DN_TEST (nodeid, parse_phase4)
{
    Nodeid n = Nodeid::parse ("9.54");
    DN_ASSERT_EQ (n.area (), 9u);
    DN_ASSERT_EQ (n.tid (), 54u);
    DN_ASSERT_EQ (n.value (), static_cast<std::uint16_t> (9 * 1024 + 54));
    DN_ASSERT_EQ (n.str (), std::string ("9.54"));
}

DN_TEST (nodeid, parse_phase3)
{
    Nodeid n = Nodeid::parse ("54");
    DN_ASSERT_EQ (n.area (), 0u);
    DN_ASSERT_EQ (n.tid (), 54u);
    DN_ASSERT (n.is_phase3 ());
    DN_ASSERT_EQ (n.str (), std::string ("54"));
}

DN_TEST (nodeid, range_checks)
{
    DN_ASSERT_THROWS (std::invalid_argument, Nodeid::parse ("64.1"));
    DN_ASSERT_THROWS (std::invalid_argument, Nodeid::parse ("1.1024"));
    DN_ASSERT_THROWS (std::invalid_argument, Nodeid::parse ("0.5"));
    DN_ASSERT_THROWS (std::invalid_argument, Nodeid::parse ("bogus"));
    DN_ASSERT_THROWS (std::invalid_argument, Nodeid::parse ("1.2.3"));
}

DN_TEST (macaddr, parse_and_format)
{
    Macaddr m = Macaddr::parse ("aa-00-04-00-36-24");
    DN_ASSERT_EQ (m.str (), std::string ("aa-00-04-00-36-24"));
    DN_ASSERT (!m.is_multicast ());
    DN_ASSERT (m.is_local ());
    DN_ASSERT_EQ (Macaddr::parse ("AA:00:04:00:36:24"), m);
    DN_ASSERT_THROWS (std::invalid_argument, Macaddr::parse ("aa-00-04-00-36"));
    DN_ASSERT_THROWS (std::invalid_argument, Macaddr::parse ("zz-00-04-00-36-24"));
}

DN_TEST (macaddr, from_nodeid)
{
    // 9.54 -> AA-00-04-00-36-24, the standard Phase IV address mapping.
    DN_ASSERT_EQ (Macaddr::from_nodeid (Nodeid::parse ("9.54")),
                  Macaddr::parse ("aa-00-04-00-36-24"));
    DN_ASSERT_EQ (Macaddr::from_nodeid (Nodeid::parse ("1.1")),
                  Macaddr::parse ("aa-00-04-00-01-04"));
}

DN_TEST (version, parse_and_format)
{
    Version v = Version::parse ("4.0.0");
    DN_ASSERT_EQ (v.v1, 4);
    DN_ASSERT_EQ (v.str (), std::string ("4.0.0"));
    DN_ASSERT_THROWS (std::invalid_argument, Version::parse ("4.0"));
}

DN_TEST (crc, ddcmp_crc16)
{
    // The CRC-16 check value over "123456789", the standard test vector.
    const char *s = "123456789";
    Bytes b (s, s + 9);
    DN_ASSERT_EQ (CRC16::compute (b), 0xbb3du);
}

DN_TEST (crc, ccitt_and_32)
{
    const char *s = "123456789";
    Bytes b (s, s + 9);
    DN_ASSERT_EQ (CRCCCITT::compute (b), 0x29b1u);
    DN_ASSERT_EQ (CRC32::compute (b), 0xcbf43926u);
}

DN_TEST (crc, residue_check)
{
    // CRC over the data plus its little endian check bytes gives the residue.
    const char *s = "hello, decnet";
    Bytes b (s, s + 13);
    std::uint16_t c = CRC16::compute (b);
    b.push_back (static_cast<std::uint8_t> (c & 0xff));
    b.push_back (static_cast<std::uint8_t> (c >> 8));
    CRC16 check;
    check.update (b);
    DN_ASSERT (check.good ());
}
