// src/http/server.cc -- the monitoring pages.  Port of http.py and html.py.

#include "decnet/http/server.h"

#include "decnet/common/logging.h"
#include "decnet/common/work.h"
#include "decnet/config.h"
#include "decnet/nice/nml.h"
#include "decnet/nice/packets.h"
#include "decnet/node.h"
#include "decnet/routing/lan.h"
#include "decnet/routing/routing.h"

#include <cerrno>
#include <cstring>
#include <future>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace decnet::http {

namespace {

// The entity kinds a page is offered for, in the order NCP lists them.
struct PageDef {
    std::uint8_t kind;
    const char  *slug;
    const char  *title;
};

constexpr PageDef pages[] = {
    { nice::Entity::node,    "nodes",    "Nodes"    },
    { nice::Entity::circuit, "circuits", "Circuits" },
    { nice::Entity::line,    "lines",    "Lines"    },
    { nice::Entity::area,    "areas",    "Areas"    },
    { nice::Entity::module,  "modules",  "Modules"  },
    { nice::Entity::logging, "logging",  "Logging"  },
};

struct InfoDef {
    unsigned    code;
    const char *slug;
    const char *title;
};

constexpr InfoDef infos[] = {
    { nice::info_summary,  "summary",  "Summary"         },
    { nice::info_status,   "status",   "Status"          },
    { nice::info_char,     "char",     "Characteristics" },
    { nice::info_counters, "counters", "Counters"        },
};

std::string escape (const std::string &s)
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

// Everything before the first "=", and everything after it.
std::pair<std::string, std::string> split_at (const std::string &s, char c)
{
    std::size_t n = s.find (c);
    if (n == std::string::npos) return { s, std::string () };
    return { s.substr (0, n), s.substr (n + 1) };
}

// The look of every page.  Self contained (no fonts or scripts from
// elsewhere: the node may have no route to the internet), light or dark to
// suit the browser.
const char *style = R"(
:root {
  --bg: #f4f6fa; --panel: #ffffff; --ink: #1c2433; --muted: #6b7486;
  --line: #e3e7ef; --head: #f7f9fc; --accent: #2f6fde; --accent-ink: #ffffff;
  --ok: #1f9d55; --ok-bg: #e5f6ec; --warn: #b7791f; --warn-bg: #fdf3e1;
  --off: #8a93a5; --off-bg: #eef0f4; --bad: #c53030; --bad-bg: #fde8e8;
  --bar1: #14213d; --bar2: #23407a;
  --shadow: 0 1px 2px rgba(20,33,61,.06), 0 4px 14px rgba(20,33,61,.06);
  --mono: ui-monospace, "SF Mono", "JetBrains Mono", Menlo, Consolas,
          "DejaVu Sans Mono", monospace;
  --sans: system-ui, -apple-system, "Segoe UI", Roboto, "Helvetica Neue",
          Arial, sans-serif;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #0f141c; --panel: #171e2a; --ink: #e4e9f2; --muted: #8d97aa;
    --line: #263042; --head: #1c2433; --accent: #5b8ff9; --accent-ink: #0f141c;
    --ok: #48c78e; --ok-bg: #143326; --warn: #e9b85a; --warn-bg: #3a2d12;
    --off: #8d97aa; --off-bg: #222b3a; --bad: #f07178; --bad-bg: #3b1b1f;
    --bar1: #0b1220; --bar2: #18284a;
    --shadow: 0 1px 2px rgba(0,0,0,.3), 0 6px 18px rgba(0,0,0,.25);
  }
}
* { box-sizing: border-box; }
html { background: var(--bg); }
body { margin: 0; font: 15px/1.45 var(--sans); color: var(--ink);
       background: var(--bg); -webkit-font-smoothing: antialiased; }
a { color: var(--accent); text-decoration: none; }
a:hover { text-decoration: underline; }

header.bar { background: linear-gradient(120deg, var(--bar1), var(--bar2));
             color: #fff; padding: 18px 28px 0; }
.brand { display: flex; align-items: center; gap: 14px; flex-wrap: wrap; }
.logo { width: 40px; height: 40px; flex: none; display: block; }
.brand a { color: #fff; }
.brand a.name { display: flex; flex-direction: column; line-height: 1.1; }
.brand a.name:hover { text-decoration: none; }
.brand .net { font-size: 12px; font-weight: 650; letter-spacing: 1.2px;
              opacity: .7; margin-bottom: 2px; }
.brand .name b { font-size: 20px; font-weight: 650; letter-spacing: .3px; }
.brand .addr { font: 13px var(--mono); padding: 2px 8px; border-radius: 999px;
               background: rgba(255,255,255,.14); }
.brand .soft { margin-left: auto; font-size: 12.5px; opacity: .7; }
nav.tabs { display: flex; gap: 2px; margin-top: 16px; overflow-x: auto; }
nav.tabs a { color: rgba(255,255,255,.75); padding: 9px 14px 11px;
             border-radius: 9px 9px 0 0; font-weight: 550; font-size: 14px;
             white-space: nowrap; }
nav.tabs a:hover { color: #fff; background: rgba(255,255,255,.08);
                   text-decoration: none; }
nav.tabs a.on { color: var(--ink); background: var(--bg); }

main { max-width: 1240px; margin: 0 auto; padding: 22px 28px 48px; }
h1 { font-size: 22px; font-weight: 650; margin: 4px 0 16px; }
.toolbar { display: flex; align-items: center; gap: 14px; flex-wrap: wrap;
           margin-bottom: 18px; }
.toolbar h1 { margin: 0; }
.seg { display: inline-flex; background: var(--panel); border-radius: 10px;
       padding: 3px; box-shadow: var(--shadow); margin-left: auto; }
.seg a { padding: 6px 12px; border-radius: 8px; color: var(--muted);
         font-size: 13.5px; font-weight: 550; }
.seg a:hover { color: var(--ink); text-decoration: none; }
.seg a.on { background: var(--accent); color: var(--accent-ink); }

.grid { display: grid; gap: 16px;
        grid-template-columns: repeat(auto-fill, minmax(340px, 1fr)); }
.card { background: var(--panel); border-radius: 12px; box-shadow: var(--shadow);
        overflow: hidden; border: 1px solid var(--line); }
.card-head { display: flex; align-items: center; gap: 8px; padding: 12px 16px;
             border-bottom: 1px solid var(--line); background: var(--head); }
h2.entity { margin: 0; font-size: 15.5px; font-weight: 650; margin-right: auto;
            font-family: var(--mono); }
h2.entity .kind { font: 500 11px var(--sans); text-transform: uppercase;
                  letter-spacing: .8px; color: var(--muted); display: block;
                  margin-bottom: 1px; }
.card .none { padding: 12px 16px; margin: 0; }

.badge { font-size: 12px; font-weight: 600; padding: 3px 9px;
         border-radius: 999px; white-space: nowrap; color: var(--off);
         background: var(--off-bg); }
.badge::before { content: ""; display: inline-block; width: 7px; height: 7px;
                 border-radius: 50%; background: currentColor;
                 margin-right: 6px; vertical-align: 1px; }
.badge.ok   { color: var(--ok);   background: var(--ok-bg); }
.badge.warn { color: var(--warn); background: var(--warn-bg); }
.badge.bad  { color: var(--bad);  background: var(--bad-bg); }

table { border-collapse: collapse; width: 100%; }
td, th { padding: 7px 16px; text-align: left; vertical-align: top;
         border-bottom: 1px solid var(--line); }
tr:last-child td, tr:last-child th { border-bottom: 0; }
th { font-weight: 500; color: var(--muted); width: 46%; font-size: 13.5px; }
td { font: 13.5px var(--mono); word-break: break-word; }
table.counters td { text-align: right; font-variant-numeric: tabular-nums; }
table.counters th { width: auto; }
tbody tr:hover td, tbody tr:hover th { background: var(--head); }

.none { color: var(--muted); font-style: italic; }
.note { display: flex; align-items: center; gap: 14px; flex-wrap: wrap;
        margin-top: 22px; padding: 12px 16px; border-radius: 12px;
        background: var(--panel); border: 1px dashed var(--line); }
.note p { margin: 0; }
a.button { display: inline-block; padding: 7px 14px; border-radius: 9px;
           background: var(--accent); color: var(--accent-ink);
           font-weight: 600; font-size: 13.5px; }
a.button:hover { filter: brightness(1.08); text-decoration: none; }

.hero { display: grid; gap: 16px; margin-bottom: 26px;
        grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); }
.hero .card table th { width: 34%; }
.section { font-size: 13px; font-weight: 650; text-transform: uppercase;
           letter-spacing: .9px; color: var(--muted); margin: 26px 0 12px; }
.tiles { display: grid; gap: 14px;
         grid-template-columns: repeat(auto-fill, minmax(210px, 1fr)); }
a.tile { display: block; padding: 16px 18px; border-radius: 12px;
         background: var(--panel); border: 1px solid var(--line);
         box-shadow: var(--shadow); color: var(--ink);
         transition: transform .12s ease, border-color .12s ease; }
a.tile:hover { transform: translateY(-2px); border-color: var(--accent);
               text-decoration: none; }
a.tile b { display: block; font-size: 15.5px; margin-bottom: 3px; }
a.tile span { color: var(--muted); font-size: 13px; }
table.status td:first-child { font-weight: 600; white-space: nowrap; }
table.status td:nth-child(2) { white-space: nowrap; }
table.status th { width: auto; }
@media (max-width: 640px) {
  header.bar { padding: 14px 16px 0; }
  main { padding: 18px 14px 36px; }
  .grid { grid-template-columns: 1fr; }
  .brand .soft { display: none; }
  td, th { padding: 7px 12px; }
  .seg { margin-left: 0; }
}
footer { text-align: center; color: var(--muted); font-size: 12px;
         padding: 0 0 28px; }
)";

// Split formatter output into name and value.  Parameters are
// "Name = value".  Counters are the count right aligned in 11 columns
// followed by the description, possibly with qualifier lines.
std::pair<std::string, std::string> split_param (const std::string &line)
{
    std::size_t eq = line.find (" = ");
    if (eq != std::string::npos)
        return { line.substr (0, eq), line.substr (eq + 3) };

    // A counter: leading spaces, the count, a space, then the description.
    std::size_t start = line.find_first_not_of (' ');
    if (start == std::string::npos) return { line, std::string () };
    std::size_t sp = line.find (' ', start);
    if (sp == std::string::npos) return { line.substr (start), std::string () };
    return { line.substr (sp + 1), line.substr (start, sp - start) };
}

// Newlines in a counter's qualifier list become line breaks.
std::string breaks (const std::string &s)
{
    std::string out;
    for (char c : s) {
        if (c == '\n') out += "<br>";
        else out += c;
    }
    return out;
}

// The site header: who this is, and the tabs, the one for active marked.
// brand is the node name, addr and soft what is shown beside it; any may
// be empty on an error page, where the node may not be answering.
// The mark: a D struck through its stem, white on a rounded tile.  Drawn
// here, not fetched, as everything on these pages is.
const char *logo_svg =
    "<svg class=\"logo\" viewBox=\"0 0 40 40\" aria-hidden=\"true\">"
    "<rect x=\"1\" y=\"1\" width=\"38\" height=\"38\" rx=\"10\" "
    "fill=\"#ffffff\" fill-opacity=\".12\" stroke=\"#ffffff\" "
    "stroke-opacity=\".28\"/>"
    "<path d=\"M14 10h6.5a10 10 0 0 1 0 20H14z\" fill=\"none\" "
    "stroke=\"#ffffff\" stroke-width=\"3.2\" stroke-linejoin=\"round\"/>"
    "<path d=\"M8.5 20h11\" stroke=\"#7fb2ff\" stroke-width=\"3.2\" "
    "stroke-linecap=\"round\"/></svg>";

// The same mark as the browser tab icon, as a data: URL.
const char *favicon =
    "<link rel=\"icon\" href=\"data:image/svg+xml,"
    "%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 40 40'%3E"
    "%3Crect width='40' height='40' rx='10' fill='%2323407a'/%3E"
    "%3Cpath d='M14 10h6.5a10 10 0 0 1 0 20H14z' fill='none' stroke='white' "
    "stroke-width='3.6' stroke-linejoin='round'/%3E"
    "%3Cpath d='M8.5 20h11' stroke='%237fb2ff' stroke-width='3.6' "
    "stroke-linecap='round'/%3E%3C/svg%3E\">";

std::string header (const std::string &brand, const std::string &addr,
                    const std::string &soft, const char *active)
{
    std::string h = "<header class=\"bar\"><div class=\"brand\">";
    h += logo_svg;
    h += "<a class=\"name\" href=\"/\"><span class=\"net\">HECnet</span><b>";
    h += escape (brand.empty () ? std::string ("DECnet") : brand);
    h += "</b></a>";
    if (!addr.empty ()) {
        h += "<span class=\"addr\">";
        h += escape (addr);
        h += "</span>";
    }
    if (!soft.empty ()) {
        h += "<span class=\"soft\">";
        h += escape (soft);
        h += "</span>";
    }
    h += "</div>\n<nav class=\"tabs\"><a href=\"/\"";
    if (active && !*active) h += " class=\"on\"";
    h += ">Overview</a>";
    for (const PageDef &d : pages) {
        h += "<a href=\"/";
        h += d.slug;
        h += "\"";
        if (active && std::strcmp (active, d.slug) == 0) h += " class=\"on\"";
        h += ">";
        h += d.title;
        h += "</a>";
    }
    h += "</nav></header>\n";
    return h;
}

// A whole page: title for the browser tab, head the header from header(),
// then the body inside main.
std::string page (const std::string &title, const std::string &head,
                  const std::string &body)
{
    std::string out = "<!doctype html>\n<html lang=\"en\"><head>"
                      "<meta charset=\"utf-8\">"
                      "<meta name=\"viewport\" content=\"width=device-width, "
                      "initial-scale=1\">";
    out += favicon;
    out += "<title>";
    out += escape (title);
    out += "</title><style>";
    out += style;
    out += "</style></head><body>\n";
    out += head.empty () ? header ("", "", "", nullptr) : head;
    out += "<main>\n";
    out += body;
    out += "</main>\n<footer>DECnet Phase IV &middot; NICE over HTTP</footer>\n"
           "</body></html>\n";
    return out;
}

// The badge class for a state value: running things green, things that
// are off grey, things on their way amber.
const char *badge_class (const std::string &v)
{
    if (v == "On" || v == "Reachable" || v == "Running") return "ok";
    if (v == "Unreachable" || v == "Off") return "off";
    if (v == "Failed") return "bad";
    return "warn";
}

std::string badge (const std::string &v)
{
    std::string b = "<span class=\"badge ";
    b += badge_class (v);
    b += "\">";
    b += escape (v);
    b += "</span>";
    return b;
}

// "Circuit = MUL-0" as a small kind label over the name.
std::string entity_title (const std::string &s)
{
    std::size_t eq = s.find (" = ");
    if (eq == std::string::npos) return escape (s);
    return "<span class=\"kind\">" + escape (s.substr (0, eq)) + "</span>"
         + escape (s.substr (eq + 3));
}

// The header for a page of this node's, with tab active marked.
std::string site_head (const Node *n, const char *active)
{
    if (!n) return header ("", "", "", active);
    return header (n->name (), n->nicenode ().id.str (),
                   n->software_identification (), active);
}

// A page that is only a message, for errors.
std::string message_page (const std::string &title, const std::string &text,
                          const std::string &head = std::string ())
{
    return page (title, head,
                 "<h1>" + escape (title) + "</h1>\n<div class=\"card\">"
                 "<p class=\"none\">" + escape (text) + "</p></div>\n");
}

// Read every known entity of kind at level info, as NCP would: each one's
// name and its formatted parameter lines, in NCP's order.
struct EntityLines {
    std::string              name;
    std::vector<std::string> lines;
};

std::vector<EntityLines> read_all (const Node *node, std::uint8_t kind,
                                   unsigned info)
{
    std::vector<EntityLines> out;
    if (!node) return out;
    nice::NiceRequest req;
    req.function    = nice::fn_read;
    req.entity_type = kind;
    req.info        = info;
    req.entity      = nice::ReqEntity::make_wild (kind, nice::ReqEntity::known);
    nice::ReplyDict replies (kind, const_cast<Node *> (node));
    if (const_cast<Node *> (node)->nice_read (req, replies) != 0) return out;
    nice::ParamDefs defs = nice::params_for (kind);
    for (const auto &group : replies.sorted (req))
        for (const nice::NiceReply *rep : group)
            if (rep) out.push_back ({ rep->entity.str (), rep->params.format (defs) });
    return out;
}

// The value of parameter name in formatted lines, or empty.
std::string value_of (const std::vector<std::string> &lines,
                      const std::string &name)
{
    for (const std::string &l : lines) {
        auto [k, v] = split_param (l);
        if (k == name) return v;
    }
    return {};
}

}   // namespace

// ------------------------------------------------------------- Request

std::string Request::param (const std::string &name) const
{
    std::string rest = query;
    while (!rest.empty ()) {
        auto [item, tail] = split_at (rest, '&');
        auto [key, value] = split_at (item, '=');
        if (key == name) return value;
        rest = tail;
    }
    return { };
}

// ------------------------------------------------------------ Response

Bytes Response::encode () const
{
    const char *reason = status == 200 ? "OK"
                       : status == 404 ? "Not Found"
                       : status == 400 ? "Bad Request"
                                       : "Error";
    std::string head = "HTTP/1.1 " + std::to_string (status) + " " + reason
                     + "\r\nContent-Type: " + content_type
                     + "\r\nContent-Length: " + std::to_string (body.size ())
                     + "\r\nConnection: close\r\n\r\n";
    Bytes out (head.begin (), head.end ());
    out.insert (out.end (), body.begin (), body.end ());
    return out;
}

// -------------------------------------------------------------- Server

Server::Server (Node *node, unsigned port)
    : Element (node), node_ (node), port_ (port)
{
}

Server::~Server () { stop (); }

void Server::start ()
{
    SourceAddress src ("", static_cast<std::uint16_t> (port_));
    listener_ = src.create_server ();
    if (!listener_) {
        DN_ERROR ("http: cannot listen on port {}", port_);
        return;
    }
    // Port 0 means "any", and a test needs to know which one that was.
    if (port_ == 0) {
        sockaddr_in6 a {};
        socklen_t len = sizeof a;
        if (::getsockname (listener_.fd (), reinterpret_cast<sockaddr *> (&a),
                           &len) == 0)
            port_ = ntohs (a.sin6_port);
    }
    DN_INFO ("http: monitoring on port {}", port_);
    thread_ = std::thread ([this] { run (); });
}

void Server::stop ()
{
    if (!thread_.joinable ()) return;
    stopping_ = true;
    // Closing the listener is what wakes the accept.
    listener_.shutdown ();
    listener_.close ();
    thread_.join ();
}

void Server::run ()
{
    logging::set_thread_name (node_ ? node_->name () : "http");
    while (!stopping_) {
        int fd = sock_accept (listener_.fd ());
        if (fd < 0) {
            if (sock_interrupted (sock_errno ())) continue;
            break;                      // listener closed, or gone
        }
        handle (Socket (fd));
    }
}

void Server::handle (Socket conn)
{
    // Read up to the end of the headers.  A monitoring request has no body,
    // so that is the whole of it.
    std::string buf;
    char tmp[2048];
    while (buf.find ("\r\n\r\n") == std::string::npos && buf.size () < 64 * 1024) {
        ssize_t n = sock_recv (conn.fd (), tmp, sizeof tmp);
        if (n <= 0) break;
        buf.append (tmp, static_cast<std::size_t> (n));
    }

    Request req;
    std::size_t eol = buf.find ("\r\n");
    if (eol != std::string::npos) {
        std::string line = buf.substr (0, eol);
        std::size_t a = line.find (' ');
        std::size_t b = line.rfind (' ');
        if (a != std::string::npos && b != std::string::npos && b > a) {
            req.method = line.substr (0, a);
            std::string target = line.substr (a + 1, b - a - 1);
            auto [path, query] = split_at (target, '?');
            req.path = path;
            req.query = query;
        }
    }

    // Gather data on the node thread.
    Response resp;
    if (node_) {
        std::promise<Response> p;
        std::future<Response> f = p.get_future ();
        node_->add_work (std::make_unique<CallbackWork> (
            [this, &req, &p] {
                try { p.set_value (serve (req)); }
                catch (...) { p.set_exception (std::current_exception ()); }
            }));
        if (f.wait_for (std::chrono::seconds (5)) == std::future_status::ready) {
            try { resp = f.get (); }
            catch (const std::exception &e) {
                resp.status = 500;
                resp.body = message_page ("Error", e.what ());
            }
        } else {
            // The node loop is not running, or is wedged.  Say so rather
            // than hanging the browser.
            resp.status = 500;
            resp.body = message_page ("Error", "the node did not answer");
        }
    } else {
        resp = serve (req);
    }

    Bytes out = resp.encode ();
    std::size_t sent = 0;
    while (sent < out.size ()) {
        ssize_t n = sock_send (conn.fd (), out.data () + sent, out.size () - sent);
        if (n <= 0) break;
        sent += static_cast<std::size_t> (n);
    }
    conn.shutdown ();
}

Response Server::serve (const Request &req)
{
    Response r;
    if (!req.method.empty () && req.method != "GET" && req.method != "HEAD") {
        r.status = 400;
        r.body = message_page ("Bad request", "only GET is served");
        return r;
    }

    std::string path = req.path;
    if (path.empty () || path == "/") {
        r.body = index_page ();
        return r;
    }
    if (!path.empty () && path[0] == '/') path.erase (0, 1);

    unsigned info = nice::info_summary;
    std::string want = req.param ("info");
    for (const InfoDef &i : infos)
        if (want == i.slug) info = i.code;

    for (const PageDef &pd : pages) {
        if (path != pd.slug) continue;
        r.body = entity_page (pd.kind, info, !req.param ("all").empty ());
        return r;
    }

    r.status = 404;
    r.body = message_page ("Not found", "no such page",
                           site_head (node_, nullptr));
    return r;
}

std::string Server::index_page () const
{
    static const char *const blurbs[] = {
        "Names, reachability and routes",
        "Routing circuits and their neighbours",
        "Data links and their traffic",
        "Areas and how to reach them",
        "Configurator and other modules",
        "Event sinks and filters",
    };

    std::string b = "<h1>Overview</h1>\n<div class=\"hero\">\n";
    b += "<div class=\"card\"><div class=\"card-head\"><h2 class=\"entity\">"
         "<span class=\"kind\">Executor</span>";
    b += escape (node_ ? node_->name () : std::string ("DECnet"));
    b += "</h2>";
    if (node_) b += badge ("On");
    b += "</div>\n<table>\n";
    auto row = [&b] (const char *k, const std::string &v) {
        b += "<tr><th>";
        b += k;
        b += "</th><td>";
        b += escape (v);
        b += "</td></tr>\n";
    };
    if (node_) {
        row ("Node", node_->nicenode ().str ());
        row ("Identification", node_->identification ());
        row ("Software", node_->software_identification ());
    }
    b += "</table></div>\n";

    // The circuits at a glance: the first thing anyone looks for.
    std::vector<EntityLines> circuits =
        read_all (node_, nice::Entity::circuit, nice::info_summary);
    b += "<div class=\"card\"><div class=\"card-head\"><h2 class=\"entity\">"
         "<span class=\"kind\">At a glance</span>Circuits</h2>"
         "<a href=\"/circuits?info=status\">details</a></div>\n";
    if (circuits.empty ()) {
        b += "<p class=\"none\">no circuits</p>";
    } else {
        b += "<table class=\"status\">\n";
        for (const EntityLines &c : circuits) {
            std::string name = c.name;
            if (auto eq = name.find (" = "); eq != std::string::npos)
                name = name.substr (eq + 3);
            std::string state = value_of (c.lines, "State");
            std::string sub   = value_of (c.lines, "Substate");
            std::string adj   = value_of (c.lines, "Adjacent node");
            b += "<tr><td>";
            b += escape (name);
            b += "</td><td>";
            if (!state.empty ()) b += badge (sub.empty () ? state : sub);
            b += "</td><td>";
            // A LAN has any number of neighbours, not one: say how many.
            const routing::LanCircuit *lan =
                (adj.empty () && node_ && node_->routing ())
                ? node_->routing ()->lan_circuit (name) : nullptr;
            if (!adj.empty ()) {
                b += escape (adj);
            } else if (lan) {
                std::size_t n = lan->adjacency_count ();
                if (n == 0) {
                    b += "<span class=\"none\">no adjacencies</span>";
                } else {
                    b += std::to_string (n);
                    b += n == 1 ? " adjacency" : " adjacencies";
                }
            } else {
                b += "<span class=\"none\">no neighbour</span>";
            }
            b += "</td></tr>\n";
        }
        b += "</table>";
    }
    b += "</div>\n</div>\n";

    b += "<div class=\"section\">Entities</div>\n<div class=\"tiles\">\n";
    std::size_t i = 0;
    for (const PageDef &pd : pages) {
        b += "<a class=\"tile\" href=\"/";
        b += pd.slug;
        b += "\"><b>";
        b += pd.title;
        b += "</b><span>";
        b += blurbs[i++];
        b += "</span></a>\n";
    }
    b += "</div>\n";
    return page (node_ ? node_->name () + " \xc2\xb7 Overview" : "DECnet",
                 site_head (node_, ""), b);
}

std::string Server::entity_page (std::uint8_t kind, unsigned info,
                                 bool all) const
{
    const PageDef *pd = nullptr;
    for (const PageDef &d : pages) if (d.kind == kind) pd = &d;
    if (!pd) return message_page ("Not found", "no such entity");

    // The header with this entity's tab marked, then a toolbar: the
    // title and the information levels, the current one marked.
    const std::string nav = site_head (node_, pd->slug);
    const std::string title = (node_ ? node_->name () + " \xc2\xb7 " : std::string ())
                            + pd->title;
    std::string toolbar = "<div class=\"toolbar\"><h1>";
    toolbar += pd->title;
    toolbar += "</h1><div class=\"seg\">";
    for (const InfoDef &i : infos) {
        toolbar += "<a href=\"/";
        toolbar += pd->slug;
        toolbar += "?info=";
        toolbar += i.slug;
        toolbar += "\"";
        if (i.code == info) toolbar += " class=\"on\"";
        toolbar += ">";
        toolbar += i.title;
        toolbar += "</a>";
    }
    toolbar += "</div></div>\n";

    std::string b = toolbar;
    if (!node_) return page (title, nav, b + "<p class=\"none\">no node</p>");

    // Ask NICE the same question NCP would: read everything known of this
    // entity, at the requested level of detail.
    nice::NiceRequest req;
    req.function    = nice::fn_read;
    req.entity_type = kind;
    req.info        = info;
    req.entity      = nice::ReqEntity::make_wild (kind, nice::ReqEntity::known);

    nice::ReplyDict replies (kind, const_cast<Node *> (node_));
    // Request all addresses.  The page filters unreachable nodes itself and
    // offers all=1.
    replies.want_every_address (true);
    int err = const_cast<Node *> (node_)->nice_read (req, replies);
    if (err != 0) {
        b += "<p class=\"none\">this node has nothing to say about ";
        b += escape (pd->title);
        b += "</p>\n";
        return page (title, nav, b);
    }

    nice::ParamDefs defs = nice::params_for (kind);
    std::vector<std::vector<nice::NiceReply *>> groups = replies.sorted (req);
    if (groups.empty ()) {
        b += "<p class=\"none\">none</p>\n";
        return page (title, nav, b);
    }

    // An entity with nothing to report is noise: a read of characteristics
    // names every node in the database but only the executor has any, and
    // a node in another area has no routing entry to report either.  Those
    // are hidden unless all=1, which is what the button at the foot sets.
    //
    // Hiding them matters more since node names are fetched: the older
    // test here was "unreachable and nameless", and a list from HECnet
    // leaves nothing nameless.
    std::size_t hidden = 0;
    // "Would be hidden", independent of whether we are hiding: with all=1
    // this is what decides whether offering to hide again makes sense.
    auto is_empty = [&] (const nice::NiceReply *rep,
                         const std::vector<std::string> &lines) {
        if (lines.empty ()) return true;
        // Only the node table has thousands of rows; a circuit or a line is
        // worth showing even when it is idle.
        if (kind != nice::Entity::node) return false;
        const nice::NiceNode &n = rep->entity.as_node ();
        // The executor is always worth showing, whatever it has to say.
        if (n.executor) return false;

        // A counters read gives every node the full set, nearly all of them
        // zero.  "Seconds since last zeroed" is this node's uptime rather
        // than anything about that node, so a set that is otherwise all
        // zero is a node we have never talked to.  Same test as the
        // "significant" one NSP applies, but at the page rather than in the
        // reply, so it covers whatever a layer chose to send.
        bool anything = false;
        for (const auto &[number, p] : rep->params) {
            if (!p.counter) { anything = true; break; }
            if (p.number == 0) continue;            // seconds since zeroed
            if (p.count.value || p.count.map) { anything = true; break; }
        }
        if (!anything) return true;

        if (!n.name.empty ()) return false;
        const nice::Param *state = rep->params.find (0);
        // Unreachable and nameless: nothing a reader is looking for.  Any
        // node carrying more than its state has something to say.
        if (state && state->value.is_number () && state->value.as_uint () == 5
            && rep->params.size () <= 1)
            return true;
        return false;
    };

    // One card per entity, its state as a badge in the corner.
    const bool counters = info == nice::info_counters;
    b += "<div class=\"grid\">\n";
    for (const auto &group : groups) {
        for (const nice::NiceReply *rep : group) {
            if (!rep) continue;
            std::vector<std::string> lines = rep->params.format (defs);
            if (is_empty (rep, lines)) {
                ++hidden;
                if (!all) continue;
            }
            b += "<section class=\"card\"><div class=\"card-head\">"
                 "<h2 class=\"entity\">";
            b += entity_title (rep->entity.str ());
            b += "</h2>";
            std::string state = value_of (lines, "State");
            if (!state.empty ()) b += badge (state);
            std::string sub = value_of (lines, "Substate");
            if (!sub.empty ()) b += badge (sub);
            b += "</div>\n";
            if (lines.empty ()) {
                b += "<p class=\"none\">no parameters</p></section>\n";
                continue;
            }
            b += counters ? "<table class=\"counters\">\n" : "<table>\n";
            for (const std::string &line : lines) {
                auto [k, v] = split_param (line);
                b += "<tr><th>";
                b += breaks (escape (k));
                b += "</th><td>";
                b += breaks (escape (v));
                b += "</td></tr>\n";
            }
            b += "</table></section>\n";
        }
    }
    b += "</div>\n";

    // The link back to this same page with the other setting of "all".
    auto same_page = [&] (bool want_all) {
        std::string href = "/";
        href += pd->slug;
        const char *sep = "?";
        if (want_all) { href += "?all=1"; sep = "&amp;"; }
        if (info != nice::info_summary)
            for (const InfoDef &i : infos)
                if (i.code == info) {
                    href += sep;
                    href += "info=";
                    href += i.slug;
                }
        return href;
    };

    if (hidden && !all) {
        b += "<div class=\"note\"><p class=\"none\">";
        b += std::to_string (hidden);
        b += hidden == 1 ? " entry with nothing to report is not shown"
                         : " entries with nothing to report are not shown";
        b += "</p><a class=\"button\" href=\"";
        b += same_page (true);
        b += "\">Show all</a></div>\n";
    } else if (hidden && all) {
        // Showing them all, so offer the way back -- but only when there
        // is something to hide, or the button would do nothing.
        b += "<div class=\"note\"><a class=\"button\" href=\"";
        b += same_page (false);
        b += "\">Hide empty entries</a></div>\n";
    }
    return page (title, nav, b);
}

}   // namespace decnet::http
