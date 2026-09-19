// src/common/nodefetch.cc -- fetching node name lists over HTTP.

#include "decnet/nodefetch.h"

#include "decnet/common/http_client.h"
#include "decnet/common/logging.h"
#include "decnet/common/work.h"
#include "decnet/node.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace decnet {

bool parse_node_list (const std::string &text, std::vector<NodeName> &out)
{
    std::istringstream in (text);
    std::string line;
    unsigned lineno = 0, bad = 0;
    while (std::getline (in, line)) {
        ++lineno;
        // Tolerate CRLF: the list is served by an RSX system.
        if (!line.empty () && line.back () == '\r') line.pop_back ();
        if (auto hash = line.find ('#'); hash != std::string::npos)
            line.erase (hash);
        std::istringstream ls (line);
        std::string addr, name;
        if (!(ls >> addr)) continue;            // blank
        if (!(ls >> name)) { ++bad; continue; } // an address with no name
        try {
            out.emplace_back (Nodeid::parse (addr), name);
        } catch (const std::exception &) {
            ++bad;
            DN_TRACE ("node list line {} is not an address and name: {}",
                      lineno, line);
        }
    }
    if (bad)
        DN_DEBUG ("node list: skipped {} unparsable line(s) of {}", bad,
                  lineno);
    return !out.empty ();
}

// The comment the cache carries its validator on.
constexpr const char *validator_tag = "# last-modified: ";

bool fetch_node_list (const std::string &url, FetchedList &out,
                      std::string &error, int timeout_seconds,
                      const std::string &if_modified_since)
{
    http::Fetch f = http::get (url, timeout_seconds, 3, 8u << 20,
                               if_modified_since);
    if (f.not_modified) {
        out.unchanged = true;
        out.last_modified = f.last_modified.empty () ? if_modified_since
                                                     : f.last_modified;
        return true;
    }
    if (!f.ok) {
        error = f.error.empty () ? "fetch failed" : f.error;
        return false;
    }
    if (!parse_node_list (f.body, out.names)) {
        error = "nothing in the response looked like a node list";
        return false;
    }
    out.last_modified = f.last_modified;
    return true;
}

std::string read_cached_validator (const std::string &path)
{
    std::ifstream f (path);
    if (!f) return {};
    std::string line;
    // The tag is written first, but tolerate a few lines of anything else.
    for (int i = 0; i < 8 && std::getline (f, line); ++i) {
        if (!line.empty () && line.back () == '\r') line.pop_back ();
        std::string lower = line;
        for (char &c : lower)
            c = static_cast<char> (std::tolower (
                    static_cast<unsigned char> (c)));
        if (lower.rfind (validator_tag, 0) == 0)
            return line.substr (std::char_traits<char>::length (validator_tag));
    }
    return {};
}

bool write_node_list (const std::string &path,
                      const std::vector<NodeName> &names,
                      const std::string &last_modified, std::string &error)
{
    // Write beside the target and rename, so a reader never sees a partial
    // file and a failure leaves the previous one intact.
    std::string tmp = path + ".tmp";
    {
        std::ofstream f (tmp, std::ios::trunc);
        if (!f) { error = "cannot write " + tmp; return false; }
        if (!last_modified.empty ())
            f << validator_tag << last_modified << '\n';
        for (const auto &[id, name] : names)
            f << id.str () << ' ' << name << '\n';
        f.flush ();
        if (!f) { error = "error writing " + tmp; return false; }
    }
    if (std::rename (tmp.c_str (), path.c_str ()) != 0) {
        error = "cannot rename " + tmp + " to " + path;
        ::unlink (tmp.c_str ());
        return false;
    }
    return true;
}

// ------------------------------------------------------------ NodeFetcher

NodeFetcher::NodeFetcher (Element *parent, std::vector<NodeSourceConfig> sources)
    : Element (parent), sources_ (std::move (sources))
{
}

NodeFetcher::~NodeFetcher () { stop (); }

void NodeFetcher::start ()
{
    if (sources_.empty ()) return;
    {
        std::lock_guard l (mutex_);
        stopping_ = false;
    }
    thread_ = std::thread ([this] { run (); });
}

void NodeFetcher::stop ()
{
    if (!thread_.joinable ()) return;
    {
        std::lock_guard l (mutex_);
        stopping_ = true;
    }
    wake_.notify_all ();
    thread_.join ();
}

void NodeFetcher::run ()
{
    logging::set_thread_name (node () ? node ()->name () : "nodefetch");

    for (;;) {
        fetch_all ();

        // The shortest refresh among the sources decides how long to wait;
        // a source set to refresh zero is fetched at startup only.
        unsigned wait = 0;
        for (const NodeSourceConfig &s : sources_)
            if (s.refresh && (wait == 0 || s.refresh < wait)) wait = s.refresh;

        std::unique_lock l (mutex_);
        if (stopping_) return;
        if (wait == 0) {
            // Nothing to do again, but the thread must still wake for stop.
            wake_.wait (l, [this] { return stopping_; });
            return;
        }
        if (wake_.wait_for (l, std::chrono::seconds (wait),
                            [this] { return stopping_; }))
            return;
    }
}

void NodeFetcher::fetch_all ()
{
    for (const NodeSourceConfig &src : sources_) {
        {
            std::lock_guard l (mutex_);
            if (stopping_) return;
        }
        FetchedList got;
        std::string error;
        // Ask only for what has changed since the cache was written.
        std::string since = read_cached_validator (src.cache);
        DN_DEBUG ("fetching node names from {}{}", src.url,
                  since.empty () ? "" : " (if modified since " + since + ")");
        if (!fetch_node_list (src.url, got, error, 30, since)) {
            // Not fatal: the cache, however old, is still in use.
            DN_WARN ("cannot fetch node names from {}: {}", src.url, error);
            continue;
        }
        if (got.unchanged) {
            DN_DEBUG ("node names at {} unchanged since {}", src.url, since);
            continue;
        }
        DN_INFO ("fetched {} node names from {}", got.names.size (), src.url);

        if (!write_node_list (src.cache, got.names, got.last_modified, error))
            DN_WARN ("fetched node names but could not update {}: {}",
                        src.cache, error);

        // Applying them touches the node database, so it happens on the
        // node thread like every other change to layer state.
        Node *n = node ();
        if (!n) continue;
        n->add_work (std::make_unique<CallbackWork> (
            [n, names = std::move (got.names)] () mutable {
                std::size_t changed = 0;
                for (const auto &[id, name] : names)
                    if (n->set_node_name (id, name)) ++changed;
                if (changed)
                    DN_INFO ("node name database updated, {} name(s) changed",
                             changed);
            }));
    }
}

}   // namespace decnet
