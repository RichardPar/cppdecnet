// src/http/monitor.cc -- the HTTP monitoring interface.
//
// Port of http.py and html.py.  See the header for why the pages are
// rendered on the node thread rather than on the server's own.

#include "decnet/http/monitor.h"

#include "decnet/common/logging.h"
#include "decnet/config.h"
#include "decnet/datalink/bc.h"
#include "decnet/datalink/datalink.h"
#include "decnet/events/logger.h"
#include "decnet/mop/mop.h"
#include "decnet/node.h"
#include "decnet/nsp/nsp.h"
#include "decnet/routing/lan.h"
#include "decnet/routing/ptp.h"
#include "decnet/routing/routing.h"
#include "decnet/session/session.h"
#include "decnet/version.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <condition_variable>
#include <future>
#include <sstream>

namespace decnet::http {

namespace {

// ------------------------------------------------------------- HTML bits

// Escape the five characters that matter.  Every string that reaches a
// page goes through this: node names, circuit names and event text all
// come from the wire or from a configuration file, and neither is a place
// to trust.
std::string esc (const std::string &s)
{
    std::string out;
    out.reserve (s.size ());
    for (char c : s) {
        switch (c) {
        case '&':  out += "&amp;";  break;
        case '<':  out += "&lt;";   break;
        case '>':  out += "&gt;";   break;
        case '"':  out += "&quot;"; break;
        case '\'': out += "&#39;";  break;
        default:   out += c;        break;
        }
    }
    return out;
}

// The style sheet, inline so the server has exactly one thing to serve and
// no resource directory to find.  Dark and light both come from the
// reader's own setting rather than from a toggle here.
constexpr const char *stylesheet = R"(
:root { color-scheme: light dark;
        --fg: #111; --bg: #fff; --dim: #666; --line: #d4d4d4;
        --head: #f4f4f4; --accent: #24608f; }
@media (prefers-color-scheme: dark) {
  :root { --fg: #ddd; --bg: #16181a; --dim: #999; --line: #33383d;
          --head: #212529; --accent: #74add1; } }
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--fg);
       font: 14px/1.5 -apple-system, "Segoe UI", Roboto, Helvetica, sans-serif; }
header { border-bottom: 1px solid var(--line); padding: 12px 16px; }
header h1 { margin: 0; font-size: 16px; font-weight: 600; }
header .sub { color: var(--dim); font-size: 12px; }
nav { padding: 8px 16px; border-bottom: 1px solid var(--line); }
nav a { color: var(--accent); text-decoration: none; margin-right: 14px; }
nav a:hover { text-decoration: underline; }
nav a.on { font-weight: 600; text-decoration: underline; }
main { padding: 16px; }
h2 { font-size: 14px; margin: 20px 0 8px; }
h2:first-child { margin-top: 0; }
table { border-collapse: collapse; width: 100%; max-width: 100%;
        margin-bottom: 8px; }
th, td { text-align: left; padding: 5px 10px 5px 0;
         border-bottom: 1px solid var(--line); vertical-align: top; }
th { background: var(--head); font-weight: 600; white-space: nowrap;
     padding-left: 8px; }
td { padding-left: 8px; }
td.num { text-align: right; font-variant-numeric: tabular-nums; }
.mono { font-family: ui-monospace, Menlo, Consolas, monospace; font-size: 12px; }
.dim { color: var(--dim); }
.none { color: var(--dim); font-style: italic; }
.wrap { overflow-x: auto; }
footer { padding: 12px 16px; color: var(--dim); font-size: 12px;
         border-top: 1px solid var(--line); }
@media (max-width: 500px) { th, td { padding-left: 6px; } }
)";

struct Page {
    std::ostringstream out;
    Page (const std::string &title, const std::string &subtitle,
          const std::string &current)
    {
        out << "<!doctype html><html><head><meta charset=\"utf-8\">"
            << "<meta name=\"viewport\" content=\"width=device-width, "
               "initial-scale=1\">"
            << "<title>" << esc (title) << "</title><style>" << stylesheet
            << "</style></head><body><header><h1>" << esc (title)
            << "</h1><div class=\"sub\">" << esc (subtitle)
            << "</div></header><nav>";
        static const struct { const char *href, *label; } tabs[] = {
            { "/", "Overview" }, { "/routing", "Routing" },
            { "/nsp", "Links" }, { "/mop", "MOP" }, { "/events", "Events" },
        };
        for (const auto &t : tabs)
            out << "<a href=\"" << t.href << "\""
                << (current == t.href ? " class=\"on\"" : "") << ">"
                << t.label << "</a>";
        out << "</nav><main>";
    }

    std::string done ()
    {
        out << "</main><footer>" << esc (version::ident ())
            << "</footer></body></html>";
        return out.str ();
    }
};

// A table.  Small helper rather than a template, because every table here
// is built the same way: a header row, then rows of already formatted
// cells.
class Table {
public:
    explicit Table (std::initializer_list<const char *> headings)
    {
        out_ << "<div class=\"wrap\"><table><tr>";
        for (const char *h : headings) out_ << "<th>" << h << "</th>";
        out_ << "</tr>";
    }

    void row (std::initializer_list<std::string> cells)
    {
        out_ << "<tr>";
        for (const std::string &c : cells) out_ << "<td>" << c << "</td>";
        out_ << "</tr>";
        ++rows_;
    }

    // Finish, or report that there was nothing to show.  A table with no
    // rows says so rather than appearing as a bare header.
    std::string done (const char *empty_message)
    {
        if (!rows_) return std::string ("<p class=\"none\">") + empty_message
                         + "</p>";
        out_ << "</table></div>";
        return out_.str ();
    }

private:
    std::ostringstream out_;
    unsigned           rows_ = 0;
};

std::string num (unsigned long long v) { return std::to_string (v); }
std::string mono (const std::string &s)
{ return "<span class=\"mono\">" + esc (s) + "</span>"; }

// A node as "1.20 (CPPGW)", or just the address when there is no name.
std::string node_str (Node *n, Nodeid id)
{
    if (!n) return esc (id.str ());
    nice::NiceNode nn = n->nicenode (id);
    if (nn.name.empty ()) return mono (id.str ());
    return mono (id.str ()) + " (" + esc (nn.name) + ")";
}

const char *node_type_name (NodeType t)
{
    switch (t) {
    case NodeType::l2router:      return "area router";
    case NodeType::l1router:      return "level 1 router";
    case NodeType::endnode:       return "endnode";
    case NodeType::phase3router:  return "Phase III router";
    case NodeType::phase3endnode: return "Phase III endnode";
    case NodeType::phase2:        return "Phase II";
    }
    return "?";
}

// Seconds as "3d 04:11:52", which is what a monitoring page wants rather
// than a raw count.
std::string duration (unsigned long secs)
{
    char buf[64];
    unsigned long d = secs / 86400;
    unsigned long h = (secs / 3600) % 24;
    unsigned long m = (secs / 60) % 60;
    unsigned long s = secs % 60;
    if (d) std::snprintf (buf, sizeof buf, "%lud %02lu:%02lu:%02lu", d, h, m, s);
    else   std::snprintf (buf, sizeof buf, "%02lu:%02lu:%02lu", h, m, s);
    return buf;
}

std::string subtitle (Node *n)
{
    if (!n) return "";
    std::string s = n->name ().empty () ? n->id ().str ()
                                        : n->name () + "  " + n->id ().str ();
    if (!n->identification ().empty ()) s += "  -  " + n->identification ();
    return s;
}

}   // namespace

// ------------------------------------------------------------- the pages

std::string page_overview (Node *n)
{
    Page p ("DECnet monitor", subtitle (n), "/");
    if (!n) { p.out << "<p class=\"none\">No node.</p>"; return p.done (); }

    p.out << "<h2>Executor</h2>";
    {
        Table t ({ "", "" });
        t.row ({ "Name", n->name ().empty () ? "<span class=\"none\">none</span>"
                                             : esc (n->name ()) });
        t.row ({ "Address", mono (n->id ().str ()) });
        t.row ({ "Type", n->routing () ? node_type_name (
                             n->config ().routing ()
                                 ? n->config ().routing ()->type
                                 : NodeType::endnode)
                                       : "no routing layer" });
        t.row ({ "Identification", esc (n->identification ()) });
        t.row ({ "Software", esc (n->software_identification ()) });
        t.row ({ "Uptime", duration (n->seconds_since_zeroed ()) });
        p.out << t.done ("");
    }

    p.out << "<h2>Circuits</h2>";
    {
        Table t ({ "Circuit", "Kind", "State", "Neighbour" });
        if (auto *r = n->routing ()) {
            for (const routing::PtpCircuit *c : r->circuits ()) {
                auto *cc = const_cast<routing::PtpCircuit *> (c);
                bool up = cc->running ();
                t.row ({ mono (c->name ()), "point to point",
                         up ? "running" : esc (cc->state_name ()),
                         up ? node_str (n, cc->neighbour ())
                            : "<span class=\"none\">-</span>" });
            }
            for (const routing::LanCircuit *c : r->lan_circuits ()) {
                auto *lc = const_cast<routing::LanCircuit *> (c);
                t.row ({ mono (c->name ()), "broadcast", "on",
                         num (lc->adjacency_count ()) + " adjacent" });
            }
        }
        p.out << t.done ("No circuits configured.");
    }

    p.out << "<h2>Objects</h2>";
    {
        Table t ({ "Number", "Name" });
        if (auto *s = n->session ())
            for (std::size_t i = 0; i < s->object_count (); ++i) {
                // The database is small and the accessors are by key, so
                // walk the numbers rather than adding an iterator for one
                // caller.
                (void) i;
            }
        for (unsigned no = 1; no < 256; ++no) {
            auto *s = n->session ();
            if (!s) break;
            const session::Object *o = s->find_object (
                static_cast<std::uint8_t> (no));
            if (!o) continue;
            t.row ({ num (o->number), o->name.empty ()
                         ? "<span class=\"none\">-</span>" : esc (o->name) });
        }
        p.out << t.done ("No objects registered.");
    }
    return p.done ();
}

std::string page_routing (Node *n)
{
    Page p ("Routing", subtitle (n), "/routing");
    auto *r = n ? n->routing () : nullptr;
    if (!r) { p.out << "<p class=\"none\">No routing layer.</p>";
              return p.done (); }

    p.out << "<h2>Point to point circuits</h2>";
    {
        Table t ({ "Circuit", "State", "Neighbour", "Type", "Block size",
                   "Hello" });
        for (routing::PtpCircuit *c : r->circuits ()) {
            bool up = c->running ();
            std::string ntype = "-";
            if (up) {
                switch (c->neighbour_type ()) {
                case routing::ENDNODE:  ntype = "endnode"; break;
                case routing::L1ROUTER: ntype = "level 1 router"; break;
                case routing::L2ROUTER: ntype = "area router"; break;
                default: ntype = num (c->neighbour_type ()); break;
                }
            }
            t.row ({ mono (c->name ()),
                     up ? "running" : esc (c->state_name ()),
                     up ? node_str (n, c->neighbour ())
                        : "<span class=\"none\">-</span>",
                     esc (ntype),
                     up ? num (c->blksize ()) : std::string ("-"),
                     duration (static_cast<unsigned long> (c->t3 ())) });
        }
        p.out << t.done ("None configured.");
    }

    p.out << "<h2>Broadcast circuits</h2>";
    {
        Table t ({ "Circuit", "Priority", "Designated router", "Adjacencies" });
        for (routing::LanCircuit *c : r->lan_circuits ()) {
            Nodeid dr = c->designated_router ();
            t.row ({ mono (c->name ()), num (c->priority ()),
                     dr.value () ? node_str (n, dr)
                                 : "<span class=\"none\">none</span>",
                     num (c->adjacency_count ()) });
        }
        p.out << t.done ("None configured.");
    }

    p.out << "<h2>Adjacencies</h2>";
    {
        Table t ({ "Node", "Block size", "Priority", "Listen timer" });
        for (const auto &[key, adj] : r->adjacencies ()) {
            if (!adj) continue;
            t.row ({ node_str (n, adj->nodeid ()), num (adj->blksize ()),
                     num (adj->priority ()),
                     duration (static_cast<unsigned long>
                               (adj->listen_time ())) });
        }
        p.out << t.done ("No adjacencies.");
    }
    return p.done ();
}

std::string page_nsp (Node *n)
{
    Page p ("Logical links", subtitle (n), "/nsp");
    auto *nsp = n ? n->nsp () : nullptr;
    if (!nsp) { p.out << "<p class=\"none\">No NSP layer.</p>";
                return p.done (); }

    p.out << "<h2>Connections</h2>";
    {
        Table t ({ "Local", "Remote", "Node", "State", "Segment size" });
        for (const auto &[addr, c] : nsp->connections ()) {
            if (!c) continue;
            t.row ({ num (c->srcaddr ()), num (c->dstaddr ()),
                     node_str (n, c->dest ()), esc (c->state_name ()),
                     c->segsize () ? num (c->segsize ())
                                   : std::string ("-") });
        }
        p.out << t.done ("No logical links.");
    }

    p.out << "<h2>Nodes</h2>";
    {
        Table t ({ "Node", "Name", "Active links" });
        for (const Nodeinfo *info : n->known_nodes ()) {
            unsigned links = nsp->links_to (info->id);
            t.row ({ mono (info->id.str ()),
                     info->name.empty () ? "<span class=\"none\">-</span>"
                                         : esc (info->name),
                     links ? num (links) : std::string ("0") });
        }
        p.out << t.done ("The node database is empty.");
    }
    return p.done ();
}

std::string page_mop (Node *n)
{
    Page p ("MOP", subtitle (n), "/mop");
    auto *mop = n ? n->mop () : nullptr;
    if (!mop) { p.out << "<p class=\"none\">MOP is not running.</p>";
                return p.done (); }

    bool any = false;
    for (mop::MopCircuit *c : mop->circuits ()) {
        mop::SysIdHandler *sysid = c->sysid ();
        if (!sysid) continue;
        any = true;
        p.out << "<h2>Circuit " << esc (c->name ()) << "</h2>"
              << "<p class=\"dim\">Listening for "
              << duration (static_cast<unsigned long> (sysid->elapsed ()))
              << ".</p>";
        Table t ({ "Address", "Device", "Software", "Version", "Buffer",
                   "Last heard" });
        for (const auto &[key, h] : sysid->heard ()) {
            const mop::SysId &s = h.sysid;
            std::chrono::duration<double> ago =
                std::chrono::steady_clock::now () - h.last_heard;
            t.row ({ mono (h.address.str ()),
                     s.device ? num (*s.device) : std::string ("-"),
                     s.software ? esc (s.software->str ()) : std::string ("-"),
                     s.version ? mono (s.version->str ()) : std::string ("-"),
                     s.bufsize ? num (*s.bufsize) : std::string ("-"),
                     duration (static_cast<unsigned long> (ago.count ()))
                         + " ago" });
        }
        p.out << t.done ("Nothing heard on this circuit yet.");
    }
    if (!any)
        p.out << "<p class=\"none\">No circuit has MOP enabled.  Add --mop "
                 "to a broadcast circuit to turn it on.</p>";
    return p.done ();
}

std::string page_events (Node *n, const std::deque<events::Event> &recent)
{
    Page p ("Events", subtitle (n), "/events");
    p.out << "<h2>Recent events</h2>";
    Table t ({ "Time", "Event", "Source", "Entity", "Parameters" });
    // Newest first: a monitoring page is read from the top.
    for (auto it = recent.rbegin (); it != recent.rend (); ++it) {
        const events::Event &e = *it;
        const events::EventDef *def = events::find_event (e.id);
        std::string params;
        for (const std::string &line : e.params.format (nice::params_for (
                                           e.entity.kind ()))) {
            if (!params.empty ()) params += "<br>";
            params += esc (line);
        }
        t.row ({ mono (e.timestamp ()),
                 mono (e.id.str ()) + " "
                     + esc (def ? def->text : "unknown"),
                 node_str (n, e.source.id),
                 esc (e.entity.str ()),
                 params.empty () ? "<span class=\"none\">-</span>" : params });
    }
    p.out << t.done ("No events recorded yet.  Add a \"logging monitor\" line "
                     "to the configuration to collect them here.");
    return p.done ();
}

std::string page_not_found (const std::string &path)
{
    Page p ("Not found", "", "");
    p.out << "<p>There is no page at " << mono (path) << ".</p>";
    return p.done ();
}

// ------------------------------------------------------------- the server

bool parse_request_line (const std::string &line, Request &out)
{
    std::istringstream in (line);
    std::string version;
    if (!(in >> out.method >> out.path)) return false;
    in >> version;      // ignored; we answer HTTP/1.1 either way
    if (out.path.empty () || out.path[0] != '/') return false;
    if (auto q = out.path.find ('?'); q != std::string::npos) {
        out.query = out.path.substr (q + 1);
        out.path.erase (q);
    }
    // A trailing slash on anything but the root names the same page.
    if (out.path.size () > 1 && out.path.back () == '/') out.path.pop_back ();
    return true;
}

Response render (Node *n, const Request &req,
                 const std::deque<events::Event> &recent)
{
    Response r;
    if (req.method != "GET" && req.method != "HEAD") {
        r.status = 405;
        r.body = page_not_found (req.path);
        return r;
    }
    if (req.path == "/")             r.body = page_overview (n);
    else if (req.path == "/routing") r.body = page_routing (n);
    else if (req.path == "/nsp")     r.body = page_nsp (n);
    else if (req.path == "/mop")     r.body = page_mop (n);
    else if (req.path == "/events")  r.body = page_events (n, recent);
    else { r.status = 404; r.body = page_not_found (req.path); }
    return r;
}

HttpMonitor::HttpMonitor (Element *parent, const Config &config)
    : Element (parent), want_port_ (config.http_port ())
{
    DN_DEBUG ("initializing http monitor on port {}", want_port_);
}

HttpMonitor::~HttpMonitor () { stop (); }

void HttpMonitor::start ()
{
    SourceAddress addr ("", static_cast<std::uint16_t> (want_port_));
    listener_ = addr.create_server ();
    if (!listener_) {
        DN_ERROR ("cannot listen on port {} for the monitoring interface",
                  want_port_);
        return;
    }
    // Report the port actually bound: asking for zero means "any free
    // port", and the caller has no other way to learn which one.
    sockaddr_storage sa {};
    socklen_t len = sizeof sa;
    if (::getsockname (listener_.fd (),
                       reinterpret_cast<sockaddr *> (&sa), &len) == 0)
        bound_port_ = ntohs (reinterpret_cast<sockaddr_in *> (&sa)->sin_port);

    stopping_ = false;
    thread_ = std::thread ([this] { serve (); });
    DN_INFO ("monitoring interface on http://localhost:{}/", bound_port_);
}

void HttpMonitor::stop ()
{
    if (!thread_.joinable ()) return;
    stopping_ = true;
    // Closing the listener wakes the accept loop out of its poll.
    listener_.shutdown ();
    thread_.join ();
    listener_.close ();
}

void HttpMonitor::record (const events::Event &e)
{
    std::lock_guard l (events_mutex_);
    recent_.push_back (e);
    while (recent_.size () > max_events) recent_.pop_front ();
}

void HttpMonitor::serve ()
{
    while (!stopping_) {
        PollResult p = poll_socket (listener_.fd (), true, false, 200);
        if (stopping_) return;
        if (p.error) return;
        if (p.timeout || !p.readable) continue;

        int fd = ::accept (listener_.fd (), nullptr, nullptr);
        if (fd < 0) continue;
        handle (Socket (fd));
    }
}

void HttpMonitor::handle (Socket client)
{
    // Read until the end of the headers.  A monitoring request has no body
    // and is tiny, so a bounded single read loop is the whole parser.
    std::string buf;
    constexpr std::size_t max_request = 8192;
    while (buf.find ("\r\n\r\n") == std::string::npos
           && buf.find ("\n\n") == std::string::npos) {
        if (buf.size () > max_request) return;
        PollResult p = poll_socket (client.fd (), true, false, 2000);
        if (p.timeout || p.error || !p.readable) return;
        char tmp[1024];
        ssize_t n = ::read (client.fd (), tmp, sizeof tmp);
        if (n <= 0) return;
        buf.append (tmp, static_cast<std::size_t> (n));
    }

    std::string line = buf.substr (0, buf.find_first_of ("\r\n"));
    Request req;
    Response resp;
    if (!parse_request_line (line, req)) {
        resp.status = 400;
        resp.body = page_not_found ("/");
    } else {
        resp = render_on_node (req);
    }

    const char *reason = resp.status == 200 ? "OK"
                       : resp.status == 404 ? "Not Found"
                       : resp.status == 405 ? "Method Not Allowed"
                       : resp.status == 503 ? "Service Unavailable"
                                            : "Bad Request";
    std::ostringstream head;
    head << "HTTP/1.1 " << resp.status << " " << reason << "\r\n"
         << "Content-Type: " << resp.content_type << "\r\n"
         << "Content-Length: " << resp.body.size () << "\r\n"
         << "Cache-Control: no-store\r\n"
         << "Connection: close\r\n\r\n";
    std::string out = head.str ();
    if (req.method != "HEAD") out += resp.body;

    std::size_t off = 0;
    while (off < out.size ()) {
        ssize_t n = ::write (client.fd (), out.data () + off, out.size () - off);
        if (n <= 0) break;
        off += static_cast<std::size_t> (n);
    }
}

Response HttpMonitor::render_on_node (const Request &req)
{
    Node *n = node ();
    if (!n) { Response r; r.status = 503; r.body = page_not_found (req.path);
              return r; }

    // Hand the work to the node thread and wait for it.  If the node is
    // stopping, the item may never run, so the wait is bounded and a
    // timeout answers "unavailable" rather than hanging the connection.
    auto promise = std::make_shared<std::promise<Response>> ();
    std::future<Response> f = promise->get_future ();
    std::deque<events::Event> recent;
    {
        std::lock_guard l (events_mutex_);
        recent = recent_;
    }
    n->add_work (std::make_unique<CallbackWork> (
        [this, req, promise, recent = std::move (recent)] () mutable {
            try {
                promise->set_value (render (node (), req, recent));
            } catch (const std::exception &e) {
                DN_ERROR ("error building monitoring page {}: {}", req.path,
                          e.what ());
                Response r;
                r.status = 503;
                r.body = page_not_found (req.path);
                promise->set_value (std::move (r));
            }
        }));

    if (f.wait_for (std::chrono::seconds (5)) != std::future_status::ready) {
        Response r;
        r.status = 503;
        r.body = page_not_found (req.path);
        return r;
    }
    return f.get ();
}

}   // namespace decnet::http
