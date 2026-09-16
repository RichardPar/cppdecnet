// src/http/server.cc -- the monitoring pages.  Port of http.py and html.py.

#include "decnet/http/server.h"

#include "decnet/common/logging.h"
#include "decnet/common/work.h"
#include "decnet/config.h"
#include "decnet/nice/nml.h"
#include "decnet/nice/packets.h"
#include "decnet/node.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <future>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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

const char *style = R"(
body { font-family: monospace; margin: 2em; background: #fbfbf9; color: #222; }
h1 { font-size: 1.3em; } h2 { font-size: 1.1em; margin-top: 1.6em; }
nav a { margin-right: 1em; } nav { margin-bottom: 1.5em; }
table { border-collapse: collapse; margin: 0.5em 0 1.5em 0; }
td, th { border: 1px solid #ccc; padding: 2px 10px; text-align: left;
         vertical-align: top; }
th { background: #eee; }
.entity { font-weight: bold; }
.none { color: #777; font-style: italic; }
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

std::string page (const std::string &title, const std::string &nav,
                  const std::string &body)
{
    std::string out = "<!doctype html>\n<html><head><meta charset=\"utf-8\">"
                      "<title>";
    out += escape (title);
    out += "</title><style>";
    out += style;
    out += "</style></head><body>\n<h1>";
    out += escape (title);
    out += "</h1>\n";
    out += nav;
    out += body;
    out += "</body></html>\n";
    return out;
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
        int fd = ::accept (listener_.fd (), nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) continue;
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
        ssize_t n = ::recv (conn.fd (), tmp, sizeof tmp, 0);
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
                resp.body = page ("Error", "", "<p>" + escape (e.what ()) + "</p>");
            }
        } else {
            // The node loop is not running, or is wedged.  Say so rather
            // than hanging the browser.
            resp.status = 500;
            resp.body = page ("Error", "", "<p class=\"none\">the node did not"
                              " answer</p>");
        }
    } else {
        resp = serve (req);
    }

    Bytes out = resp.encode ();
    std::size_t sent = 0;
    while (sent < out.size ()) {
        ssize_t n = ::send (conn.fd (), out.data () + sent, out.size () - sent,
                            MSG_NOSIGNAL);
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
        r.body = page ("Bad request", "", "<p>only GET is served</p>");
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
    r.body = page ("Not found", "<nav><a href=\"/\">home</a></nav>",
                   "<p>no such page</p>");
    return r;
}

std::string Server::index_page () const
{
    std::string b = "<table>\n";
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
    b += "</table>\n<h2>Entities</h2>\n<ul>\n";
    for (const PageDef &pd : pages) {
        b += "<li><a href=\"/";
        b += pd.slug;
        b += "\">";
        b += pd.title;
        b += "</a></li>\n";
    }
    b += "</ul>\n";
    return page (node_ ? node_->name () : "DECnet", std::string (), b);
}

std::string Server::entity_page (std::uint8_t kind, unsigned info,
                                 bool all) const
{
    const PageDef *pd = nullptr;
    for (const PageDef &d : pages) if (d.kind == kind) pd = &d;
    if (!pd) return page ("Not found", "", "<p>no such entity</p>");

    // Navigation: home, the other entities, and the information levels.
    std::string nav = "<nav><a href=\"/\">home</a>";
    for (const PageDef &d : pages) {
        nav += " <a href=\"/";
        nav += d.slug;
        nav += "\">";
        nav += d.title;
        nav += "</a>";
    }
    nav += "<br>";
    for (const InfoDef &i : infos) {
        nav += " <a href=\"/";
        nav += pd->slug;
        nav += "?info=";
        nav += i.slug;
        nav += "\">";
        nav += i.title;
        nav += "</a>";
    }
    nav += "</nav>\n";

    std::string b;
    if (!node_) return page (pd->title, nav, "<p class=\"none\">no node</p>");

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
        return page (pd->title, nav, b);
    }

    nice::ParamDefs defs = nice::params_for (kind);
    std::vector<std::vector<nice::NiceReply *>> groups = replies.sorted (req);
    if (groups.empty ()) {
        b += "<p class=\"none\">none</p>\n";
        return page (pd->title, nav, b);
    }

    // Hide unreachable unnamed nodes unless all=1.
    std::size_t hidden = 0;
    auto worth_showing = [&] (const nice::NiceReply *rep) {
        if (all || kind != nice::Entity::node) return true;
        const nice::NiceNode &n = rep->entity.as_node ();
        if (n.executor || !n.name.empty ()) return true;
        const nice::Param *state = rep->params.find (0);
        // Unreachable and nameless: nothing a reader is looking for.  Any
        // node carrying more than its state has something to say.
        if (state && state->value.is_number () && state->value.as_uint () == 5
            && rep->params.size () <= 1)
            return false;
        return true;
    };

    for (const auto &group : groups) {
        for (const nice::NiceReply *rep : group) {
            if (!rep) continue;
            if (!worth_showing (rep)) { ++hidden; continue; }
            b += "<h2 class=\"entity\">";
            b += escape (rep->entity.str ());
            b += "</h2>\n";
            std::vector<std::string> lines = rep->params.format (defs);
            if (lines.empty ()) {
                b += "<p class=\"none\">no parameters</p>\n";
                continue;
            }
            b += "<table>\n";
            for (const std::string &line : lines) {
                auto [k, v] = split_param (line);
                b += "<tr><th>";
                b += breaks (escape (k));
                b += "</th><td>";
                b += breaks (escape (v));
                b += "</td></tr>\n";
            }
            b += "</table>\n";
        }
    }
    if (hidden) {
        b += "<p class=\"none\">";
        b += std::to_string (hidden);
        b += " unreachable node";
        b += hidden == 1 ? "" : "s";
        b += " not shown &mdash; <a href=\"/";
        b += pd->slug;
        b += "?all=1";
        if (info != nice::info_summary) {
            for (const InfoDef &i : infos)
                if (i.code == info) { b += "&amp;info="; b += i.slug; }
        }
        b += "\">show every node</a></p>\n";
    }
    return page (pd->title, nav, b);
}

}   // namespace decnet::http
