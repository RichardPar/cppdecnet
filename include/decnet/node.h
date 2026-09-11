// decnet/node.h -- the Node object, container for everything else.
//
// Port of node.py.  A Node owns the work queue, the timer wheel and the
// per-layer objects, and runs the main loop that gives the whole system its
// single threaded semantics.  As in pydecnet, more than one Node can exist
// in one process, which is how a whole test network fits in one program.

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

// Per node database entry for a remote node.  Port of node.Nodeinfo; the
// NSP and routing state that pydecnet mixes in here will be added as those
// layers are ported.
struct Nodeinfo {
    Nodeid      id;
    std::string name;
    std::string inbound_verification;
    std::string outbound_verification;
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

    // Answer a NICE read request.  Returns zero with the replies filled
    // in, or a NICE error return code.  The request is taken by reference
    // because it is rewritten first: a read of "the executor" arrives as
    // node address zero, and a read by name arrives as a name, and the
    // layers below are spared both.  Port of Node.nice_read.
    int nice_read (nice::NiceRequest &req, nice::ReplyDict &replies);

    // What this node calls itself in a management reply.  Port of
    // Node.ident and Node.swident.
    const std::string &identification () const noexcept { return ident_; }
    const std::string &software_identification () const noexcept
    { return swident_; }

    // Seconds since the counters were last zeroed, which for now is since
    // the node started: nothing zeroes them yet.
    unsigned seconds_since_zeroed () const noexcept;

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

    // Every node the database knows, in address order.  A NICE read of
    // "known nodes" walks this, and so does the monitoring page.  Sorted
    // rather than in hash order so a listing is stable between reads.
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

    WorkQueue      queue_;
    TimerWheel     timers_;
    WorkStats      stats_;
    std::thread    thread_;

    std::unordered_map<std::uint16_t, std::unique_ptr<Nodeinfo>> by_id_;
    std::unordered_map<std::string, Nodeinfo *>                  by_name_;

    // Layers, in node.Node.startlist order.  They are started in this
    // order and stopped in the reverse.
    std::unique_ptr<datalink::DatalinkLayer> datalink_;
    std::unique_ptr<routing::BaseRouter>     routing_;
    std::unique_ptr<nsp::NSP>                nsp_;
    std::unique_ptr<session::Session>        session_;
    std::unique_ptr<mop::Mop>                mop_;
    std::unique_ptr<http::Server>            http_;
    std::unique_ptr<events::EventLogger>     event_logger_;
    // PORT: the bridge follows.
};

}   // namespace decnet

#endif  // DECNET_NODE_H
