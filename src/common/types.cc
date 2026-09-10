#include "decnet/common/types.h"

#include <cctype>
#include <charconv>
#include <cstdio>
#include <stdexcept>

namespace decnet {

namespace {

// Parse an unsigned decimal number occupying all of s.
unsigned parse_uint (std::string_view s, const char *what)
{
    unsigned v = 0;
    const char *first = s.data ();
    const char *last  = s.data () + s.size ();
    auto [ptr, ec] = std::from_chars (first, last, v);
    if (ec != std::errc () || ptr != last)
        throw std::invalid_argument (std::string ("bad ") + what + ": "
                                     + std::string (s));
    return v;
}

int hexval (char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}   // namespace

// ---------------------------------------------------------------- Nodeid

Nodeid Nodeid::parse (std::string_view s)
{
    auto dot = s.find ('.');
    if (dot == std::string_view::npos) {
        unsigned tid = parse_uint (s, "node id");
        if (tid > 1023)
            throw std::invalid_argument ("node number out of range");
        return Nodeid (0u, tid);
    }
    unsigned area = parse_uint (s.substr (0, dot), "area number");
    unsigned tid  = parse_uint (s.substr (dot + 1), "node number");
    if (area < 1 || area > 63)
        throw std::invalid_argument ("area number out of range");
    if (tid < 1 || tid > 1023)
        throw std::invalid_argument ("node number out of range");
    return Nodeid (area, tid);
}

std::string Nodeid::str () const
{
    char buf[16];
    if (area () == 0)
        std::snprintf (buf, sizeof buf, "%u", tid ());
    else
        std::snprintf (buf, sizeof buf, "%u.%u", area (), tid ());
    return buf;
}

// --------------------------------------------------------------- Macaddr

Macaddr Macaddr::parse (std::string_view s)
{
    std::array<std::uint8_t, 6> b {};
    std::size_t pos = 0;
    for (int i = 0; i < 6; ++i) {
        if (i) {
            if (pos >= s.size () || (s[pos] != '-' && s[pos] != ':'))
                throw std::invalid_argument ("bad MAC address: "
                                             + std::string (s));
            ++pos;
        }
        if (pos + 1 >= s.size ())
            throw std::invalid_argument ("bad MAC address: " + std::string (s));
        int hi = hexval (s[pos]), lo = hexval (s[pos + 1]);
        if (hi < 0 || lo < 0)
            throw std::invalid_argument ("bad MAC address: " + std::string (s));
        b[static_cast<std::size_t> (i)] =
            static_cast<std::uint8_t> ((hi << 4) | lo);
        pos += 2;
    }
    if (pos != s.size ())
        throw std::invalid_argument ("bad MAC address: " + std::string (s));
    return Macaddr (b);
}

Macaddr Macaddr::from_nodeid (Nodeid n)
{
    // The DECnet Phase IV "HIORD" prefix, AA-00-04-00, followed by the node
    // address in little endian order.
    std::array<std::uint8_t, 6> b { 0xaa, 0x00, 0x04, 0x00, 0, 0 };
    b[4] = static_cast<std::uint8_t> (n.value () & 0xff);
    b[5] = static_cast<std::uint8_t> (n.value () >> 8);
    return Macaddr (b);
}

std::string Macaddr::str () const
{
    char buf[24];
    std::snprintf (buf, sizeof buf, "%02x-%02x-%02x-%02x-%02x-%02x",
                   bytes_[0], bytes_[1], bytes_[2],
                   bytes_[3], bytes_[4], bytes_[5]);
    return buf;
}

// --------------------------------------------------------------- Version

Version Version::parse (std::string_view s)
{
    Version v;
    auto d1 = s.find ('.');
    if (d1 == std::string_view::npos)
        throw std::invalid_argument ("bad version: " + std::string (s));
    auto d2 = s.find ('.', d1 + 1);
    if (d2 == std::string_view::npos)
        throw std::invalid_argument ("bad version: " + std::string (s));
    v.v1 = static_cast<std::uint8_t> (parse_uint (s.substr (0, d1), "version"));
    v.v2 = static_cast<std::uint8_t> (
        parse_uint (s.substr (d1 + 1, d2 - d1 - 1), "version"));
    v.v3 = static_cast<std::uint8_t> (parse_uint (s.substr (d2 + 1), "version"));
    return v;
}

std::string Version::str () const
{
    char buf[16];
    std::snprintf (buf, sizeof buf, "%u.%u.%u", v1, v2, v3);
    return buf;
}

// ------------------------------------------------------------------ misc

std::string nodename (std::string_view s)
{
    bool has_alpha = false;
    for (char c : s) {
        if (std::isalpha (static_cast<unsigned char> (c))) has_alpha = true;
        else if (!std::isdigit (static_cast<unsigned char> (c)))
            throw std::invalid_argument ("invalid node name: "
                                         + std::string (s));
    }
    if (!has_alpha || s.empty () || s.size () > 6)
        throw std::invalid_argument ("invalid node name: " + std::string (s));
    std::string out (s);
    for (char &c : out)
        c = static_cast<char> (std::toupper (static_cast<unsigned char> (c)));
    return out;
}

std::string circname (std::string_view s)
{
    if (s.empty () || !std::isalpha (static_cast<unsigned char> (s[0])))
        throw std::invalid_argument ("invalid circuit name: "
                                     + std::string (s));
    std::size_t i = 0;
    while (i < s.size () && std::isalpha (static_cast<unsigned char> (s[i])))
        ++i;
    for (; i < s.size (); ++i)
        if (!std::isdigit (static_cast<unsigned char> (s[i])) && s[i] != '-')
            throw std::invalid_argument ("invalid circuit name: "
                                         + std::string (s));
    std::string out (s);
    for (char &c : out)
        c = static_cast<char> (std::toupper (static_cast<unsigned char> (c)));
    return out;
}

Phase phase_of (NodeType t) noexcept
{
    switch (t) {
    case NodeType::phase2:                                  return Phase::ph2;
    case NodeType::phase3router: case NodeType::phase3endnode: return Phase::ph3;
    default:                                                return Phase::ph4;
    }
}

const char *name_of (NodeType t) noexcept
{
    switch (t) {
    case NodeType::l2router:       return "l2router";
    case NodeType::l1router:       return "l1router";
    case NodeType::endnode:        return "endnode";
    case NodeType::phase3router:   return "phase3router";
    case NodeType::phase3endnode:  return "phase3endnode";
    case NodeType::phase2:         return "phase2";
    }
    return "?";
}

std::string hexdump (ByteView b, std::size_t bytes_per_line)
{
    std::string out;
    char buf[8];
    for (std::size_t i = 0; i < b.size (); ++i) {
        if (i && i % bytes_per_line == 0) out += '\n';
        else if (i)                       out += ' ';
        std::snprintf (buf, sizeof buf, "%02x", b[i]);
        out += buf;
    }
    return out;
}

}   // namespace decnet
