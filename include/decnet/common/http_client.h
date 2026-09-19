// decnet/common/http_client.h -- a minimal HTTP client.
//
// Enough of HTTP/1.1 to fetch one small document over plain TCP: a GET,
// a status line, headers, and a body delimited by Content-Length, by
// chunked encoding or by the connection closing.
//
// There is no TLS, so only http:// URLs work.  The one thing this is for
// -- the HECnet node name list at mim.softjar.se -- is served over plain
// HTTP, and linking a TLS library for it would be a large dependency for
// a small job.  See NOTDONE.md.
//
// This blocks, so it is called on a thread of its own, never on a node
// thread.

#ifndef DECNET_COMMON_HTTP_CLIENT_H
#define DECNET_COMMON_HTTP_CLIENT_H

#include <cstdint>
#include <string>

namespace decnet::http {

// A parsed http:// URL.
struct Url {
    std::string   host;
    std::uint16_t port = 80;
    std::string   path = "/";

    // Parse an http:// URL.  Returns false for anything else, https://
    // included: there is no TLS here.
    static bool parse (const std::string &text, Url &out);

    std::string str () const;
};

// The result of a fetch.  A conditional request that the server answers
// with 304 sets not_modified rather than ok: there is no body, and that is
// the good outcome, not a failure.
struct Fetch {
    bool        ok = false;
    bool        not_modified = false;
    unsigned    status = 0;
    std::string body;
    std::string error;

    // The validator to send back next time, from the Last-Modified header.
    // Empty when the server did not offer one.
    std::string last_modified;
};

// Fetch a document.  Follows up to max_redirects redirections, and gives
// up after timeout_seconds in total.  Never throws.
//
// if_modified_since, when not empty, is sent as the header of that name --
// a Last-Modified value kept from a previous fetch.  A server that
// understands it answers 304 with no body when the document has not
// changed, which is what makes a weekly refresh cost one small exchange
// rather than the whole list.
Fetch get (const std::string &url, int timeout_seconds = 30,
           unsigned max_redirects = 3, std::size_t max_bytes = 8u << 20,
           const std::string &if_modified_since = std::string ());

}   // namespace decnet::http

#endif  // DECNET_COMMON_HTTP_CLIENT_H
