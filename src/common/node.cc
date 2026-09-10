#include "decnet/node.h"

#include "decnet/common/logging.h"
#include "decnet/config.h"
#include "decnet/datalink/datalink.h"
#include "decnet/events/events.h"
#include "decnet/events/logger.h"
#include "decnet/mop/mop.h"
#include "decnet/nsp/nsp.h"
#include "decnet/routing/routing.h"
#include "decnet/session/session.h"

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

    for (const auto &n : config.nodes ())
        add_node (Nodeinfo { n.id, n.name,
                             n.inbound_verification, n.outbound_verification });

    if (Nodeinfo *self = find_node (id_, false); self && !self->name.empty ())
        name_ = self->name;
    else if (!config.node_name ().empty ())
        name_ = config.node_name ();
    else
        name_ = id_.str ();

    logging::set_thread_name (name_);
    DN_DEBUG ("initializing node {}", name_);

    // Layer objects, in node.Node.__init__ order.  The event logger comes
    // first, so that everything built after it can report what it does.
    // PORT: a bridge-only configuration replaces the lot with a bridge.
    event_logger_ = std::make_unique<events::EventLogger> (this, config);
    datalink_ = std::make_unique<datalink::DatalinkLayer> (this, config);
    // MOP runs on broadcast circuits that ask for it, whether or not this
    // node routes.
    mop_ = std::make_unique<mop::Mop> (this, config);
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

Nodeinfo *Node::find_node (Nodeid id, bool add)
{
    auto it = by_id_.find (id.value ());
    if (it != by_id_.end ()) return it->second.get ();
    if (!add) return nullptr;
    // No entry: add a nameless one, which is what NSP's node database
    // needs.  Port of Node.nodeinfo's add behaviour.
    add_node (Nodeinfo { id, "", "", "" });
    return by_id_[id.value ()].get ();
}

Nodeinfo *Node::find_node (const std::string &name)
{
    auto it = by_name_.find (name);
    return it == by_name_.end () ? nullptr : it->second;
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

void Node::logevent (events::Event &e)
{
    // The source is always this node: an event is reported by whoever saw
    // it, and a record forwarded from elsewhere goes through
    // logremoteevent instead.
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
    thread_ = std::thread ([this] { mainloop (); });
}

void Node::stop_layers ()
{
    // The reverse of the order they were started, and each one above the
    // layer it depends on: session control lets go of its connections
    // before NSP frees them, and the event logger's remote sinks let go
    // before session control does.
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
    if (thread_.joinable ()) {
        // Stopping happens on the node's own thread, not the caller's.
        //
        // Every layer here holds state that the main loop dispatches into,
        // and the whole design rests on only one thread touching it.  A
        // caller that stopped the layers itself would be freeing objects
        // out from under work already in flight -- NSP connections being
        // the ones that showed it.  See BUGS.md.
        //
        // The work queue is FIFO, so this runs after everything already
        // queued and before the shutdown sentinel behind it, which is what
        // gives the layers a running loop to stop against.
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
