// dap/fal_users.cc -- who may use the FAL, and where.

#include "decnet/dap/fal_users.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <crypt.h>
#endif

namespace decnet::dap {

namespace {

#ifdef _WIN32

// Windows has no crypt(3).  This is SHA-512-crypt ("$6$"), per Ulrich
// Drepper's "Unix crypt using SHA-256 and SHA-512", on the CNG hash.  Its
// hashes verify with crypt on Linux too; Linux's default yescrypt ("$y$")
// hashes do not verify here, so make users files with dnfal --hash on
// Windows, or with "mkpasswd -m sha-512" elsewhere.

using Digest = std::array<unsigned char, 64>;

Digest sha512 (const std::string &in)
{
    Digest d {};
    if (::BCryptHash (BCRYPT_SHA512_ALG_HANDLE, nullptr, 0,
                      reinterpret_cast<PUCHAR> (const_cast<char *> (in.data ())),
                      static_cast<ULONG> (in.size ()), d.data (),
                      static_cast<ULONG> (d.size ())) != 0)
        throw std::runtime_error ("SHA-512 failed");
    return d;
}

std::string bytes_of (const Digest &d, std::size_t n)
{
    std::string s;
    while (s.size () < n)
        s.append (reinterpret_cast<const char *> (d.data ()),
                  std::min (d.size (), n - s.size ()));
    return s;
}

constexpr char b64[] =
    "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

constexpr const char *sha512_prefix = "$6$";
constexpr unsigned    default_rounds = 5000;

// Hash password with a setting ("$6$[rounds=N$]salt[$...]").  Returns
// "*0" if the setting is not one this understands, as crypt does.
std::string sha512_crypt (const std::string &key, const std::string &setting)
{
    if (setting.compare (0, 3, sha512_prefix) != 0) return "*0";
    std::size_t pos = 3;
    unsigned rounds = default_rounds;
    bool custom_rounds = false;
    if (setting.compare (pos, 7, "rounds=") == 0) {
        std::size_t end = setting.find ('$', pos + 7);
        if (end == std::string::npos) return "*0";
        unsigned long r = std::strtoul (setting.c_str () + pos + 7, nullptr, 10);
        rounds = static_cast<unsigned> (std::clamp (r, 1000ul, 999999999ul));
        custom_rounds = true;
        pos = end + 1;
    }
    std::size_t end = setting.find ('$', pos);
    std::string salt = setting.substr (pos, std::min<std::size_t> (
        16, (end == std::string::npos ? setting.size () : end) - pos));

    Digest b = sha512 (key + salt + key);
    std::string a_in = key + salt + bytes_of (b, key.size ());
    for (std::size_t n = key.size (); n; n >>= 1)
        a_in += (n & 1) ? std::string (reinterpret_cast<char *> (b.data ()), 64)
                        : key;
    Digest a = sha512 (a_in);

    std::string dp_in;
    for (std::size_t i = 0; i < key.size (); ++i) dp_in += key;
    std::string p = bytes_of (sha512 (dp_in), key.size ());

    std::string ds_in;
    for (unsigned i = 0; i < 16u + a[0]; ++i) ds_in += salt;
    std::string s = bytes_of (sha512 (ds_in), salt.size ());

    Digest c = a;
    for (unsigned i = 0; i < rounds; ++i) {
        std::string in;
        std::string cs (reinterpret_cast<char *> (c.data ()), 64);
        in += (i & 1) ? p : cs;
        if (i % 3) in += s;
        if (i % 7) in += p;
        in += (i & 1) ? cs : p;
        c = sha512 (in);
    }

    std::string out = sha512_prefix;
    if (custom_rounds) out += "rounds=" + std::to_string (rounds) + "$";
    out += salt + "$";
    auto put = [&] (unsigned b2, unsigned b1, unsigned b0, int n) {
        unsigned w = (b2 << 16) | (b1 << 8) | b0;
        while (n-- > 0) { out += b64[w & 0x3f]; w >>= 6; }
    };
    static constexpr int order[21][3] = {
        { 0, 21, 42 }, { 22, 43,  1 }, { 44,  2, 23 }, {  3, 24, 45 },
        { 25, 46,  4 }, { 47,  5, 26 }, {  6, 27, 48 }, { 28, 49,  7 },
        { 50,  8, 29 }, {  9, 30, 51 }, { 31, 52, 10 }, { 53, 11, 32 },
        { 12, 33, 54 }, { 34, 55, 13 }, { 56, 14, 35 }, { 15, 36, 57 },
        { 37, 58, 16 }, { 59, 17, 38 }, { 18, 39, 60 }, { 40, 61, 19 },
        { 62, 20, 41 },
    };
    for (const auto &g : order) put (c[g[0]], c[g[1]], c[g[2]], 4);
    put (0, 0, c[63], 2);
    return out;
}

#endif

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

#ifdef _WIN32

bool check_password (const std::string &password, const std::string &hash)
{
    std::string h = sha512_crypt (password, hash);
    if (h[0] == '*') return false;
    return same (h, hash);
}

std::string hash_password (const std::string &password)
{
    unsigned char raw[16];
    if (::BCryptGenRandom (nullptr, raw, sizeof raw,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error ("cannot make a salt");
    std::string salt;
    for (unsigned char c : raw) salt += b64[c & 0x3f];
    return sha512_crypt (password, sha512_prefix + salt);
}

#else

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

#endif

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
        // Rooted, not absolute: on Windows "/srv/pub" has no drive but is
        // still not relative to base.
        u.root = std::filesystem::path (w[2]).has_root_directory ()
                     ? w[2] : base + "/" + w[2];
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
