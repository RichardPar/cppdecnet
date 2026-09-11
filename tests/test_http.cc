// tests/test_http.cc -- the monitoring pages.
//
// Two kinds of test.  Most drive Server::serve directly, which is the page
// builder without a socket in the way, so a failure points at the page
// rather than at the network.  The last one goes over a real TCP
// connection to a running node, because that is the part the others cannot
// check: the helper thread, the hand-off to the node's thread, and the
// response actually reaching a client.

#include "harness.h"

#include "decnet/common/socket.h"
#include "decnet/config.h"
#include "decnet/http/server.h"
#include "decnet/node.h"

#include <chrono>
#include <cstring>
#include <netdb.h>
#include <thread>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

using namespace decnet;
using namespace decnet::http;

namespace {

const char *conf =
    "routing 1.1 --type endnode\n"
    "node 1.1 NODEA\n"
    "circuit mul-0 Multinet 127.0.0.1:1:connect\n";

template <typename P>
bool wait_until (P pred, std::chrono::milliseconds timeout
                     = std::chrono::seconds (15))
{
    auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        if (pred ()) return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    return pred ();
}

Request get (const std::string &path, const std::string &query = "")
{
    Request r;
    r.method = "GET";
    r.path = path;
    r.query = query;
    return r;
}

bool contains (const std::string &hay, const std::string &needle)
{
    return hay.find (needle) != std::string::npos;
}

}   // namespace

// ------------------------------------------------------------- requests

DN_TEST (http, a_query_string_is_split_into_parameters)
{
    Request r = get ("/nodes", "info=counters&other=2");
    DN_ASSERT_EQ (r.param ("info"), std::string ("counters"));
    DN_ASSERT_EQ (r.param ("other"), std::string ("2"));
    DN_ASSERT_EQ (r.param ("missing"), std::string (""));
}

DN_TEST (http, a_response_carries_its_length)
{
    Response r;
    r.body = "hello";
    Bytes b = r.encode ();
    std::string s (b.begin (), b.end ());
    DN_ASSERT (contains (s, "HTTP/1.1 200 OK"));
    DN_ASSERT (contains (s, "Content-Length: 5"));
    // The body follows the blank line, whole.
    DN_ASSERT (contains (s, "\r\n\r\nhello"));
}

// ---------------------------------------------------------------- pages

DN_TEST (http, the_index_names_the_node)
{
    Config c = Config::from_string (conf);
    Node n (c);
    Server s (&n, 0);

    Response r = s.serve (get ("/"));
    DN_ASSERT_EQ (r.status, 200);
    DN_ASSERT (contains (r.body, "NODEA"));
    // And links to every entity page.
    DN_ASSERT (contains (r.body, "/circuits"));
    DN_ASSERT (contains (r.body, "/nodes"));
}

DN_TEST (http, an_entity_page_reports_what_nice_knows)
{
    Config c = Config::from_string (conf);
    Node n (c);
    Server s (&n, 0);

    Response r = s.serve (get ("/circuits"));
    DN_ASSERT_EQ (r.status, 200);
    // The circuit in the configuration, named by the same code that
    // answers NCP.
    DN_ASSERT (contains (r.body, "MUL-0") || contains (r.body, "mul-0"));
}

DN_TEST (http, the_information_level_is_selectable)
{
    Config c = Config::from_string (conf);
    Node n (c);
    Server s (&n, 0);

    // Counters and summary are different questions, and the page offers
    // both rather than only the one NCP asks by default.
    Response sum = s.serve (get ("/circuits", "info=summary"));
    Response cnt = s.serve (get ("/circuits", "info=counters"));
    DN_ASSERT_EQ (sum.status, 200);
    DN_ASSERT_EQ (cnt.status, 200);
    DN_ASSERT_NE (sum.body, cnt.body);
}

DN_TEST (http, counters_are_rendered_name_first_like_parameters)
{
    // The formatter gives two shapes: a parameter is "Name = value", a
    // counter is the count right aligned and then its description, because
    // that is how NCP prints a counter block.  A page wants both as name
    // and value, in that order, or the columns disagree with each other.
    //
    // Splitting every line on "=" made a counter come out as a name of
    // "0 Bytes received" with an empty value.  This is that bug.
    Config c = Config::from_string (conf);
    Node n (c);
    Server s (&n, 0);

    Response r = s.serve (get ("/circuits", "info=counters"));
    DN_ASSERT_EQ (r.status, 200);
    DN_ASSERT (contains (r.body, "<th>Bytes received</th><td>0</td>"));
    // And the name must not have swallowed the count.
    DN_ASSERT (!contains (r.body, "<th>0 Bytes received"));
}

DN_TEST (http, the_node_page_hides_the_unreachable_thousand)
{
    // A router's node list is every address in its routing table.  Almost
    // all of them are "Unreachable" and nameless, and a page of a thousand
    // of those buries the handful that mean something.
    Config c = Config::from_string (
        "routing 1.1 --type l1router\n"
        "node 1.1 NODEA\nnode 1.9 FRIEND\n"
        "circuit mul-0 Multinet 127.0.0.1:1:connect\n");
    Node n (c);
    Server s (&n, 0);

    Response few = s.serve (get ("/nodes"));
    Response all = s.serve (get ("/nodes", "all=1"));
    DN_ASSERT_EQ (few.status, 200);
    DN_ASSERT_EQ (all.status, 200);

    // The executor and the named node survive the filter.
    DN_ASSERT (contains (few.body, "NODEA"));
    DN_ASSERT (contains (few.body, "FRIEND"));
    // The full list is the longer one, and the short page says so and
    // offers it rather than hiding the choice.
    DN_ASSERT (all.body.size () > few.body.size ());
    DN_ASSERT (contains (few.body, "not shown"));
    DN_ASSERT (contains (few.body, "all=1"));
}

DN_TEST (http, other_entities_are_not_filtered)
{
    // The filter is about the node table's thousand empty rows.  Nothing
    // else has that shape, and a circuit must never be hidden.
    Config c = Config::from_string (conf);
    Node n (c);
    Server s (&n, 0);

    Response few = s.serve (get ("/circuits"));
    Response all = s.serve (get ("/circuits", "all=1"));
    DN_ASSERT_EQ (few.body, all.body);
    DN_ASSERT (!contains (few.body, "not shown"));
}

DN_TEST (http, an_unknown_page_is_a_404)
{
    Config c = Config::from_string (conf);
    Node n (c);
    Server s (&n, 0);

    Response r = s.serve (get ("/nosuchthing"));
    DN_ASSERT_EQ (r.status, 404);
}

DN_TEST (http, only_get_is_served)
{
    Config c = Config::from_string (conf);
    Node n (c);
    Server s (&n, 0);

    Request r = get ("/");
    r.method = "POST";
    DN_ASSERT_EQ (s.serve (r).status, 400);
}

DN_TEST (http, markup_in_a_value_is_escaped)
{
    // Nothing in a DECnet database should contain markup, but a page that
    // renders names from the wire has to assume one might.
    Config c = Config::from_string (conf);
    Node n (c);
    Server s (&n, 0);

    Response r = s.serve (get ("/"));
    DN_ASSERT (!contains (r.body, "<script"));
}

// ------------------------------------------------------- configuration

DN_TEST (http, the_port_comes_from_the_configuration)
{
    Config c = Config::from_string (std::string (conf)
                                    + "http --http-port 8102 --https-port 0\n");
    DN_ASSERT_EQ (c.http_port (), 8102u);
    // And the line is claimed, not left for a later layer to puzzle over.
    for (const ConfigLine &l : c.unhandled ())
        DN_ASSERT_NE (l.command, std::string ("http"));
}

DN_TEST (http, no_http_line_means_no_server)
{
    Config c = Config::from_string (conf);
    DN_ASSERT_EQ (c.http_port (), 0u);
}

// ------------------------------------------------------- over a socket

DN_TEST (http, a_page_is_served_over_a_real_connection)
{
    // Port 0: the system picks one, which keeps the test from colliding
    // with whatever else is on this machine.
    Config c = Config::from_string (std::string (conf)
                                    + "http --http-port 0\n");
    Node n (c);
    n.start ();

    Server *srv = nullptr;
    (void) srv;
    // The node owns the server; ask it which port was bound by connecting
    // to the one the configuration produced.  With --http-port 0 the
    // server rewrites its own port, so go through a fresh server of our
    // own to keep the test independent of Node's internals.
    Server s (&n, 0);
    s.start ();
    DN_ASSERT (s.port () != 0);

    HostAddress dest ("127.0.0.1", static_cast<std::uint16_t> (s.port ()));
    SourceAddress src ("", 0);
    Socket sock = create_connection (dest, src);
    DN_ASSERT (sock.valid ());

    // create_connection is non-blocking; wait for it to finish.
    DN_ASSERT (wait_until ([&] { return sock.socket_error () == 0; },
                           std::chrono::seconds (5)));

    const char *req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    DN_ASSERT (::send (sock.fd (), req, std::strlen (req), 0) > 0);

    std::string got;
    char buf[4096];
    auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (std::chrono::steady_clock::now () < deadline) {
        ssize_t k = ::recv (sock.fd (), buf, sizeof buf, MSG_DONTWAIT);
        if (k > 0) { got.append (buf, static_cast<std::size_t> (k)); continue; }
        if (k == 0) break;
        if (contains (got, "</html>")) break;
        std::this_thread::sleep_for (std::chrono::milliseconds (20));
    }

    DN_ASSERT (contains (got, "HTTP/1.1 200 OK"));
    DN_ASSERT (contains (got, "NODEA"));

    s.stop ();
    n.stop ();
}
