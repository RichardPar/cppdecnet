// Tests for dnfal's user file.

#include "harness.h"

#include "decnet/dap/fal_users.h"

#include <stdexcept>

using namespace decnet::dap;

namespace {

// A table with one of each kind of entry.  Hashes are made fresh, so the
// test uses whatever method this system's crypt prefers.
FalUsers table ()
{
    return FalUsers::parse (
        "# comment\n"
        "richard " + hash_password ("secret") + " home rw\n"
        "\n"
        "Guest   -   /srv/pub   ro   # trailing comment\n",
        "/srv/decnet");
}

}   // namespace

DN_TEST (fal_users, hashes_check_and_differ)
{
    std::string h = hash_password ("pw");
    DN_ASSERT (check_password ("pw", h));
    DN_ASSERT (!check_password ("PW", h));
    DN_ASSERT (!check_password ("", h));
    DN_ASSERT (h != hash_password ("pw"));      // salted
    DN_ASSERT (!check_password ("pw", "not a hash"));
}

DN_TEST (fal_users, the_file_is_parsed)
{
    FalUsers u = table ();
    DN_ASSERT_EQ (u.size (), 2u);

    auto r = u.authenticate ("RICHARD", "secret");
    DN_ASSERT (r.has_value ());
    DN_ASSERT_EQ (r->root, std::string ("/srv/decnet/home"));   // relative
    DN_ASSERT (r->writable);

    auto g = u.authenticate ("guest", "anything");
    DN_ASSERT (g.has_value ());
    DN_ASSERT_EQ (g->root, std::string ("/srv/pub"));
    DN_ASSERT (!g->writable);
}

DN_TEST (fal_users, wrong_or_missing_credentials_are_refused)
{
    FalUsers u = table ();
    DN_ASSERT (!u.authenticate ("richard", "wrong"));
    DN_ASSERT (!u.authenticate ("richard", ""));
    DN_ASSERT (!u.authenticate ("nobody", "secret"));
    // No "*" entry, so no anonymous access.
    DN_ASSERT (!u.authenticate ("", ""));
}

DN_TEST (fal_users, vms_upper_case_passwords_match)
{
    // DCL sends NODE"RICHARD SECRET":: when the quotes hold no inner quotes.
    DN_ASSERT (table ().authenticate ("RICHARD", "SECRET").has_value ());
    // But a password stored in mixed case must be sent as it is.
    FalUsers m = FalUsers::parse ("u " + hash_password ("MiXed") + " d ro\n", "/r");
    DN_ASSERT (m.authenticate ("u", "MiXed").has_value ());
    DN_ASSERT (!m.authenticate ("u", "MIXED").has_value ());
}

DN_TEST (fal_users, anonymous_needs_a_star_entry)
{
    FalUsers u = FalUsers::parse ("* - pub ro\n", "/srv");
    auto a = u.authenticate ("", "");
    DN_ASSERT (a.has_value ());
    DN_ASSERT_EQ (a->root, std::string ("/srv/pub"));
    // A named user that is not listed does not fall back to anonymous.
    DN_ASSERT (!u.authenticate ("someone", ""));
}

DN_TEST (fal_users, mistakes_name_their_line)
{
    auto error = [] (const std::string &text) -> std::string {
        try { FalUsers::parse (text, "/r", "f"); }
        catch (const std::runtime_error &e) { return e.what (); }
        return "no error";
    };
    DN_ASSERT_EQ (error ("\nu - d\n"),
                  std::string ("f line 2: expected: user hash directory ro|rw"));
    DN_ASSERT_EQ (error ("u - d rx\n"),
                  std::string ("f line 1: access must be ro or rw"));
    DN_ASSERT_EQ (error ("u secret d ro\n"),
                  std::string ("f line 1: that is not a password hash; "
                               "see dnfal --hash"));
    DN_ASSERT_EQ (error ("u - a ro\nU - b ro\n"),
                  std::string ("f line 2: user U twice"));
}
