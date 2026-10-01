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

// ------------------------------------------------------------------ proxy

namespace {

FalUsers proxies (bool with_default)
{
    return FalUsers::parse (
        "richard " + hash_password ("secret") + " home rw\n"
        "guest   -   pub  ro\n"
        + std::string (with_default ? "*  -  anon ro\n" : "")
        + "proxy VMSNOD::RICHARD richard\n"
        "proxy VMSNOD::* guest\n"
        "proxy *::SYSTEM -\n"
        "proxy 1.5::OPER richard\n",
        "/srv");
}

Requester proxy (const std::string &node, const std::string &user,
                 const std::string &addr = "1.2")
{
    Requester r;
    r.proxy = true;
    r.user = user;
    r.node_name = node;
    r.node_address = addr;
    return r;
}

}   // namespace

DN_TEST (fal_users, proxy_picks_the_most_specific_line)
{
    FalUsers u = proxies (false);
    auto a = u.authenticate (proxy ("vmsnod", "richard"));
    DN_ASSERT (a.has_value ());
    DN_ASSERT_EQ (a->name, std::string ("RICHARD"));
    DN_ASSERT (a->writable);

    // Anyone else from that node gets the node's line.
    auto b = u.authenticate (proxy ("VMSNOD", "FRED"));
    DN_ASSERT (b.has_value ());
    DN_ASSERT_EQ (b->name, std::string ("GUEST"));

    // A node by address.
    auto c = u.authenticate (proxy ("", "OPER", "1.5"));
    DN_ASSERT (c.has_value ());
    DN_ASSERT_EQ (c->name, std::string ("RICHARD"));
}

DN_TEST (fal_users, proxy_can_be_refused_and_falls_back_to_the_default)
{
    FalUsers none = proxies (false);
    // SYSTEM is refused from any node without a line of its own ...
    DN_ASSERT (!none.authenticate (proxy ("OTHER", "SYSTEM")));
    // ... but a line for the node outranks one for the user, as in
    // DECnet-VAX's order: node::user, node::*, *::user, *::*.
    DN_ASSERT_EQ (none.authenticate (proxy ("VMSNOD", "SYSTEM"))->name,
                  std::string ("GUEST"));
    // Unknown node and user, and no default account: refused.
    DN_ASSERT (!none.authenticate (proxy ("OTHER", "FRED")));

    FalUsers with = proxies (true);
    auto d = with.authenticate (proxy ("OTHER", "FRED"));
    DN_ASSERT (d.has_value ());
    DN_ASSERT_EQ (d->name, std::string ("*"));
    DN_ASSERT (!with.authenticate (proxy ("OTHER", "SYSTEM")));
}

DN_TEST (fal_users, a_proxy_name_is_not_a_login)
{
    // RICHARD by proxy from a node with no line for him is not the local
    // RICHARD, whose password it does not have.
    FalUsers u = proxies (true);
    auto a = u.authenticate (proxy ("ELSEWHERE", "RICHARD"));
    DN_ASSERT (a.has_value ());
    DN_ASSERT_EQ (a->name, std::string ("*"));

    // With a password it is an ordinary login, proxy flag or not.
    Requester r = proxy ("ELSEWHERE", "richard");
    r.password = "secret";
    DN_ASSERT_EQ (u.authenticate (r)->name, std::string ("RICHARD"));
    r.password = "wrong";
    DN_ASSERT (!u.authenticate (r));
}

DN_TEST (fal_users, proxy_user_may_come_from_the_source_end_user)
{
    FalUsers u = proxies (false);
    Requester r = proxy ("VMSNOD", "");
    r.source_user = "[200,201]RICHARD";
    auto a = u.authenticate (r);
    DN_ASSERT (a.has_value ());
    DN_ASSERT_EQ (a->name, std::string ("RICHARD"));
}

DN_TEST (fal_users, a_proxy_must_name_a_known_user)
{
    bool threw = false;
    try { FalUsers::parse ("proxy A::B nobody\n", "/r", "f"); }
    catch (const std::runtime_error &e) {
        threw = std::string (e.what ()).find ("NOBODY") != std::string::npos;
    }
    DN_ASSERT (threw);
    DN_ASSERT_THROWS (std::runtime_error,
                      FalUsers::parse ("proxy NOSEP guest\nguest - p ro\n", "/r"));
}
