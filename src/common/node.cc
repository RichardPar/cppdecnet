#include "decnet/http/server.h"
#include "decnet/nodefetch.h"
#include "decnet/node.h"

#include "decnet/common/logging.h"
#include "decnet/config.h"
#include "decnet/datalink/datalink.h"
#include "decnet/events/events.h"
#include "decnet/events/logger.h"
#include "decnet/mop/mop.h"
#include "decnet/nice/packets.h"
#include "decnet/nsp/nsp.h"
#include "decnet/routing/routing.h"
#include "decnet/session/session.h"

#include "decnet/version.h"

#include <algorithm>
#include <chrono>

namespace decnet {

// ------------------------------------------------------------- WorkStats

void WorkStats::add (const char *kind, double seconds)
{
    Row &r = rows_[kind];
    ++r.count;
    r.total += seconds;
    r.max = std::max (r.max, seconds);
}

std::map<std::string, std::pair<unsigned long, double>> WorkStats::rows () const
{
    std::map<std::string, std::pair<unsigned long, double>> out;
    for (const auto &[k, v] : rows_) out[k] = { v.count, v.total };
    return out;
}

// ------------------------------------------------------------------ Node

Node::Node (const Config &config)
    : Element (nullptr),
      config_ (config),
      timers_ (this, JIFFY, 3600)
{
    set_node (this);

    if (const auto &r = config.routing ()) {
        phase_ = phase_of (r->type);
        // Below Phase IV a node id is an eight bit value with no area, so
        // normalise it here the way node.py does.
        id_ = (phase_ == Phase::ph4) ? r->id : Nodeid (0u, r->id.tid ());
    }

    for (const auto &n : config.nodes ()) {
        Nodeinfo info;
        info.id                    = n.id;
        info.name                  = n.name;
        info.inbound_verification  = n.inbound_verification;
        info.outbound_verification = n.outbound_verification;
        add_node (std::move (info));
        // Remember which names the operator wrote, so a refresh of a
        // fetched list cannot rename them.
        if (!n.from_source && !n.name.empty ())
            config_named_.insert (n.id.value ());
    }

    if (Nodeinfo *self = find_node (id_, false); self && !self->name.empty ())
        name_ = self->name;
    else if (!config.node_name ().empty ())
        name_ = config.node_name ();
    else
        name_ = id_.str ();

    ident_ = config.identification ();
    swident_ = version::ident ();
    if (ident_.empty ()) ident_ = swident_;
    zeroed_ = std::chrono::steady_clock::now ();

    logging::set_thread_name (name_);
    DN_DEBUG ("initializing node {}", name_);

    // Layer objects, in node.Node.__init__ order.  The event logger is first.
    // PORT: bridge-only configurations.
    event_logger_ = std::make_unique<events::EventLogger> (this, config);
    datalink_ = std::make_unique<datalink::DatalinkLayer> (this, config);
    // MOP runs on broadcast circuits that ask for it, whether or not this
    // node routes.
    mop_ = std::make_unique<mop::Mop> (this, config);

    // Monitoring server, if configured.
    if (config.http_port ())
        http_ = std::make_unique<http::Server> (this, config.http_port ());

    // Node name lists to keep up to date.  The configuration has already
    // loaded each one's cache; this refreshes them in the background.
    if (!config.node_sources ().empty ())
        node_fetcher_ = std::make_unique<NodeFetcher> (this,
                                                       config.node_sources ());
    if (config.routing ()) {
        try {
            routing_ = routing::make_router (this, config);
        } catch (const std::exception &e) {
            // A bad routing configuration is fatal: unlike a single bad
            // circuit, there is no useful node without it.
            DN_CRIT ("cannot initialize routing layer: {}", e.what ());
            throw;
        }
        if (routing_) {
            nsp_ = std::make_unique<nsp::NSP> (this, config);
            routing_->set_nsp (nsp_.get ());
            session_ = std::make_unique<session::Session> (this, config);
            session::add_default_objects (*session_);
            nsp_->set_session_control (session_.get ());
        }
    }
}

Node::~Node ()
{
    stop ();
}

void Node::add_work (WorkPtr w)
{
    queue_.put (std::move (w));
}

void Node::add_work (WorkPtr w, Element *handler)
{
    w->set_owner (handler);
    queue_.put (std::move (w));
}

void Node::add_node (Nodeinfo info)
{
    std::string name = info.name;
    auto owned = std::make_unique<Nodeinfo> (std::move (info));
    Nodeinfo *p = owned.get ();
    by_id_[p->id.value ()] = std::move (owned);
    if (!name.empty ()) by_name_[name] = p;
}

bool Node::set_node_name (Nodeid id, const std::string &name)
{
    if (name.empty ()) return false;
    // What the operator wrote wins over anything downloaded.
    if (config_named_.count (id.value ())) return false;

    Nodeinfo *info = find_node (id);     // creates a nameless entry if new
    if (!info) return false;
    if (info->name == name) return false;
    // Keep the entry itself: it carries this node's counters and the round
    // trip estimate NSP has built up, neither of which a rename should lose.
    if (!info->name.empty ()) by_name_.erase (info->name);
    info->name = name;
    by_name_[name] = info;
    return true;
}

Nodeinfo *Node::find_node (Nodeid id, bool add)
{
    auto it = by_id_.find (id.value ());
    if (it != by_id_.end ()) return it->second.get ();
    if (!add) return nullptr;
    // No entry: add a nameless one, which is what NSP's node database
    // needs.  Port of Node.nodeinfo's add behaviour.
    Nodeinfo info;
    info.id = id;
    add_node (std::move (info));
    return by_id_[id.value ()].get ();
}

Nodeinfo *Node::find_node (const std::string &name)
{
    auto it = by_name_.find (name);
    return it == by_name_.end () ? nullptr : it->second;
}

std::vector<const Nodeinfo *> Node::known_nodes () const
{
    std::vector<const Nodeinfo *> out;
    out.reserve (by_id_.size ());
    for (const auto &[id, info] : by_id_) out.push_back (info.get ());
    std::sort (out.begin (), out.end (),
               [] (const Nodeinfo *a, const Nodeinfo *b)
               { return a->id.value () < b->id.value (); });
    return out;
}

void Node::dispatch (Work &)
{
    // Work addressed to the node itself; nothing uses it yet.
}

nice::NiceNode Node::nicenode () const
{
    return nice::NiceNode (id_, name_);
}

nice::NiceNode Node::nicenode (Nodeid id) const
{
    auto it = by_id_.find (id.value ());
    if (it != by_id_.end ()) return nice::NiceNode (id, it->second->name);
    return nice::NiceNode (id);
}

unsigned Node::seconds_since_zeroed () const noexcept
{
    std::chrono::duration<double> dt =
        std::chrono::steady_clock::now () - zeroed_;
    return static_cast<unsigned> (dt.count ());
}

unsigned NodeCounters::seconds_since_zeroed () const noexcept
{
    std::chrono::duration<double> dt =
        std::chrono::steady_clock::now () - zeroed;
    return static_cast<unsigned> (dt.count ());
}

int Node::nice_read (nice::NiceRequest &req, nice::ReplyDict &replies)
{
    using namespace nice;

    // Resolve executor and node name forms.  Port of the start of
    // Node.nice_read.
    if (req.entity_type == Entity::node && req.entity.code == 0
        && req.entity.id.value () == 0) {
        // "The executor" arrives as node address zero.
        req.entity.id = id_;
    }
    if (req.entity_type == Entity::node && req.entity.code > 0) {
        // A read by name.  Look the name up and substitute the address.
        auto it = by_name_.find (req.entity.name);
        if (it == by_name_.end ()) return rc_unrecognized_component;
        req.entity.code = 0;
        req.entity.id = it->second->id;
    }

    if (req.entity_type == Entity::logging) {
        // Logging is the event logger's business alone.
        if (event_logger_) event_logger_->nice_read (req, replies);
        return 0;
    }
    if (req.events ()) return rc_unrecognized_function;

    // NSP first, since it has the node database; routing then adds
    // reachability.
    if (nsp_)      nsp_->nice_read (req, replies);
    if (routing_)  routing_->nice_read (req, replies);
    if (datalink_) datalink_->nice_read (req, replies);
    if (mop_)      mop_->nice_read (req, replies);

    // The executor's own identity, which no single layer owns.
    if (req.entity_type == Entity::node && replies.contains_node (id_)) {
        NiceReply &exe = replies.node_entry (id_);
        exe.entity = Entity::make_node (nice::NiceNode (id_, name_, true));
        if (req.sum () || req.chars ())
            exe.params.set (100, Value::ai (ident_));
        if (req.chars ()) {
            exe.params.set (126, Value::ai (swident_));
            // Network management version 4.0.0.
            exe.params.set (101, Value::cm ({ Value::du (4), Value::du (0),
                                              Value::du (0) }));
        } else if (req.stat ()) {
            Macaddr m = Macaddr::from_nodeid (id_);
            exe.params.set (10, Value::hi (Bytes (m.bytes ().begin (),
                                                  m.bytes ().end ())));
        }
        if (req.sumstat ())
            exe.params.set (0, Value::c (0));   // On
        if (req.counters ()) {
            // The counters only the executor has.  The per node set has
            // already been added by NSP; these come from this node's own
            // accounting and, for the four a routing table keeps, from the
            // router -- an endnode has none, exactly as PyDECnet excludes
            // them through ExecCounters.rtr_only_nc.
            const ExecCounters &e = exec_counters_;
            exe.params.set_counter (700, Counter { e.peak_conns, 2, false, 0 });
            exe.params.set_counter (903, Counter { e.oversized_loss, 1, false,
                                                   0 });
            exe.params.set_counter (910, Counter { e.fmt_errors, 1, false, 0 });
            exe.params.set_counter (930, Counter { e.ver_rejects, 1, false, 0 });
            if (routing_) routing_->nice_counters (exe);
        }
    }
    return 0;
}

void Node::logevent (events::Event &e)
{
    // Local events only; received records go through logremoteevent.
    e.source = nicenode ();
    if (event_logger_) event_logger_->logevent (e);
}

void Node::start ()
{
    DN_DEBUG ("starting node {}", name_);
    timers_.startup ();
    // Start the layers in startlist order.
    if (event_logger_) event_logger_->start ();
    if (datalink_) datalink_->start ();
    if (mop_)      mop_->start ();
    if (routing_)  routing_->start ();
    if (nsp_)      nsp_->start ();
    if (session_)  session_->start ();
    if (http_)     http_->start ();
    // Last, and only once the loop below is about to run: it hands its
    // results to the node thread.
    if (node_fetcher_) node_fetcher_->start ();
    thread_ = std::thread ([this] { mainloop (); });
}

void Node::stop_layers ()
{
    // Reverse start order: session control releases connections before NSP
    // frees them, and remote event sinks close before session control stops.
    if (event_logger_) event_logger_->stop_remote ();
    if (session_)  session_->stop ();
    if (nsp_)      nsp_->stop ();
    if (routing_)  routing_->stop ();
    if (mop_)      mop_->stop ();
    if (datalink_) datalink_->stop ();
    if (event_logger_) event_logger_->stop ();
}

void Node::stop ()
{
    // Stop the monitoring server and the name fetcher first, from this
    // thread.  Both post work to the node loop, so both must be joined
    // while the loop is still running.
    if (http_) http_->stop ();
    if (node_fetcher_) node_fetcher_->stop ();

    if (thread_.joinable ()) {
        // Stop the layers on the node thread, since only that thread may touch
        // layer state.  The queue is FIFO, so this runs after pending work and
        // before the shutdown sentinel.
        add_work (std::make_unique<CallbackWork> ([this] { stop_layers (); }));
        add_work (std::make_unique<Shutdown> ());
        thread_.join ();
    }
    timers_.shutdown ();
}

void Node::mainloop ()
{
    logging::set_thread_name (name_);
    for (;;) {
        WorkPtr w = queue_.get ();
        if (dynamic_cast<Shutdown *> (w.get ())) break;

        auto started = std::chrono::steady_clock::now ();
        DN_TRACE ("dispatching {}", w->kind ());
        try {
            w->dispatch ();
        } catch (const std::exception &e) {
            DN_ERROR ("exception dispatching {}: {}", w->kind (), e.what ());
        }
        std::chrono::duration<double> dt =
            std::chrono::steady_clock::now () - started;
        stats_.add (w->kind (), dt.count ());
        if (dt.count () > 0.5)
            DN_TRACE ("excessive run time {:.3f}s for work item {}",
                      dt.count (), w->kind ());
    }
    DN_DEBUG ("node {} main loop exiting", name_);
}

}   // namespace decnet
