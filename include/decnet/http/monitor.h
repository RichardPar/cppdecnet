// decnet/http/monitor.h -- the HTTP monitoring interface.
//
// Port of http.py and html.py.  A small web server that shows what the
// node is doing: its identity, its circuits and adjacencies, the logical
// links NSP is carrying, what MOP has heard on each LAN, and the recent
// event records.
//
// The one design decision worth stating is where the pages are rendered.
// The node is single threaded by design -- every layer assumes it is the
// only thing touching its state -- so the server thread must not read that
// state directly.  Instead it posts a CallbackWork to the node and waits
// for it: the page is built on the node thread, between two work items,
// with the same guarantees every other part of the system has.  A request
// therefore costs one round trip through the work queue, which is the
// price of not having to lock anything.
//
// PORT: the Python also serves HTTPS, a REST style API under /api, and the
// network map; and it can front several nodes in one process, which is why
// its URLs start with a node name.  This serves one node over plain HTTP.
// See NOTDONE.md.

#ifndef DECNET_HTTP_MONITOR_H
#define DECNET_HTTP_MONITOR_H

#include "decnet/common/element.h"
#include "decnet/common/socket.h"
#include "decnet/events/events.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace decnet {
class Config;
class Node;
}

namespace decnet::http {

// One decoded request line.  Only the path matters here; the method is
// checked and everything else is ignored.
struct Request {
    std::string method;
    std::string path;
    std::string query;
};

// A response, ready to write.
struct Response {
    unsigned    status = 200;
    std::string content_type = "text/html; charset=utf-8";
    std::string body;
};

// The pages.  Free functions taking the node, so they can be tested
// without a socket anywhere near them.  Each must be called on the node
// thread; the server arranges that.
std::string page_overview (Node *n);
std::string page_routing (Node *n);
std::string page_nsp (Node *n);
std::string page_mop (Node *n);
std::string page_events (Node *n, const std::deque<events::Event> &recent);
std::string page_not_found (const std::string &path);

// Route a request to a page.  Exposed for the tests.
Response render (Node *n, const Request &req,
                 const std::deque<events::Event> &recent);

// Parse the first line of an HTTP request.  Returns false if it is not
// something we can answer.
bool parse_request_line (const std::string &line, Request &out);

class HttpMonitor : public Element {
public:
    HttpMonitor (Element *parent, const Config &config);
    ~HttpMonitor () override;

    void start ();
    void stop ();

    void dispatch (Work &) override {}

    // The port actually bound, which is what the tests need when they ask
    // for port 0 and let the kernel choose.
    std::uint16_t port () const noexcept { return bound_port_; }

    // Record an event for the events page.  Called from the node thread by
    // the event logger's monitor sink.
    void record (const events::Event &e);

private:
    void serve ();                      // the accept loop
    void handle (Socket client);        // one connection

    // Build a page on the node thread and return it here.  This is the
    // hop described at the top of the file.
    Response render_on_node (const Request &req);

    unsigned          want_port_;
    std::uint16_t     bound_port_ = 0;
    Socket            listener_;
    std::thread       thread_;
    std::atomic<bool> stopping_ { false };

    // The recent events ring.  Written on the node thread, read on the
    // node thread (inside the render callback), so the mutex is only
    // guarding against the two ends of that hop, not against the layers.
    std::mutex                 events_mutex_;
    std::deque<events::Event>  recent_;
    static constexpr std::size_t max_events = 100;
};

}   // namespace decnet::http

#endif  // DECNET_HTTP_MONITOR_H
