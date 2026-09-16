// decnet/http/server.h -- the monitoring pages.
//
// Port of http.py and html.py.  Read only pages showing circuits,
// adjacencies, nodes and counters.
//
// Page data comes from each layer's nice_read, so the pages show the same
// information NCP can read.  The server thread does socket I/O and posts a
// callback to gather data on the node thread.

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

    // Turn a request into a response without a socket.  For tests.
    Response serve (const Request &req);

private:
    void run ();                        // the accept loop
    void handle (Socket conn);

    // Page builders.  Each returns the body of a page.
    std::string index_page () const;
    std::string entity_page (std::uint8_t kind, unsigned info, bool all) const;

    Node             *node_;
    unsigned          port_;
    Socket            listener_;
    std::thread       thread_;
    std::atomic<bool> stopping_ { false };
};

}   // namespace decnet::http
}   // namespace decnet

#endif
