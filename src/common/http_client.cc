// src/common/http_client.cc -- a minimal HTTP client.  See the header.

#include "decnet/common/http_client.h"

#include "decnet/common/logging.h"
#include "decnet/common/socket.h"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>

namespace decnet::http {

namespace {

std::string lower (std::string s)
{
    for (char &c : s)
        c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
    return s;
}

bool starts_with (const std::string &s, const char *p)
{
    return s.compare (0, std::char_traits<char>::length (p), p) == 0;
}

// One header value, matched without regard to case.  Empty if absent.
std::string header (const std::string &headers, const char *name)
{
    std::string want = lower (name);
    std::size_t pos = 0;
    while (pos < headers.size ()) {
        std::size_t eol = headers.find ("\r\n", pos);
        if (eol == std::string::npos) eol = headers.size ();
        std::size_t colon = headers.find (':', pos);
        if (colon != std::string::npos && colon < eol) {
            if (lower (headers.substr (pos, colon - pos)) == want) {
                std::size_t b = headers.find_first_not_of (" \t", colon + 1);
                if (b == std::string::npos || b > eol) return {};
                return headers.substr (b, eol - b);
            }
        }
        pos = eol + 2;
    }
    return {};
}

// Milliseconds left before the deadline, floored at zero.
int remaining_ms (std::chrono::steady_clock::time_point deadline)
{
    auto left = std::chrono::duration_cast<std::chrono::milliseconds> (
        deadline - std::chrono::steady_clock::now ()).count ();
    return left <= 0 ? 0 : static_cast<int> (left);
}

// Read until the deadline, appending to buf.  Returns false on error or
// timeout; end of stream is success with eof set.
bool read_some (int fd, std::string &buf, bool &eof,
                std::chrono::steady_clock::time_point deadline,
                std::size_t max_bytes, std::string &error)
{
    int ms = remaining_ms (deadline);
    if (ms == 0) { error = "timed out"; return false; }
    PollResult p = poll_socket (fd, true, false, std::min (ms, 1000));
    if (p.error) { error = "connection error"; return false; }
    if (p.timeout) return true;         // nothing yet; the caller loops
    if (!p.readable) return true;
    char tmp[8192];
    ssize_t n = ::recv (fd, tmp, sizeof tmp, 0);
    if (n < 0) { error = "read failed"; return false; }
    if (n == 0) { eof = true; return true; }
    if (buf.size () + static_cast<std::size_t> (n) > max_bytes) {
        error = "response too large";
        return false;
    }
    buf.append (tmp, static_cast<std::size_t> (n));
    return true;
}

// Undo chunked transfer encoding.  Returns false if the body is malformed.
bool dechunk (const std::string &in, std::string &out)
{
    std::size_t pos = 0;
    for (;;) {
        std::size_t eol = in.find ("\r\n", pos);
        if (eol == std::string::npos) return false;
        // The size line may carry chunk extensions after a ";".
        std::string sizeline = in.substr (pos, eol - pos);
        if (auto semi = sizeline.find (';'); semi != std::string::npos)
            sizeline.erase (semi);
        char *end = nullptr;
        unsigned long len = std::strtoul (sizeline.c_str (), &end, 16);
        if (end == sizeline.c_str ()) return false;
        pos = eol + 2;
        if (len == 0) return true;      // the trailer is of no interest
        if (pos + len > in.size ()) return false;
        out.append (in, pos, len);
        pos += len + 2;                 // the CRLF after the chunk
    }
}

Fetch get_once (const Url &url, std::chrono::steady_clock::time_point deadline,
                std::size_t max_bytes, std::string &redirect,
                const std::string &if_modified_since)
{
    Fetch r;
    HostAddress dest (url.host, url.port);
    SourceAddress any ("", 0);
    Socket s = create_connection (dest, any);
    if (!s) { r.error = "cannot connect to " + url.str (); return r; }

    // create_connection is non-blocking, so wait for the connect to finish.
    int ms = remaining_ms (deadline);
    PollResult p = poll_socket (s.fd (), false, true, ms ? ms : 1);
    if (!p.writable || p.error) {
        r.error = p.timeout ? "connect timed out" : "connect failed";
        return r;
    }
    int err = 0;
    socklen_t elen = sizeof err;
    if (::getsockopt (s.fd (), SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err) {
        r.error = "connect failed";
        return r;
    }

    std::string req = "GET " + url.path + " HTTP/1.1\r\nHost: " + url.host;
    if (url.port != 80) req += ":" + std::to_string (url.port);
    req += "\r\nUser-Agent: cppdecnet\r\nAccept: */*\r\n";
    if (!if_modified_since.empty ())
        req += "If-Modified-Since: " + if_modified_since + "\r\n";
    req += "Connection: close\r\n\r\n";
    std::size_t off = 0;
    while (off < req.size ()) {
        ssize_t n = ::send (s.fd (), req.data () + off, req.size () - off,
                            MSG_NOSIGNAL);
        if (n <= 0) { r.error = "send failed"; return r; }
        off += static_cast<std::size_t> (n);
    }

    // Headers first.
    std::string buf;
    bool eof = false;
    std::size_t hdr_end = std::string::npos;
    while (!eof) {
        hdr_end = buf.find ("\r\n\r\n");
        if (hdr_end != std::string::npos) break;
        if (!read_some (s.fd (), buf, eof, deadline, max_bytes, r.error))
            return r;
    }
    if (hdr_end == std::string::npos) hdr_end = buf.find ("\r\n\r\n");
    if (hdr_end == std::string::npos) { r.error = "no headers"; return r; }

    std::string head = buf.substr (0, hdr_end + 2);
    std::string body = buf.substr (hdr_end + 4);

    std::size_t sp = head.find (' ');
    if (sp == std::string::npos) { r.error = "bad status line"; return r; }
    r.status = static_cast<unsigned> (std::strtoul (head.c_str () + sp + 1,
                                                    nullptr, 10));

    r.last_modified = header (head, "Last-Modified");

    // Not modified: no body follows, and nothing more is needed.
    if (r.status == 304) {
        r.not_modified = true;
        return r;
    }

    std::string location = header (head, "Location");
    if (r.status >= 300 && r.status < 400 && !location.empty ()) {
        redirect = location;
        return r;                       // the caller follows it
    }

    bool chunked = lower (header (head, "Transfer-Encoding")).find ("chunked")
                   != std::string::npos;
    std::string clen = header (head, "Content-Length");
    std::size_t want = clen.empty () ? 0
                                     : static_cast<std::size_t> (
                                           std::strtoul (clen.c_str (), nullptr,
                                                         10));

    // Then the body: to the declared length, to the end of the chunks, or
    // to end of stream when the server says neither.
    while (!eof) {
        if (chunked) {
            std::string decoded;
            if (dechunk (body, decoded)) break;
        } else if (!clen.empty () && body.size () >= want) {
            break;
        }
        std::string more;
        if (!read_some (s.fd (), more, eof, deadline, max_bytes, r.error))
            return r;
        body += more;
    }

    if (chunked) {
        std::string decoded;
        if (!dechunk (body, decoded)) { r.error = "bad chunked body"; return r; }
        body = std::move (decoded);
    } else if (!clen.empty ()) {
        if (body.size () < want) {
            r.error = "short body: got " + std::to_string (body.size ())
                    + " of " + clen + " bytes";
            return r;
        }
        body.resize (want);
    }

    r.body = std::move (body);
    r.ok = (r.status == 200);
    if (!r.ok && r.error.empty ())
        r.error = "HTTP status " + std::to_string (r.status);
    return r;
}

}   // namespace

bool Url::parse (const std::string &text, Url &out)
{
    if (!starts_with (lower (text), "http://")) return false;
    std::string rest = text.substr (7);
    std::size_t slash = rest.find ('/');
    std::string hostport = rest.substr (0, slash);
    out.path = (slash == std::string::npos) ? "/" : rest.substr (slash);
    if (out.path.empty ()) out.path = "/";
    if (auto colon = hostport.rfind (':');
        colon != std::string::npos && hostport.find (']') == std::string::npos) {
        out.host = hostport.substr (0, colon);
        unsigned long p = std::strtoul (hostport.c_str () + colon + 1, nullptr,
                                        10);
        if (p == 0 || p > 65535) return false;
        out.port = static_cast<std::uint16_t> (p);
    } else {
        out.host = hostport;
        out.port = 80;
    }
    return !out.host.empty ();
}

std::string Url::str () const
{
    std::string s = "http://" + host;
    if (port != 80) s += ":" + std::to_string (port);
    return s + path;
}

Fetch get (const std::string &url, int timeout_seconds,
           unsigned max_redirects, std::size_t max_bytes,
           const std::string &if_modified_since)
{
    Fetch r;
    std::string target = url;
    auto deadline = std::chrono::steady_clock::now ()
                  + std::chrono::seconds (timeout_seconds);

    for (unsigned hop = 0; hop <= max_redirects; ++hop) {
        Url u;
        if (!Url::parse (target, u)) {
            r.error = "not a plain http:// URL: " + target;
            return r;
        }
        std::string redirect;
        try {
            r = get_once (u, deadline, max_bytes, redirect, if_modified_since);
        } catch (const std::exception &e) {
            r = Fetch {};
            r.error = e.what ();
            return r;
        }
        if (redirect.empty ()) return r;
        // A relative redirect keeps the host we already have.
        if (starts_with (lower (redirect), "http://")
            || starts_with (lower (redirect), "https://")) {
            target = redirect;
        } else {
            if (redirect.empty () || redirect[0] != '/') redirect = "/" + redirect;
            target = "http://" + u.host
                   + (u.port == 80 ? "" : ":" + std::to_string (u.port))
                   + redirect;
        }
        DN_DEBUG ("http: redirected to {}", target);
    }
    r = Fetch {};
    r.error = "too many redirects";
    return r;
}

}   // namespace decnet::http
