// dap/fal_users.cc -- who may use the FAL, and where.

#include "decnet/dap/fal_users.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>

#include <crypt.h>

namespace decnet::dap {

namespace {

std::string upper (std::string s)
{
    for (char &c : s) c = static_cast<char> (std::toupper (static_cast<unsigned char> (c)));
    return s;
}

std::string lower (std::string s)
{
    for (char &c : s) c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
    return s;
}

// Compare without stopping at the first difference.
bool same (const std::string &a, const std::string &b)
{
    if (a.size () != b.size ()) return false;
    unsigned char d = 0;
    for (std::size_t i = 0; i < a.size (); ++i)
        d |= static_cast<unsigned char> (a[i] ^ b[i]);
    return d == 0;
}

}   // namespace

bool check_password (const std::string &password, const std::string &hash)
{
    // crypt_r's work area is large; keep it off the stack.
    auto data = std::make_unique<crypt_data> ();
    const char *h = ::crypt_r (password.c_str (), hash.c_str (), data.get ());
    // A failed crypt returns null, or a string starting with "*".
    if (!h || h[0] == '*') return false;
    return same (h, hash);
}

std::string hash_password (const std::string &password)
{
    char salt[CRYPT_GENSALT_OUTPUT_SIZE];
    if (!::crypt_gensalt_rn (nullptr, 0, nullptr, 0, salt, sizeof salt))
        throw std::runtime_error (std::string ("cannot make a salt: ")
                                  + std::strerror (errno));
    auto data = std::make_unique<crypt_data> ();
    const char *h = ::crypt_r (password.c_str (), salt, data.get ());
    if (!h || h[0] == '*')
        throw std::runtime_error ("crypt failed");
    return h;
}

FalUsers FalUsers::load (const std::string &path, const std::string &base)
{
    std::ifstream f (path);
    if (!f) throw std::runtime_error ("cannot read " + path + ": "
                                      + std::strerror (errno));
    std::stringstream s;
    s << f.rdbuf ();
    return parse (s.str (), base, path);
}

FalUsers FalUsers::parse (const std::string &text, const std::string &base,
                          const std::string &source)
{
    FalUsers out;
    std::istringstream in (text);
    std::string line;
    unsigned n = 0;
    while (std::getline (in, line)) {
        ++n;
        if (auto h = line.find ('#'); h != std::string::npos) line.erase (h);
        std::istringstream words (line);
        std::vector<std::string> w;
        for (std::string x; words >> x;) w.push_back (x);
        if (w.empty ()) continue;
        auto bad = [&] (const std::string &why) {
            return std::runtime_error (source + " line " + std::to_string (n)
                                       + ": " + why);
        };
        if (w[0] == "proxy" && w.size () == 3) {
            auto sep = w[1].find ("::");
            if (sep == std::string::npos || sep == 0 || sep + 2 == w[1].size ())
                throw bad ("expected: proxy NODE::USER localuser");
            out.proxies_.push_back (Proxy {
                upper (w[1].substr (0, sep)), upper (w[1].substr (sep + 2)),
                w[2] == "-" || w[2] == "*" ? w[2] : upper (w[2]) });
            continue;
        }
        if (w.size () != 4)
            throw bad ("expected: user hash directory ro|rw");
        FalUser u;
        u.name = w[0] == "*" ? "*" : upper (w[0]);
        u.hash = w[1];
        if (u.hash != "-" && u.hash.size () < 13)
            throw bad ("that is not a password hash; see dnfal --hash");
        u.root = w[2][0] == '/' ? w[2] : base + "/" + w[2];
        if (w[3] == "rw")      u.writable = true;
        else if (w[3] != "ro") throw bad ("access must be ro or rw");
        for (const FalUser &o : out.users_)
            if (o.name == u.name) throw bad ("user " + u.name + " twice");
        out.users_.push_back (std::move (u));
    }
    for (const Proxy &p : out.proxies_)
        if (p.local != "-" && !out.find (p.local))
            throw std::runtime_error (source + ": proxy " + p.node + "::"
                                      + p.user + " names " + p.local
                                      + ", who is not in the file");
    return out;
}

const FalUser *FalUsers::find (const std::string &name) const
{
    for (const FalUser &u : users_)
        if (u.name == name) return &u;
    return nullptr;
}

std::optional<FalUser> FalUsers::authenticate (const Requester &r) const
{
    if (!r.proxy || !r.password.empty ())
        return authenticate (r.user, r.password);

    // Proxy: who the user is at the far end.  DNA puts it in the access
    // control user name; some implementations leave that empty and send
    // only the source end user, possibly with a UIC in front.
    std::string who = r.user;
    if (who.empty ()) {
        who = r.source_user;
        if (auto b = who.find (']'); b != std::string::npos) who.erase (0, b + 1);
    }
    who = upper (who);
    std::string name = upper (r.node_name), addr = r.node_address;
    auto node_is = [&] (const std::string &n) {
        return !n.empty () && (n == name || n == addr);
    };

    const Proxy *best = nullptr;
    int best_rank = 0;
    for (const Proxy &p : proxies_) {
        bool nm = node_is (p.node), nw = p.node == "*";
        bool um = !who.empty () && p.user == who, uw = p.user == "*";
        int rank = nm && um ? 4 : nm && uw ? 3 : nw && um ? 2 : nw && uw ? 1 : 0;
        if (rank > best_rank) { best = &p; best_rank = rank; }
    }
    if (best) {
        if (best->local == "-") return std::nullopt;
        if (const FalUser *u = find (best->local)) return *u;
    }
    // No proxy: the default account, if there is one.
    if (const FalUser *u = find ("*")) return *u;
    return std::nullopt;
}

std::optional<FalUser> FalUsers::authenticate (const std::string &user,
                                               const std::string &password) const
{
    std::string want = user.empty () ? "*" : upper (user);
    auto it = std::find_if (users_.begin (), users_.end (),
                            [&] (const FalUser &u) { return u.name == want; });
    if (it == users_.end ()) return std::nullopt;
    if (it->hash == "-") return *it;
    if (check_password (password, it->hash)) return *it;
    // VMS sends a password typed without quotes in upper case.
    std::string lc = lower (password);
    if (lc != password && check_password (lc, it->hash)) return *it;
    return std::nullopt;
}

}   // namespace decnet::dap
