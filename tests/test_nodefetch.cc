// Node name lists: parsing them, caching them, fetching them over HTTP,
// and the "node @..." configuration lines that name them.
//
// The HTTP tests talk to a server this file starts on a loopback port, so
// nothing here needs the network.

#include "harness.h"

#include "decnet/common/http_client.h"
#include "decnet/common/socket.h"
#include "decnet/config.h"
#include "decnet/node.h"
#include "decnet/nodefetch.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

using namespace decnet;

namespace {

std::string temp_path (const char *stem)
{
    static std::atomic<int> seq { 0 };
    return "/tmp/cppdecnet-test-" + std::string (stem) + "-"
         + std::to_string (::getpid ()) + "-"
         + std::to_string (seq.fetch_add (1));
}

std::string read_file (const std::string &path)
{
    std::ifstream f (path);
    return std::string ((std::istreambuf_iterator<char> (f)),
                        std::istreambuf_iterator<char> ());
}

// A one-shot HTTP server: serves a canned body, and answers 304 when the
// request carries a matching If-Modified-Since.  It handles as many
// connections as asked for, then stops.
class TestServer {
public:
    TestServer (std::string body, std::string last_modified = "")
        : body_ (std::move (body)), last_modified_ (std::move (last_modified))
    {
        SourceAddress any ("127.0.0.1", 0);
        listener_ = any.create_server ();
        if (!listener_) throw std::runtime_error ("cannot listen");
        sockaddr_storage sa {};
        socklen_t len = sizeof sa;
        ::getsockname (listener_.fd (), reinterpret_cast<sockaddr *> (&sa),
                       &len);
        port_ = ntohs (reinterpret_cast<sockaddr_in *> (&sa)->sin_port);
        thread_ = std::thread ([this] { run (); });
    }

    ~TestServer ()
    {
        stopping_ = true;
        listener_.shutdown ();
        if (thread_.joinable ()) thread_.join ();
        listener_.close ();
    }

    std::string url (const char *path = "/list.dat") const
    {
        return "http://127.0.0.1:" + std::to_string (port_) + path;
    }

    // What the last request asked for, so a test can check the header went.
    std::string last_request () const
    {
        std::lock_guard l (m_);
        return last_request_;
    }

    int served () const { return served_.load (); }

private:
    void run ()
    {
        while (!stopping_) {
            PollResult p = poll_socket (listener_.fd (), true, false, 200);
            if (stopping_ || p.error) return;
            if (!p.readable) continue;
            int fd = ::accept (listener_.fd (), nullptr, nullptr);
            if (fd < 0) continue;
            Socket c (fd);
            std::string req;
            char tmp[2048];
            while (req.find ("\r\n\r\n") == std::string::npos) {
                ssize_t n = ::recv (c.fd (), tmp, sizeof tmp, 0);
                if (n <= 0) break;
                req.append (tmp, static_cast<std::size_t> (n));
            }
            {
                std::lock_guard l (m_);
                last_request_ = req;
            }
            bool fresh = !last_modified_.empty ()
                      && req.find ("If-Modified-Since: " + last_modified_)
                             != std::string::npos;
            std::string resp;
            if (fresh) {
                resp = "HTTP/1.1 304 Not Modified\r\n";
                if (!last_modified_.empty ())
                    resp += "Last-Modified: " + last_modified_ + "\r\n";
                resp += "Connection: close\r\n\r\n";
            } else {
                resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n";
                if (!last_modified_.empty ())
                    resp += "Last-Modified: " + last_modified_ + "\r\n";
                resp += "Content-Length: " + std::to_string (body_.size ())
                      + "\r\nConnection: close\r\n\r\n" + body_;
            }
            ::send (c.fd (), resp.data (), resp.size (), MSG_NOSIGNAL);
            c.shutdown ();
            ++served_;
        }
    }

    std::string        body_, last_modified_;
    Socket             listener_;
    std::uint16_t      port_ = 0;
    std::thread        thread_;
    std::atomic<bool>  stopping_ { false };
    std::atomic<int>   served_ { 0 };
    mutable std::mutex m_;
    std::string        last_request_;
};

const char *sample_list =
    "1.1 MAGICA\n"
    "1.2 ERNIE\n"
    "29.150 CPPNOD\n";

}   // namespace

// ------------------------------------------------------------- parsing

DN_TEST (nodefetch, parses_address_and_name_pairs)
{
    std::vector<NodeName> names;
    DN_ASSERT (parse_node_list (sample_list, names));
    DN_ASSERT_EQ (names.size (), 3u);
    DN_ASSERT_EQ (names[0].first.str (), std::string ("1.1"));
    DN_ASSERT_EQ (names[0].second, std::string ("MAGICA"));
    DN_ASSERT_EQ (names[2].first.str (), std::string ("29.150"));
    DN_ASSERT_EQ (names[2].second, std::string ("CPPNOD"));
}

DN_TEST (nodefetch, tolerates_padding_comments_and_crlf)
{
    // What the real file looks like: name padded out, served by an RSX
    // system, and our own cache carries a comment line in front.
    std::vector<NodeName> names;
    DN_ASSERT (parse_node_list (
        "# last-modified: Thu, 17 Sep 2026 22:52:00 GMT\r\n"
        "1.1 MAGICA        \r\n"
        "\r\n"
        "1.2 ERNIE         \r\n", names));
    DN_ASSERT_EQ (names.size (), 2u);
    DN_ASSERT_EQ (names[1].second, std::string ("ERNIE"));
}

DN_TEST (nodefetch, skips_bad_lines_rather_than_failing)
{
    // One bad line should not cost us the other thousand.
    std::vector<NodeName> names;
    DN_ASSERT (parse_node_list ("1.1 MAGICA\nnonsense\n999.999 BAD\n"
                                "1.2 ERNIE\n", names));
    DN_ASSERT_EQ (names.size (), 2u);
}

DN_TEST (nodefetch, an_empty_or_wrong_document_is_not_a_node_list)
{
    std::vector<NodeName> names;
    DN_ASSERT (!parse_node_list ("", names));
    DN_ASSERT (!parse_node_list ("<html><body>not this</body></html>", names));
}

// --------------------------------------------------------------- the URL

DN_TEST (nodefetch, url_parsing)
{
    http::Url u;
    DN_ASSERT (http::Url::parse ("http://mim.softjar.se/hecnet.dat", u));
    DN_ASSERT_EQ (u.host, std::string ("mim.softjar.se"));
    DN_ASSERT_EQ (u.port, 80);
    DN_ASSERT_EQ (u.path, std::string ("/hecnet.dat"));

    DN_ASSERT (http::Url::parse ("http://example.org:8080/a/b", u));
    DN_ASSERT_EQ (u.port, 8080);
    DN_ASSERT_EQ (u.path, std::string ("/a/b"));

    // No path is the root.
    DN_ASSERT (http::Url::parse ("http://example.org", u));
    DN_ASSERT_EQ (u.path, std::string ("/"));

    // There is no TLS here, so https is rejected rather than silently
    // fetched in the clear.
    DN_ASSERT (!http::Url::parse ("https://example.org/x", u));
    DN_ASSERT (!http::Url::parse ("ftp://example.org/x", u));
    DN_ASSERT (!http::Url::parse ("/just/a/path", u));
}

// ------------------------------------------------------------- fetching

DN_TEST (nodefetch, fetches_a_list_over_http)
{
    TestServer srv (sample_list, "Thu, 17 Sep 2026 22:52:00 GMT");
    FetchedList got;
    std::string error;
    DN_ASSERT (fetch_node_list (srv.url (), got, error, 10));
    DN_ASSERT_EQ (error, std::string (""));
    DN_ASSERT (!got.unchanged);
    DN_ASSERT_EQ (got.names.size (), 3u);
    DN_ASSERT_EQ (got.last_modified,
                  std::string ("Thu, 17 Sep 2026 22:52:00 GMT"));
}

DN_TEST (nodefetch, a_matching_validator_gets_not_modified)
{
    const std::string when = "Thu, 17 Sep 2026 22:52:00 GMT";
    TestServer srv (sample_list, when);

    FetchedList got;
    std::string error;
    DN_ASSERT (fetch_node_list (srv.url (), got, error, 10, when));
    // The good outcome: no body, no names, and not an error.
    DN_ASSERT (got.unchanged);
    DN_ASSERT (got.names.empty ());
    DN_ASSERT_EQ (error, std::string (""));
    DN_ASSERT_EQ (got.last_modified, when);
    // And the header really went.
    DN_ASSERT (srv.last_request ().find ("If-Modified-Since: " + when)
               != std::string::npos);
}

DN_TEST (nodefetch, a_stale_validator_gets_the_list_again)
{
    TestServer srv (sample_list, "Thu, 17 Sep 2026 22:52:00 GMT");
    FetchedList got;
    std::string error;
    DN_ASSERT (fetch_node_list (srv.url (), got, error, 10,
                                "Mon, 01 Jan 2024 00:00:00 GMT"));
    DN_ASSERT (!got.unchanged);
    DN_ASSERT_EQ (got.names.size (), 3u);
}

DN_TEST (nodefetch, an_unreachable_server_is_an_error_not_a_crash)
{
    FetchedList got;
    std::string error;
    // Port 1 on loopback: nothing listens there.
    DN_ASSERT (!fetch_node_list ("http://127.0.0.1:1/list.dat", got, error, 3));
    DN_ASSERT (!error.empty ());
}

// --------------------------------------------------------------- caching

DN_TEST (nodefetch, cache_round_trips_through_the_validator)
{
    std::string path = temp_path ("cache");
    std::vector<NodeName> names;
    DN_ASSERT (parse_node_list (sample_list, names));

    const std::string when = "Thu, 17 Sep 2026 22:52:00 GMT";
    std::string error;
    DN_ASSERT (write_node_list (path, names, when, error));
    DN_ASSERT_EQ (read_cached_validator (path), when);

    // What was written is still a plain node list that parses back.
    std::vector<NodeName> back;
    DN_ASSERT (parse_node_list (read_file (path), back));
    DN_ASSERT_EQ (back.size (), names.size ());
    DN_ASSERT_EQ (back[2].second, std::string ("CPPNOD"));

    // And nothing is left lying beside it.
    std::ifstream leftover (path + ".tmp");
    DN_ASSERT (!leftover);

    ::unlink (path.c_str ());
}

DN_TEST (nodefetch, a_cache_with_no_validator_reads_as_none)
{
    std::string path = temp_path ("plain");
    std::vector<NodeName> names;
    parse_node_list (sample_list, names);
    std::string error;
    DN_ASSERT (write_node_list (path, names, "", error));
    DN_ASSERT_EQ (read_cached_validator (path), std::string (""));
    DN_ASSERT_EQ (read_cached_validator ("/nonexistent/file"),
                  std::string (""));
    ::unlink (path.c_str ());
}

// --------------------------------------------------------- configuration

DN_TEST (nodefetch, node_at_file_reads_a_bare_name_list)
{
    // The bug this covers: "node @file" used to re-parse the file as whole
    // configuration lines, so a file of bare "1.1 MAGICA" lines silently
    // loaded nothing at all.
    std::string path = temp_path ("names");
    {
        std::ofstream f (path);
        f << sample_list;
    }
    Config c = Config::from_string (
        "routing 1.500 --type endnode\nnode 1.500 TESTND\n"
        "node @" + path + "\n"
        "circuit mul-0 Multinet 127.0.0.1:1:connect\n");
    // Our own node plus the three from the file.
    DN_ASSERT_EQ (c.nodes ().size (), 4u);
    bool found = false;
    for (const auto &n : c.nodes ())
        if (n.name == "CPPNOD" && n.id.str () == "29.150") found = true;
    DN_ASSERT (found);
    ::unlink (path.c_str ());
}

DN_TEST (nodefetch, node_at_url_registers_a_source_and_needs_a_cache)
{
    // No --cache: refused, because without one a failed fetch would leave
    // the node with no names at all.
    bool threw = false;
    try {
        Config::from_string (
            "routing 1.500 --type endnode\nnode 1.500 TESTND\n"
            "node @hecnet\n"
            "circuit mul-0 Multinet 127.0.0.1:1:connect\n");
    } catch (const std::exception &) { threw = true; }
    DN_ASSERT (threw);

    std::string cache = temp_path ("hecnet");
    Config c = Config::from_string (
        "routing 1.500 --type endnode\nnode 1.500 TESTND\n"
        "node @hecnet --cache " + cache + " --refresh 604800\n"
        "circuit mul-0 Multinet 127.0.0.1:1:connect\n");
    DN_ASSERT_EQ (c.node_sources ().size (), 1u);
    DN_ASSERT_EQ (c.node_sources ()[0].url, std::string (Config::hecnet_url ()));
    DN_ASSERT_EQ (c.node_sources ()[0].refresh, 604800u);
    // A cache that is not there yet is not an error: nothing was fetched.
    DN_ASSERT_EQ (c.nodes ().size (), 1u);
}

DN_TEST (nodefetch, a_url_source_loads_its_cache_at_startup)
{
    std::string cache = temp_path ("warm");
    std::vector<NodeName> names;
    parse_node_list (sample_list, names);
    std::string error;
    DN_ASSERT (write_node_list (cache, names,
                                "Thu, 17 Sep 2026 22:52:00 GMT", error));

    Config c = Config::from_string (
        "routing 1.500 --type endnode\nnode 1.500 TESTND\n"
        "node @hecnet --cache " + cache + "\n"
        "circuit mul-0 Multinet 127.0.0.1:1:connect\n");
    // Ours plus the cached three, and the comment line is not one of them.
    DN_ASSERT_EQ (c.nodes ().size (), 4u);
    // The cached ones are marked as coming from a source, ours is not.
    for (const auto &n : c.nodes ())
        DN_ASSERT_EQ (n.from_source, n.name != "TESTND");
    ::unlink (cache.c_str ());
}

DN_TEST (nodefetch, the_weekly_refresh_is_the_default)
{
    std::string cache = temp_path ("default");
    Config c = Config::from_string (
        "routing 1.500 --type endnode\nnode 1.500 TESTND\n"
        "node @hecnet --cache " + cache + "\n"
        "circuit mul-0 Multinet 127.0.0.1:1:connect\n");
    DN_ASSERT_EQ (c.node_sources ()[0].refresh, 604800u);
}

// ------------------------------------------------- applying to a node

DN_TEST (nodefetch, a_fetched_name_does_not_override_a_configured_one)
{
    std::string cache = temp_path ("override");
    Config c = Config::from_string (
        "routing 1.500 --type endnode\nnode 1.500 TESTND\n"
        "node 1.1 MYOWN\n"
        "node @hecnet --cache " + cache + "\n"
        "circuit mul-0 Multinet 127.0.0.1:1:connect\n");
    Node n (c);

    // 1.1 was named in the configuration, so a refresh leaves it be.
    DN_ASSERT (!n.set_node_name (Nodeid::parse ("1.1"), "MAGICA"));
    DN_ASSERT_EQ (n.find_node (Nodeid::parse ("1.1"))->name,
                  std::string ("MYOWN"));

    // A node the configuration never mentioned takes the fetched name.
    DN_ASSERT (n.set_node_name (Nodeid::parse ("1.2"), "ERNIE"));
    DN_ASSERT_EQ (n.find_node (Nodeid::parse ("1.2"))->name,
                  std::string ("ERNIE"));
    // And it can be looked up by that name afterwards.
    DN_ASSERT (n.find_node ("ERNIE") != nullptr);
}

DN_TEST (nodefetch, renaming_keeps_the_counters_and_delay)
{
    std::string cache = temp_path ("keep");
    Config c = Config::from_string (
        "routing 1.500 --type endnode\nnode 1.500 TESTND\n"
        "node @hecnet --cache " + cache + "\n"
        "circuit mul-0 Multinet 127.0.0.1:1:connect\n");
    Node n (c);

    // Give 1.2 some history, as a conversation would.
    Nodeinfo *info = n.find_node (Nodeid::parse ("1.2"));
    DN_ASSERT (info != nullptr);
    info->counters.con_xmt = 7;
    info->counters.t_byt_rcv = 1234;
    info->delay = 0.5;

    DN_ASSERT (n.set_node_name (Nodeid::parse ("1.2"), "ERNIE"));

    // The rename must not have thrown the entry away and built a new one:
    // that would lose this node's counters and its round trip estimate.
    Nodeinfo *after = n.find_node (Nodeid::parse ("1.2"));
    DN_ASSERT_EQ (after->name, std::string ("ERNIE"));
    DN_ASSERT_EQ (after->counters.con_xmt, 7u);
    DN_ASSERT_EQ (after->counters.t_byt_rcv, 1234u);
    DN_ASSERT_EQ (after->delay, 0.5);

    // Renaming again releases the old name from the by-name index.
    DN_ASSERT (n.set_node_name (Nodeid::parse ("1.2"), "ERNIE2"));
    DN_ASSERT (n.find_node ("ERNIE") == nullptr);
    DN_ASSERT (n.find_node ("ERNIE2") != nullptr);
}
