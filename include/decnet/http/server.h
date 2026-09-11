// decnet/http/server.h -- the monitoring pages.
//
// Port of http.py and html.py.  pydecnet serves a small set of read only
// pages describing what the node is doing: the layers, the circuits, the
// adjacencies, the routing table and the counters.
//
// Two decisions worth stating, because they shape the rest.
//
// The data comes from NICE.  Every layer already answers
// `nice_read (request, replies)` for the network management protocol, and
// that is the same information these pages want.  Asking NICE rather than
// reaching into each layer means one description of what a circuit or a
// node looks like, not two that drift apart -- and anything the monitoring
// pages can show, a remote NCP can read, by construction.
//
// The gathering runs on the node's thread.  A helper thread does the
// blocking socket work and posts a callback to collect the data, exactly
// as the datalinks do for their receive paths.  Layer state is still
// touched from one thread only, which is the rule the whole design rests
// on.

#ifndef DECNET_HTTP_SERVER_H
#define DECNET_HTTP_SERVER_H

#include "decnet/common/element.h"
#include "decnet/common/socket.h"
#include "decnet/common/types.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace decnet {

class Node;

namespace http {

// One parsed request line.  Enough of HTTP/1.1 to serve a browser: a
// method, a path, and the query string that follows a "?".
struct Request {
    std::string method;
    std::string path;
    std::string query;

    // The value of one query parameter, empty if it is not there.
    std::string param (const std::string &name) const;
};

// A response, ready to write.
struct Response {
    int         status = 200;
    std::string content_type = "text/html; charset=utf-8";
    std::string body;

    Bytes encode () const;
};

class Server : public Element {
public:
    Server (Node *node, unsigned port);
    ~Server () override;

    void start ();
    void stop ();

    // Nothing is addressed to this element: the server's thread posts
    // CallbackWork, which dispatches itself.  Required by Element.
    void dispatch (Work &) override {}

    // The port actually bound, which is what a test needs when it asks for
    // port 0 and lets the system choose.
    unsigned port () const noexcept { return port_; }

    // Exposed for testing: turn a request into a response without a socket
    // in the way.  Runs on the caller's thread, so a test drives it
    // directly while the server thread is not running.
    Response serve (const Request &req);

private:
    void run ();                        // the accept loop
    void handle (Socket conn);

    // Page builders.  Each returns the body of a page.
    std::string index_page () const;
    std::string entity_page (std::uint8_t kind, unsigned info) const;

    Node             *node_;
    unsigned          port_;
    Socket            listener_;
    std::thread       thread_;
    std::atomic<bool> stopping_ { false };
};

}   // namespace decnet::http
}   // namespace decnet

#endif
