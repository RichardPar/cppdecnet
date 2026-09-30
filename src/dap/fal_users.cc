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
    return out;
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
