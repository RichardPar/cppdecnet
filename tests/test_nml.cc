// The network management listener, object 19, driven over a real logical
// link between two nodes.
//
// These are the tests that say the NICE path works end to end: a request
// goes out over NSP as NCP would send it, the listener decodes it, the
// layers answer, and the replies come back in the multiple-item framing
// the protocol requires.  The unit tests in test_nice.cc check the value
// coding underneath; these check that something real is on the other end.

#include "harness.h"

#include "decnet/common/socket.h"
#include "decnet/config.h"
#include "decnet/nice/nml.h"
#include "decnet/nice/packets.h"
#include "decnet/node.h"
#include "decnet/routing/routing.h"
#include "decnet/session/session.h"

#include <chrono>
#include <mutex>
#include <thread>

using namespace decnet;
using namespace decnet::nice;
using namespace decnet::session;

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
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    return pred ();
}

// Stands in for NCP: connects to object 19 and collects what comes back.
class Ncp : public Application {
public:
    void connect_received (SessionConnection &, ByteView data) override
    {
        std::lock_guard l (m_);
        ++accepts_;
        accept_data_.assign (data.begin (), data.end ());
    }
    void data_received (SessionConnection &, ByteView data) override
    {
        std::lock_guard l (m_);
        replies_.push_back (Bytes (data.begin (), data.end ()));
    }
    void disconnected (SessionConnection &, unsigned reason) override
    {
        std::lock_guard l (m_);
        ++disconnects_;
        reason_ = reason;
    }

    int accepts () { std::lock_guard l (m_); return accepts_; }
    Bytes accept_data () { std::lock_guard l (m_); return accept_data_; }
    std::size_t count () { std::lock_guard l (m_); return replies_.size (); }
    Bytes at (std::size_t i) { std::lock_guard l (m_); return replies_.at (i); }
    int disconnects () { std::lock_guard l (m_); return disconnects_; }
    unsigned reason () { std::lock_guard l (m_); return reason_; }

private:
    std::mutex         m_;
    int                accepts_ = 0, disconnects_ = 0;
    unsigned           reason_ = 0;
    Bytes              accept_data_;
    std::vector<Bytes> replies_;
};

// Two nodes joined by a Multinet circuit.  Node A is the one being asked
// about; node B runs the client.
struct Pair {
    std::uint16_t port = free_port ();
    Config acfg, bcfg;
    std::unique_ptr<Node> a, b;

    Pair ()
        : acfg (Config::from_string (
              "routing 1.1 --type endnode\nnode 1.1 NODEA\nnode 1.2 NODEB\n"
              "system --ident \"test executor\"\n"
              "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
              + ":listen --t3 2\n")),
          bcfg (Config::from_string (
              "routing 1.2 --type endnode\nnode 1.2 NODEB\nnode 1.1 NODEA\n"
              "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
              + ":connect --t3 2\n"))
    {
        a = std::make_unique<Node> (acfg);
        b = std::make_unique<Node> (bcfg);
    }

    void start ()
    {
        a->start ();
        b->start ();
        wait_until ([&] {
            return a->routing ()->adjacency_count () == 1
                && b->routing ()->adjacency_count () == 1;
        });
    }
    void stop () { if (b) b->stop (); if (a) a->stop (); }

    // Open the NICE connection from B to A's object 19, as NCP does,
    // sending the protocol version as the connect data.
    SessionConnection *open (Ncp *&out)
    {
        auto client = std::make_unique<Ncp> ();
        out = client.get ();
        ConnectData cd;
        cd.dstname = EndUser::number (19);
        cd.srcname = EndUser::named ("NCP");
        cd.connectdata = Bytes { 4, 0, 0 };
        return b->session ()->connect (Nodeid::parse ("1.1"), std::move (cd),
                                       std::move (client));
    }
};

// A read request for one entity kind, at one information level.
NiceRequest read_request (std::uint8_t etype, unsigned info,
                          ReqEntity ent)
{
    NiceRequest r;
    r.function = fn_read;
    r.info = info;
    r.entity_type = etype;
    r.entity = ent;
    return r;
}

}   // namespace

// ------------------------------------------------------------ the object

DN_TEST (nml, object_19_is_registered_by_default)
{
    Config c = Config::from_string (
        "routing 1.1 --type endnode\nnode 1.1 NODEA\n"
        "circuit mul-0 Multinet 127.0.0.1:1:connect\n");
    Node n (c);
    const Object *o = n.session ()->find_object (19);
    DN_ASSERT (o != nullptr);
    DN_ASSERT_EQ (o->name, std::string ("NML"));
}

DN_TEST (nml, connection_is_accepted_with_our_version)
{
    Pair p;
    p.start ();

    Ncp *ncp = nullptr;
    SessionConnection *c = p.open (ncp);
    DN_ASSERT (c != nullptr);

    // The accept data is the NICE version we speak, which is 4.0.0.
    DN_ASSERT (wait_until ([&] { return ncp->accepts () == 1; }));
    DN_ASSERT_EQ (ncp->accept_data (), (Bytes { 4, 0, 0 }));

    p.stop ();
}

// ---------------------------------------------------------- read requests

DN_TEST (nml, read_executor_characteristics)
{
    Pair p;
    p.start ();

    Ncp *ncp = nullptr;
    SessionConnection *c = p.open (ncp);
    DN_ASSERT (wait_until ([&] { return ncp->accepts () == 1; }));

    // "Read executor characteristics": node entity, address zero, which is
    // how the executor is named on the wire.
    NiceRequest req = read_request (Entity::node, info_char,
                                    ReqEntity::make_node (Nodeid ()));
    c->send_data (req.encode ());

    // One entity, one reply: answered directly rather than wrapped in a
    // multiple-item exchange.
    DN_ASSERT (wait_until ([&] { return ncp->count () == 1; }));
    NiceReply r = NiceReply::parse (ncp->at (0), Entity::node);
    DN_ASSERT_EQ (r.retcode, rc_success);
    DN_ASSERT (r.has_entity);
    DN_ASSERT_EQ (r.entity.as_node ().id.str (), std::string ("1.1"));

    // The identification the configuration gave us, and the management
    // version, both of which only the executor reports.
    const Param *ident = r.params.find (100);
    DN_ASSERT (ident != nullptr);
    DN_ASSERT_EQ (ident->value.format (), std::string ("test executor"));
    DN_ASSERT (r.params.find (101) != nullptr);
    // The routing layer's contribution: the routing version and the block
    // size it will accept.
    DN_ASSERT (r.params.find (900) != nullptr);
    DN_ASSERT (r.params.find (932) != nullptr);
    // And NSP's: the maximum number of logical links.
    DN_ASSERT (r.params.find (710) != nullptr);

    p.stop ();
}

DN_TEST (nml, read_executor_status_names_the_node)
{
    Pair p;
    p.start ();

    Ncp *ncp = nullptr;
    SessionConnection *c = p.open (ncp);
    DN_ASSERT (wait_until ([&] { return ncp->accepts () == 1; }));

    NiceRequest req = read_request (Entity::node, info_status,
                                    ReqEntity::make_node (Nodeid ()));
    c->send_data (req.encode ());
    DN_ASSERT (wait_until ([&] { return ncp->count () == 1; }));

    NiceReply r = NiceReply::parse (ncp->at (0), Entity::node);
    DN_ASSERT_EQ (r.retcode, rc_success);
    // The executor entity carries its name as well as its address, and is
    // flagged as the executor.
    DN_ASSERT_EQ (r.entity.as_node ().name, std::string ("NODEA"));
    DN_ASSERT (r.entity.as_node ().executor);
    // State "on", and the executor's physical address.
    DN_ASSERT (r.params.find (0) != nullptr);
    DN_ASSERT (r.params.find (10) != nullptr);

    p.stop ();
}

DN_TEST (nml, read_known_circuits_uses_multiple_item_framing)
{
    Pair p;
    p.start ();

    Ncp *ncp = nullptr;
    SessionConnection *c = p.open (ncp);
    DN_ASSERT (wait_until ([&] { return ncp->accepts () == 1; }));

    // "Show known circuits status".  Node A has one circuit, MUL-0, and
    // it is up, so the reply should name the neighbour.
    NiceRequest req = read_request (
        Entity::circuit, info_status,
        ReqEntity::make_wild (Entity::circuit, ReqEntity::known));
    c->send_data (req.encode ());

    // One circuit is still one entity with one reply, so this is the
    // direct form rather than the wrapped one.
    DN_ASSERT (wait_until ([&] { return ncp->count () == 1; }));
    NiceReply r = NiceReply::parse (ncp->at (0), Entity::circuit);
    DN_ASSERT_EQ (r.retcode, rc_success);
    DN_ASSERT_EQ (r.entity.as_string (), std::string ("MUL-0"));
    // State on, no substate (the circuit is running), and the adjacent
    // node named.
    const Param *state = r.params.find (0);
    DN_ASSERT (state != nullptr);
    DN_ASSERT (r.params.find (1) == nullptr);
    DN_ASSERT (r.params.find (800) != nullptr);

    p.stop ();
}

DN_TEST (nml, read_known_lines)
{
    Pair p;
    p.start ();

    Ncp *ncp = nullptr;
    SessionConnection *c = p.open (ncp);
    DN_ASSERT (wait_until ([&] { return ncp->accepts () == 1; }));

    NiceRequest req = read_request (
        Entity::line, info_char,
        ReqEntity::make_wild (Entity::line, ReqEntity::known));
    c->send_data (req.encode ());
    DN_ASSERT (wait_until ([&] { return ncp->count () == 1; }));

    NiceReply r = NiceReply::parse (ncp->at (0), Entity::line);
    DN_ASSERT_EQ (r.retcode, rc_success);
    DN_ASSERT_EQ (r.entity.as_string (), std::string ("MUL-0"));
    // Duplex and protocol are what a line read reports.
    DN_ASSERT (r.params.find (1111) != nullptr);
    DN_ASSERT (r.params.find (1112) != nullptr);

    p.stop ();
}

DN_TEST (nml, read_known_nodes_wraps_several_entities)
{
    Pair p;
    p.start ();

    Ncp *ncp = nullptr;
    SessionConnection *c = p.open (ncp);
    DN_ASSERT (wait_until ([&] { return ncp->accepts () == 1; }));

    // Node A's database has both nodes in it, so this is the case that
    // exercises the multiple-item framing.
    NiceRequest req = read_request (
        Entity::node, info_summary,
        ReqEntity::make_wild (Entity::node, ReqEntity::known));
    c->send_data (req.encode ());

    // A "multiple items" header, the entries, and an end marker.
    DN_ASSERT (wait_until ([&] { return ncp->count () >= 3; }));
    NiceReply first = NiceReply::parse_header (ncp->at (0));
    DN_ASSERT_EQ (first.retcode, rc_multiple);

    NiceReply last = NiceReply::parse_header (ncp->at (ncp->count () - 1));
    DN_ASSERT_EQ (last.retcode, rc_done);

    // The executor comes first among the entries, whatever its address
    // sorts as.
    NiceReply exec = NiceReply::parse (ncp->at (1), Entity::node);
    DN_ASSERT_EQ (exec.retcode, rc_success);
    DN_ASSERT (exec.entity.as_node ().executor);
    DN_ASSERT_EQ (exec.entity.as_node ().id.str (), std::string ("1.1"));

    p.stop ();
}

DN_TEST (nml, read_a_node_we_know_nothing_about)
{
    Pair p;
    p.start ();

    Ncp *ncp = nullptr;
    SessionConnection *c = p.open (ncp);
    DN_ASSERT (wait_until ([&] { return ncp->accepts () == 1; }));

    // An address the database has never heard of.  Asking about one by
    // address creates the entry, so the answer is a reply about that node
    // carrying nothing -- which is what NCP prints as a bare "Node 9.9".
    // Only an unknown *name* is an unrecognized component, because a name
    // that is not in the database names nothing at all.
    NiceRequest req = read_request (Entity::node, info_status,
                                    ReqEntity::make_node (Nodeid::parse ("9.9")));
    c->send_data (req.encode ());
    DN_ASSERT (wait_until ([&] { return ncp->count () == 1; }));

    NiceReply r = NiceReply::parse (ncp->at (0), Entity::node);
    DN_ASSERT_EQ (r.retcode, rc_success);
    DN_ASSERT_EQ (r.entity.as_node ().id.str (), std::string ("9.9"));
    DN_ASSERT (r.params.empty ());

    p.stop ();
}

DN_TEST (nml, read_an_unknown_node_name_is_unrecognized_component)
{
    Pair p;
    p.start ();

    Ncp *ncp = nullptr;
    SessionConnection *c = p.open (ncp);
    DN_ASSERT (wait_until ([&] { return ncp->accepts () == 1; }));

    NiceRequest req = read_request (
        Entity::node, info_status,
        ReqEntity::make_named (Entity::node, "NOSUCH"));
    c->send_data (req.encode ());
    DN_ASSERT (wait_until ([&] { return ncp->count () == 1; }));

    NiceReply r = NiceReply::parse_header (ncp->at (0));
    DN_ASSERT_EQ (r.retcode, rc_unrecognized_component);

    p.stop ();
}

// -------------------------------------------------------- what we refuse

DN_TEST (nml, set_is_unrecognized_and_zero_is_a_privilege_violation)
{
    Pair p;
    p.start ();

    Ncp *ncp = nullptr;
    SessionConnection *c = p.open (ncp);
    DN_ASSERT (wait_until ([&] { return ncp->accepts () == 1; }));

    // The two writes are refused differently, and the difference is not
    // cosmetic: NCP prints the reason.
    //
    // SET is not implemented at all, here or in pydecnet, whose nml falls
    // through to "Unsupported NICE request" and answers -1.  Saying
    // "privilege violation" would suggest some credential would make it
    // work, and none would.
    NiceRequest set;
    set.function = fn_set;
    set.entity_type = Entity::node;
    set.entity = ReqEntity::make_node (Nodeid::parse ("1.1"));
    c->send_data (set.encode ());
    DN_ASSERT (wait_until ([&] { return ncp->count () == 1; }));
    DN_ASSERT_EQ (NiceReply::parse_header (ncp->at (0)).retcode,
                  rc_unrecognized_function);

    // ZERO is a different case: pydecnet implements it and refuses it with
    // -3 when its read-only flag is set, which is our permanent posture
    // until the access control question is settled.

    NiceRequest zero;
    zero.function = fn_zero;
    zero.entity_type = Entity::circuit;
    zero.entity = ReqEntity::make_wild (Entity::circuit, ReqEntity::known);
    c->send_data (zero.encode ());
    DN_ASSERT (wait_until ([&] { return ncp->count () == 2; }));
    DN_ASSERT_EQ (NiceReply::parse_header (ncp->at (1)).retcode,
                  rc_privilege_violation);

    p.stop ();
}

DN_TEST (nml, an_unparseable_request_is_refused_not_fatal)
{
    Pair p;
    p.start ();

    Ncp *ncp = nullptr;
    SessionConnection *c = p.open (ncp);
    DN_ASSERT (wait_until ([&] { return ncp->accepts () == 1; }));

    // Garbage, and then a request that should still be answered: the
    // listener must not fall over on the first one.
    c->send_data (Bytes { 0xfe, 0xff, 0xff });
    DN_ASSERT (wait_until ([&] { return ncp->count () == 1; }));
    DN_ASSERT_EQ (NiceReply::parse_header (ncp->at (0)).retcode,
                  rc_unrecognized_function);

    NiceRequest req = read_request (Entity::node, info_status,
                                    ReqEntity::make_node (Nodeid ()));
    c->send_data (req.encode ());
    DN_ASSERT (wait_until ([&] { return ncp->count () == 2; }));
    DN_ASSERT_EQ (NiceReply::parse_header (ncp->at (1)).retcode, rc_success);

    p.stop ();
}

// --------------------------------------------------------------- loop node

DN_TEST (nml, loop_node_runs_through_the_mirror)
{
    Pair p;
    p.start ();

    Ncp *ncp = nullptr;
    SessionConnection *c = p.open (ncp);
    DN_ASSERT (wait_until ([&] { return ncp->accepts () == 1; }));

    // NCP LOOP NODE: node A is asked to loop messages off node B's
    // MIRROR, so the traffic crosses the circuit twice.
    NiceRequest req;
    req.function = fn_test;
    req.test_type = test_node;
    req.entity_type = Entity::node;
    req.entity = ReqEntity::make_node (Nodeid::parse ("1.2"));
    req.loop_count = 2;
    req.loop_length = 40;
    req.loop_with = 2;      // mixed
    c->send_data (req.encode ());

    // One success reply when the whole count has been looped.
    DN_ASSERT (wait_until ([&] { return ncp->count () == 1; }));
    NiceReply r = NiceReply::parse_header (ncp->at (0));
    DN_ASSERT_EQ (r.retcode, rc_success);

    p.stop ();
}

DN_TEST (nml, loop_node_rejects_a_bad_argument)
{
    Pair p;
    p.start ();

    Ncp *ncp = nullptr;
    SessionConnection *c = p.open (ncp);
    DN_ASSERT (wait_until ([&] { return ncp->accepts () == 1; }));

    // Loop with a fill pattern that is not one of the three defined ones.
    NiceRequest req;
    req.function = fn_test;
    req.test_type = test_node;
    req.entity_type = Entity::node;
    req.entity = ReqEntity::make_node (Nodeid::parse ("1.2"));
    req.loop_count = 1;
    req.loop_length = 16;
    req.loop_with = 7;
    c->send_data (req.encode ());

    DN_ASSERT (wait_until ([&] { return ncp->count () == 1; }));
    NiceReply r = NiceReply::parse_header (ncp->at (0));
    DN_ASSERT_EQ (r.retcode, rc_invalid_parameter);
    // The detail says which parameter was wrong: 152 is "loop with".
    DN_ASSERT_EQ (r.detail, 152u);

    p.stop ();
}

DN_TEST (nml, loop_node_by_name)
{
    Pair p;
    p.start ();

    Ncp *ncp = nullptr;
    SessionConnection *c = p.open (ncp);
    DN_ASSERT (wait_until ([&] { return ncp->accepts () == 1; }));

    // "LOOP NODE NODEB".  A loop request does not go through the read
    // path, so nothing has turned the name into an address before the
    // listener sees it -- this is what checks that it does so itself.
    NiceRequest req;
    req.function = fn_test;
    req.test_type = test_node;
    req.entity_type = Entity::node;
    req.entity = ReqEntity::make_named (Entity::node, "NODEB");
    req.loop_count = 1;
    req.loop_length = 32;
    c->send_data (req.encode ());

    DN_ASSERT (wait_until ([&] { return ncp->count () == 1; }));
    DN_ASSERT_EQ (NiceReply::parse_header (ncp->at (0)).retcode, rc_success);

    p.stop ();
}

DN_TEST (nml, loop_node_to_an_unknown_name_fails_at_once)
{
    Pair p;
    p.start ();

    Ncp *ncp = nullptr;
    SessionConnection *c = p.open (ncp);
    DN_ASSERT (wait_until ([&] { return ncp->accepts () == 1; }));

    // A name nothing in the database matches.  This has to be answered
    // immediately rather than by trying to connect: an address that is
    // merely unreachable takes the NSP connect timeout to fail, but a
    // name that resolves to nothing can be refused straight away.
    NiceRequest req;
    req.function = fn_test;
    req.test_type = test_node;
    req.entity_type = Entity::node;
    req.entity = ReqEntity::make_named (Entity::node, "NOSUCH");
    req.loop_count = 3;
    req.loop_length = 16;
    c->send_data (req.encode ());

    DN_ASSERT (wait_until ([&] { return ncp->count () == 1; },
                           std::chrono::seconds (5)));
    NiceReply r = NiceReply::parse_loop (ncp->at (0));
    DN_ASSERT_EQ (r.retcode, rc_mirror_connect_failed);
    DN_ASSERT_EQ (r.detail, 2u);        // unknown node name
    // The reply says how many of the requested messages were not looped,
    // which is all of them.
    DN_ASSERT (r.has_notlooped);
    DN_ASSERT_EQ (r.notlooped, 3u);

    p.stop ();
}
