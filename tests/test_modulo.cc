// Port of tests/test_modulo.py: RFC 1982 sequence number arithmetic.
//
// The Python tests use moduli 15 and 16 to exercise the odd and even cases,
// where "even" is the one with a half-way distance that has no defined
// order.  Same values here.

#include "harness.h"

#include "decnet/common/modulo.h"

#include <compare>

using namespace decnet;

using Mod15 = Mod<15>;
using Mod16 = Mod<16>;

DN_TEST (modulo, construction_range)
{
    DN_ASSERT_EQ (Mod16 (1).value (), 1u);
    DN_ASSERT_EQ (Mod16 (15).value (), 15u);
    DN_ASSERT_THROWS (std::overflow_error, Mod16 (16));
    // wrap() reduces instead of throwing, for values built from packets.
    DN_ASSERT_EQ (Mod16::wrap (16).value (), 0u);
    DN_ASSERT_EQ (Mod16::wrap (0x1234).value (), 4u);
}

DN_TEST (modulo, constants)
{
    // Even modulus: half the modulus is the undefined distance.
    DN_ASSERT_EQ (Mod16::maxdelta, 7u);
    DN_ASSERT (Mod16::has_undef);
    DN_ASSERT_EQ (Mod16::undef, 8u);
    // Odd modulus: every distance is ordered.
    DN_ASSERT_EQ (Mod15::maxdelta, 7u);
    DN_ASSERT (!Mod15::has_undef);
}

DN_TEST (modulo, equal_compares_equal)
{
    Mod16 a (1);
    DN_ASSERT (a == a);
    DN_ASSERT (a <= a);
    DN_ASSERT (a >= a);
    DN_ASSERT (!(a != a));
    DN_ASSERT (!(a < a));
    DN_ASSERT (!(a > a));
}

DN_TEST (modulo, ordering_even_modulus)
{
    Mod16 a (1), b (8), c (10);
    // 1 < 8: forward distance 7, within maxdelta.
    DN_ASSERT (a < b);
    DN_ASSERT (a <= b);
    DN_ASSERT (a != b);
    DN_ASSERT (!(a > b));
    DN_ASSERT (!(a >= b));

    // 1 vs 10: forward distance 9 is more than half, so 1 is the greater.
    DN_ASSERT (!(a < c));
    DN_ASSERT (!(a <= c));
    DN_ASSERT (a > c);
    DN_ASSERT (a >= c);
}

DN_TEST (modulo, ordering_odd_modulus)
{
    Mod15 a (1), b (8), c (9);
    DN_ASSERT (a < b);
    DN_ASSERT (a <= b);
    DN_ASSERT (!(a > b));

    DN_ASSERT (!(a < c));
    DN_ASSERT (a > c);
    DN_ASSERT (a >= c);
}

DN_TEST (modulo, half_way_is_unordered)
{
    // Python raises TypeError here; C++ has a spelling for it.
    Mod16 a (1), b (9);
    DN_ASSERT (!Mod16::comparable (a, b));
    DN_ASSERT ((a <=> b) == std::partial_ordering::unordered);
    // Every ordering test is false, in both directions.
    DN_ASSERT (!(a < b));
    DN_ASSERT (!(a <= b));
    DN_ASSERT (!(a > b));
    DN_ASSERT (!(a >= b));
    DN_ASSERT (!(b < a));
    DN_ASSERT (!(b > a));
    // But equality is still well defined.
    DN_ASSERT (a != b);
    DN_ASSERT (!(a == b));
}

DN_TEST (modulo, wraparound_ordering)
{
    // The property NSP actually depends on: near the wrap point, the
    // smaller number is the later one.
    using Seq = Mod<4096>;
    Seq high (4095), low (0);
    DN_ASSERT (high < low);
    DN_ASSERT (low > high);
    DN_ASSERT (Seq (4090) < Seq (5));
    DN_ASSERT_EQ (Seq::maxdelta, 2047u);
}

DN_TEST (modulo, arithmetic_wraps)
{
    Mod16 a (14);
    DN_ASSERT_EQ ((a + Mod16 (3)).value (), 1u);
    DN_ASSERT_EQ ((Mod16 (1) - Mod16 (3)).value (), 14u);

    Mod16 b (15);
    DN_ASSERT_EQ ((++b).value (), 0u);
    Mod16 c (15);
    DN_ASSERT_EQ ((c++).value (), 15u);
    DN_ASSERT_EQ (c.value (), 0u);
}

DN_TEST (modulo, distance_to)
{
    // The forward distance is what the NSP flow control window measures.
    DN_ASSERT_EQ (Mod16 (14).distance_to (Mod16 (2)), 4u);
    DN_ASSERT_EQ (Mod16 (2).distance_to (Mod16 (14)), 12u);
    DN_ASSERT_EQ (Mod16 (5).distance_to (Mod16 (5)), 0u);
}
