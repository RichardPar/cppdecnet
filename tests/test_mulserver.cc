// Multinet server mode: one listening port, a circuit per connection, and
// one neighbour per node address.

#include "harness.h"

#include "decnet/common/socket.h"
#include "decnet/config.h"
#include "decnet/datalink/multinet.h"
#include "decnet/node.h"
#include "decnet/routing/l1router.h"
#include "decnet/routing/ptp.h"

#include <chrono>
#include <future>
#include <set>
#include <stdexcept>
#include <thread>

using namespace decnet;
using namespace decnet::routing;
using decnet::datalink::MultinetDevice;

namespace {

std::uint16_t free_port ()
{
    SourceAddress any ("127.0.0.1", 0);
    Socket s = any.create_server ();
    if (!s) throw std::runtime_error ("cannot find a free port");
    sockaddr_storage sa {};
    socklen_t len = sizeof sa;
    ::getsockname (s.fd (), reinterpret_cast<sockaddr *> (&sa), &len);
    return ntohs (reinterpret_cast<sockaddr_in *> (&sa)->sin_port);
}

template <typename P>
bool wait_until (P pred, std::chrono::milliseconds timeout
                     = std::chrono::seconds (15))
{
    auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        if (pred ()) return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (20));
    }
    return pred ();
}

// Run fn on n's node thread and return its result.  The circuit tables
// change on that thread, so that is where the tests look at them.
template <typename F>
auto on_node (Node &n, F fn) -> decltype (fn ())
{
    using R = decltype (fn ());
    auto p = std::make_shared<std::promise<R>> ();
    auto f = p->get_future ();
    n.add_work (std::make_unique<CallbackWork> ([p, fn] { p->set_value (fn ()); }));
    if (f.wait_for (std::chrono::seconds (5)) != std::future_status::ready)
        throw std::runtime_error ("node thread did not answer");
    return f.get ();
}

// What the router has: its point to point circuit names, and for each
// adjacency the node address and the circuit it is on.
struct Snapshot {
    std::set<std::string>                    circuits;
    std::map<unsigned, std::string>          adjacencies;   // tid -> circuit
};

Snapshot snapshot (Node &n)
{
    return on_node (n, [&n] {
        Snapshot s;
        for (const PtpCircuit *c : n.routing ()->circuits ())
            s.circuits.insert (c->name ());
        for (const auto &[id, adj] : n.routing ()->adjacencies ())
            s.adjacencies[adj->nodeid ().tid ()] =
                adj->circuit () ? adj->circuit ()->name () : "";
        return s;
    });
}

// A level 1 router, 1.1, with a Multinet server on a loopback port.
std::unique_ptr<Node> make_router (std::uint16_t port,
                                   const std::string &extra = "")
{
    Config cfg = Config::from_string (
        "routing 1.1 --type l1router\nnode 1.1 RTR\n"
        "circuit mul Multinet 127.0.0.1:" + std::to_string (port)
        + ":server --t3 2" + extra + "\n");
    return std::make_unique<Node> (cfg);
}

// An endnode with address 1.<tid> that dials the router's server.
std::unique_ptr<Node> make_endnode (unsigned tid, std::uint16_t port)
{
    std::string id = "1." + std::to_string (tid);
    Config cfg = Config::from_string (
        "routing " + id + " --type endnode\nnode " + id + " END"
        + std::to_string (tid) + "\nnode 1.1 RTR\n"
        "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
        + ":connect --t3 2\n");
    return std::make_unique<Node> (cfg);
}

bool endnode_up (Node &n) { return n.routing ()->adjacency_count () >= 1; }

}   // namespace

// ---------------------------------------------------------------- config

DN_TEST (mulserver, device_parses_with_and_without_a_bind_host)
{
    MultinetDevice d = MultinetDevice::parse (":7100:server");
    DN_ASSERT (d.mode == MultinetDevice::Mode::server);
    DN_ASSERT_EQ (d.source_port, 7100u);
    DN_ASSERT_EQ (d.dest_port, 0u);
    DN_ASSERT (d.destination.empty ());

    d = MultinetDevice::parse ("10.1.2.3:7100:server");
    DN_ASSERT (d.mode == MultinetDevice::Mode::server);
    DN_ASSERT_EQ (d.destination, std::string ("10.1.2.3"));
    DN_ASSERT_EQ (d.source_port, 7100u);

    // A port is needed: "server" alone is not one.
    DN_ASSERT_THROWS (std::invalid_argument, MultinetDevice::parse (":server"));
    DN_ASSERT (datalink::is_multinet_server (":7100:server"));
    DN_ASSERT (!datalink::is_multinet_server ("h:7100:listen"));
}

DN_TEST (mulserver, max_and_allow_options)
{
    Config cfg = Config::from_string (
        "routing 1.1 --type l1router\nnode 1.1 RTR\n"
        "circuit mul Multinet :7100:server --max 3 "
        "--allow 10.0.0.1,10.0.0.2 --allow host3\n");
    const CircuitConfig &c = cfg.circuits ().front ();
    DN_ASSERT_EQ (c.max_connections, 3u);
    DN_ASSERT_EQ (c.allow.size (), 3u);
    DN_ASSERT_EQ (c.allow[0], std::string ("10.0.0.1"));
    DN_ASSERT_EQ (c.allow[2], std::string ("host3"));

    Config dflt = Config::from_string (
        "routing 1.1 --type l1router\nnode 1.1 RTR\n"
        "circuit mul Multinet :7100:server\n");
    DN_ASSERT_EQ (dflt.circuits ().front ().max_connections, 16u);
    DN_ASSERT (dflt.circuits ().front ().allow.empty ());
}

DN_TEST (mulserver, an_endnode_may_not_have_a_server)
{
    Config cfg = Config::from_string (
        "routing 1.5 --type endnode\nnode 1.5 END\n"
        "circuit mul Multinet :7100:server\n");
    DN_ASSERT_THROWS (std::exception, Node (cfg));
}

DN_TEST (mulserver, the_server_line_is_not_a_circuit)
{
    auto r = make_router (free_port ());
    // Nothing is listed until someone connects.
    DN_ASSERT (r->routing ()->circuits ().empty ());
    DN_ASSERT (r->datalink ()->circuits ().empty ());
}

// ------------------------------------------------------------ connections

DN_TEST (mulserver, each_caller_gets_its_own_circuit)
{
    std::uint16_t port = free_port ();
    auto r = make_router (port);
    auto e1 = make_endnode (120, port);
    auto e2 = make_endnode (121, port);
    r->start (); e1->start (); e2->start ();

    DN_ASSERT (wait_until ([&] {
        return snapshot (*r).adjacencies.size () == 2; }));
    Snapshot s = snapshot (*r);
    DN_ASSERT_EQ (s.circuits, (std::set<std::string> { "MUL-0", "MUL-1" }));
    DN_ASSERT (s.adjacencies.count (120) && s.adjacencies.count (121));
    DN_ASSERT_NE (s.adjacencies[120], s.adjacencies[121]);
    DN_ASSERT (endnode_up (*e1) && endnode_up (*e2));

    // Both are reachable through the router, one hop away.
    auto *l1 = dynamic_cast<L1Router *> (r->routing ());
    DN_ASSERT (wait_until ([&] {
        return on_node (*r, [&] { return l1->reachable (120)
                                      && l1->reachable (121); }); }));

    e2->stop (); e1->stop (); r->stop ();
}

DN_TEST (mulserver, a_second_claim_to_an_address_is_refused)
{
    std::uint16_t port = free_port ();
    auto r = make_router (port);
    auto first = make_endnode (120, port);
    r->start (); first->start ();
    DN_ASSERT (wait_until ([&] {
        return snapshot (*r).adjacencies.count (120) == 1; }));
    const std::string held = snapshot (*r).adjacencies[120];

    // Another node configured with the same address.
    auto dup = make_endnode (120, port);
    dup->start ();

    // It gets as far as a circuit, is turned away at its routing init, and
    // the circuit goes again.  Give it time for a couple of attempts.
    std::this_thread::sleep_for (std::chrono::seconds (4));
    Snapshot s = snapshot (*r);
    DN_ASSERT_EQ (s.adjacencies.size (), 1u);
    DN_ASSERT_EQ (s.adjacencies[120], held);       // the first is untouched
    DN_ASSERT (!endnode_up (*dup));
    DN_ASSERT (endnode_up (*first));
    // No circuit is left behind for it, bar one for a retry in progress.
    DN_ASSERT (s.circuits.size () <= 2);
    DN_ASSERT (s.circuits.count (held) == 1);

    dup->stop ();
    DN_ASSERT (wait_until ([&] { return snapshot (*r).circuits.size () == 1; }));

    // Once the first has gone, the address is free again.
    first->stop ();
    DN_ASSERT (wait_until ([&] { return snapshot (*r).circuits.empty (); }));
    auto again = make_endnode (120, port);
    again->start ();
    DN_ASSERT (wait_until ([&] {
        return snapshot (*r).adjacencies.count (120) == 1; }));
    again->stop ();
    r->stop ();
}

DN_TEST (mulserver, our_own_address_is_refused)
{
    std::uint16_t port = free_port ();
    auto r = make_router (port);
    auto self = make_endnode (1, port);     // claims 1.1, the router's
    r->start (); self->start ();
    std::this_thread::sleep_for (std::chrono::seconds (3));
    DN_ASSERT (snapshot (*r).adjacencies.empty ());
    DN_ASSERT (!endnode_up (*self));
    self->stop (); r->stop ();
}

DN_TEST (mulserver, a_dropped_connection_removes_its_circuit)
{
    std::uint16_t port = free_port ();
    auto r = make_router (port);
    auto e1 = make_endnode (120, port);
    auto e2 = make_endnode (121, port);
    r->start (); e1->start (); e2->start ();
    DN_ASSERT (wait_until ([&] {
        return snapshot (*r).adjacencies.size () == 2; }));
    const std::string gone = snapshot (*r).adjacencies[121];

    e2->stop ();
    DN_ASSERT (wait_until ([&] {
        Snapshot s = snapshot (*r);
        return s.circuits.size () == 1 && !s.adjacencies.count (121); }));
    DN_ASSERT (!snapshot (*r).circuits.count (gone));
    DN_ASSERT (snapshot (*r).adjacencies.count (120) == 1);

    // A new caller gets a circuit; the name may be the one just freed.
    auto e3 = make_endnode (122, port);
    e3->start ();
    DN_ASSERT (wait_until ([&] {
        return snapshot (*r).adjacencies.count (122) == 1; }));
    DN_ASSERT_EQ (snapshot (*r).circuits.size (), 2u);

    // Well past the time the removed circuit is kept for before it is
    // freed: nothing may still be using it.
    std::this_thread::sleep_for (std::chrono::seconds (10));
    DN_ASSERT_EQ (snapshot (*r).adjacencies.size (), 2u);

    e3->stop (); e1->stop (); r->stop ();
}

DN_TEST (mulserver, max_limits_the_connections)
{
    std::uint16_t port = free_port ();
    auto r = make_router (port, " --max 1");
    auto e1 = make_endnode (120, port);
    auto e2 = make_endnode (121, port);
    r->start (); e1->start ();
    DN_ASSERT (wait_until ([&] {
        return snapshot (*r).adjacencies.size () == 1; }));
    e2->start ();
    std::this_thread::sleep_for (std::chrono::seconds (3));
    Snapshot s = snapshot (*r);
    DN_ASSERT_EQ (s.circuits.size (), 1u);
    DN_ASSERT (!s.adjacencies.count (121));
    DN_ASSERT (!endnode_up (*e2));
    e2->stop (); e1->stop (); r->stop ();
}

DN_TEST (mulserver, allow_refuses_other_hosts)
{
    std::uint16_t port = free_port ();
    // The callers are all on 127.0.0.1.
    auto r = make_router (port, " --allow 192.0.2.1");
    auto e1 = make_endnode (120, port);
    r->start (); e1->start ();
    std::this_thread::sleep_for (std::chrono::seconds (3));
    DN_ASSERT (snapshot (*r).circuits.empty ());
    DN_ASSERT (!endnode_up (*e1));
    e1->stop (); r->stop ();
}

DN_TEST (mulserver, allow_admits_a_listed_host)
{
    std::uint16_t port = free_port ();
    auto r = make_router (port, " --allow 192.0.2.1,127.0.0.1");
    auto e1 = make_endnode (120, port);
    r->start (); e1->start ();
    DN_ASSERT (wait_until ([&] {
        return snapshot (*r).adjacencies.count (120) == 1; }));
    e1->stop (); r->stop ();
}

// ------------------------------------------------------ static circuits

DN_TEST (mulserver, static_circuits_also_refuse_a_duplicate)
{
    // The check is in the routing init, so it covers configured circuits
    // too: a second 1.120 on another circuit used to replace the first in
    // the adjacency table.
    std::uint16_t p1 = free_port (), p2 = free_port ();
    Config rc = Config::from_string (
        "routing 1.1 --type l1router\nnode 1.1 RTR\n"
        "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (p1)
        + ":listen --t3 2\n"
        "circuit mul-1 Multinet 127.0.0.1:" + std::to_string (p2)
        + ":listen --t3 2\n");
    Node r (rc);
    auto e1 = make_endnode (120, p1);
    auto e2 = make_endnode (120, p2);
    r.start (); e1->start ();
    DN_ASSERT (wait_until ([&] {
        return snapshot (r).adjacencies.count (120) == 1; }));
    e2->start ();
    std::this_thread::sleep_for (std::chrono::seconds (4));
    Snapshot s = snapshot (r);
    DN_ASSERT_EQ (s.adjacencies.size (), 1u);
    DN_ASSERT_EQ (s.adjacencies[120], std::string ("MUL-0"));
    // A configured circuit sends its init at once, as it always has, so
    // the duplicate may see itself up for a moment before the restart;
    // only server circuits hold theirs back.  What matters is our table.
    // Configured circuits stay, whatever happens on them.
    DN_ASSERT_EQ (s.circuits.size (), 2u);
    e2->stop (); e1->stop (); r.stop ();
}
