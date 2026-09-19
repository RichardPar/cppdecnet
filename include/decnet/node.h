// decnet/node.h -- the Node object.
//
// Port of node.py.  A Node owns the work queue, timer wheel and layer
// objects and runs the main loop.  Several nodes can exist in one process.

#ifndef DECNET_NODE_H
#define DECNET_NODE_H

#include "decnet/common/element.h"
#include "decnet/common/timers.h"
#include "decnet/common/types.h"
#include "decnet/common/work.h"
#include "decnet/nice/entity.h"
#include "decnet/nice/nml.h"

#include <chrono>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>

namespace decnet {

class Config;

namespace datalink { class DatalinkLayer; }
namespace routing  { class BaseRouter; }
namespace nsp      { class NSP; }
namespace session  { class Session; }
namespace mop      { class Mop; }
namespace events   { class Event; class EventLogger; }
namespace http     { class Server; }
class NodeFetcher;

// The NSP counters kept for every node we have talked to.  Port of
// nsp.NspCounters.  "User" counts what session control handed over; "total"
// counts every NSP message on the wire, acknowledgements included.
struct NodeCounters {
    std::uint64_t byt_rcv = 0, byt_xmt = 0;
    std::uint64_t msg_rcv = 0, msg_xmt = 0;
    std::uint64_t t_byt_rcv = 0, t_byt_xmt = 0;
    std::uint64_t t_msg_rcv = 0, t_msg_xmt = 0;
    std::uint64_t con_rcv = 0, con_xmt = 0;
    std::uint64_t timeout = 0, no_res_rcv = 0;

    // When this set was created, which is what "time since counters zeroed"
    // reports: nothing zeroes them, as NOTDONE.md records.
    std::chrono::steady_clock::time_point zeroed
        = std::chrono::steady_clock::now ();

    unsigned seconds_since_zeroed () const noexcept;

    // Has this node been talked to at all?  What makes a node "significant"
    // to a counters read.  Port of NSPNode.used.
    bool used () const noexcept
    { return t_byt_rcv || t_byt_xmt || con_rcv || con_xmt; }
};

// Node database entry for a remote node.  Port of node.Nodeinfo.
struct Nodeinfo {
    Nodeid      id;
    std::string name;
    std::string inbound_verification;
    std::string outbound_verification;

    // Smoothed round trip time to this node in seconds, zero until measured.
    // Shared by all connections to the node.
    double      delay = 0.0;

    // Per node NSP counters, kept for the executor as well as for remote
    // nodes.  Port of the counters NSPNode carries.
    NodeCounters counters;
};

// The counters the executor keeps that no other node has.  Port of
// routing.ExecCounters, less the four a router keeps in its routing table
// (aged, unreachable and out of range loss, and partial update loss), which
// live on the router itself and are reported only when there is one.
struct ExecCounters {
    std::uint64_t peak_conns = 0;
    std::uint64_t oversized_loss = 0;
    std::uint64_t fmt_errors = 0;
    std::uint64_t ver_rejects = 0;
};

// Timing histogram for work item dispatch, as node.WorkStats does.
class WorkStats {
public:
    void add (const char *kind, double seconds);
    std::map<std::string, std::pair<unsigned long, double>> rows () const;

private:
    struct Row { unsigned long count = 0; double total = 0; double max = 0; };
    std::map<std::string, Row> rows_;
};

class Node : public Element {
public:
    explicit Node (const Config &config);
    ~Node () override;

    Node (const Node &) = delete;
    Node &operator= (const Node &) = delete;

    // Identity
    const std::string &name () const noexcept { return name_; }
    Nodeid id () const noexcept { return id_; }
    Phase phase () const noexcept { return phase_; }

    // Subsystems
    TimerWheel &timers () noexcept { return timers_; }
    const Config &config () const noexcept { return config_; }
    datalink::DatalinkLayer *datalink () const noexcept
    { return datalink_.get (); }
    routing::BaseRouter *routing () const noexcept { return routing_.get (); }
    nsp::NSP *nsp () const noexcept { return nsp_.get (); }
    session::Session *session () const noexcept { return session_.get (); }
    mop::Mop *mop () const noexcept { return mop_.get (); }
    events::EventLogger *event_logger () const noexcept
    { return event_logger_.get (); }

    // Raise an event.  The source node is filled in here, so a caller only
    // has to say what happened and to what.  Port of Node.logevent.
    void logevent (events::Event &e);

    // This node as a NICE node: address plus name, which is what an event
    // record and a network management reply both carry.
    nice::NiceNode nicenode () const;

    // Any node as a NICE node: the address, plus the name if the node
    // database has one for it.
    nice::NiceNode nicenode (Nodeid id) const;

    // Answer a NICE read.  Returns zero with replies filled in, or a NICE
    // error code.  The request is rewritten first: executor (address zero)
    // and node names are resolved to addresses.  Port of Node.nice_read.
    int nice_read (nice::NiceRequest &req, nice::ReplyDict &replies);

    // What this node calls itself in a management reply.  Port of
    // Node.ident and Node.swident.
    const std::string &identification () const noexcept { return ident_; }
    const std::string &software_identification () const noexcept
    { return swident_; }

    // Seconds since the counters were last zeroed, which for now is since
    // the node started: nothing zeroes them yet.
    unsigned seconds_since_zeroed () const noexcept;

    // The executor's own counters, which NSP and routing both add to.
    ExecCounters &exec_counters () noexcept { return exec_counters_; }
    const ExecCounters &exec_counters () const noexcept
    { return exec_counters_; }

    // Queue work to this node from any thread.  Port of Node.addwork.
    void add_work (WorkPtr w);
    void add_work (WorkPtr w, Element *handler);

    // Start every layer in order, then the main loop on its own thread.
    void start ();

    // Post a Shutdown item and wait for the main loop to finish.
    void stop ();

    // Run the main loop on the calling thread.  start() uses a thread; a
    // single node program can call this directly instead.
    void mainloop ();

    // Node database.  Port of Node.nodeinfo and friends.
    Nodeinfo *find_node (Nodeid id, bool add = true);
    Nodeinfo *find_node (const std::string &name);
    void add_node (Nodeinfo info);

    // Set a node's name, creating the entry if it is new and keeping
    // everything else about an existing one -- its counters and its round
    // trip estimate in particular, which add_node would discard.
    //
    // A name the configuration gave is never overwritten: a downloaded list
    // does not get to rename what the operator named.  Returns true if
    // anything changed.  Node thread only.
    bool set_node_name (Nodeid id, const std::string &name);

    // How many names came from a fetched list at the last refresh, for the
    // log line and for the tests.
    std::size_t fetched_names () const noexcept { return fetched_names_; }

    // All known nodes, in address order.
    std::vector<const Nodeinfo *> known_nodes () const;

    void dispatch (Work &w) override;

    const WorkStats &stats () const noexcept { return stats_; }

private:
    // Stop every layer, in order.  Runs on the node's thread; see stop().
    void stop_layers ();

    const Config  &config_;
    std::string    name_;
    Nodeid         id_;
    Phase          phase_ = Phase::ph4;

    std::string    ident_, swident_;
    std::chrono::steady_clock::time_point zeroed_;
    ExecCounters   exec_counters_;

    WorkQueue      queue_;
    TimerWheel     timers_;
    WorkStats      stats_;
    std::thread    thread_;

    std::unordered_map<std::uint16_t, std::unique_ptr<Nodeinfo>> by_id_;
    std::unordered_map<std::string, Nodeinfo *>                  by_name_;
    // Addresses the configuration named itself, which a refresh leaves be.
    std::set<std::uint16_t>                                      config_named_;
    std::size_t                                                  fetched_names_ = 0;

    // Layers, in node.Node.startlist order.  They are started in this
    // order and stopped in the reverse.
    std::unique_ptr<datalink::DatalinkLayer> datalink_;
    std::unique_ptr<routing::BaseRouter>     routing_;
    std::unique_ptr<nsp::NSP>                nsp_;
    std::unique_ptr<session::Session>        session_;
    std::unique_ptr<mop::Mop>                mop_;
    std::unique_ptr<http::Server>            http_;
    std::unique_ptr<NodeFetcher>             node_fetcher_;
    std::unique_ptr<events::EventLogger>     event_logger_;
    // PORT: the bridge follows.
};

}   // namespace decnet

#endif  // DECNET_NODE_H
