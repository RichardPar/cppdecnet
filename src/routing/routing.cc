#include "decnet/routing/routing.h"
#include "decnet/events/events.h"

#include "decnet/common/logging.h"
#include "decnet/config.h"
#include "decnet/datalink/datalink.h"
#include "decnet/node.h"
#include "decnet/nsp/nsp.h"
#include "decnet/datalink/bc.h"
#include "decnet/routing/l1router.h"
#include "decnet/routing/lan.h"
#include "decnet/routing/ptp.h"

namespace decnet::routing {

unsigned CircuitCounters::seconds_since_up () const noexcept
{
    if (!ever_up ()) return 0;
    std::chrono::duration<double> dt =
        std::chrono::steady_clock::now () - last_up;
    return static_cast<unsigned> (dt.count ());
}

// ------------------------------------------------------------ BaseRouter

BaseRouter::BaseRouter (Element *parent, const Config &config)
    : Element (parent)
{
    DN_DEBUG ("initializing routing layer");
    const auto &rc = config.routing ();
    if (!rc) throw InternalError ("no routing configuration");

    nodeid_   = rc->id;
    phase_    = phase_of (rc->type);
    maxnodes_ = rc->maxnodes;
    maxarea_  = rc->maxarea;

    if (phase_ == Phase::ph4 && nodeid_.area () == 0)
        throw std::invalid_argument ("no area number for executor node "
                                     + nodeid_.str ());
    if (phase_ != Phase::ph4 && nodeid_.area () != 0)
        throw std::invalid_argument ("executor node " + nodeid_.str ()
                                     + " should not have an area number");

    if (Nodeinfo *self = node ()->find_node (nodeid_, false))
        name_ = self->name;
    if (name_.empty ())
        throw std::invalid_argument ("no node name set for executor node "
                                     + nodeid_.str ());

}

void BaseRouter::init_circuits (const Config &config)
{
    // One routing circuit per datalink circuit.
    datalink::DatalinkLayer *dll = node ()->datalink ();
    for (const CircuitConfig &c : config.circuits ()) {
        datalink::Datalink *dl = dll ? dll->circuit (c.name) : nullptr;
        if (!dl) continue;      // the datalink layer has already logged why
        try {
            // A broadcast datalink gets a LAN circuit, everything else a
            // point to point one.  Port of BaseRouter.routing_circuit.
            if (auto *bc = dynamic_cast<datalink::BcDatalink *> (dl)) {
                std::unique_ptr<LanCircuit> lc;
                if (ntype () == ENDNODE)
                    lc = std::make_unique<EndnodeLanCircuit> (this, c.name,
                                                              bc, c);
                else
                    lc = std::make_unique<RoutingLanCircuit> (this, c.name,
                                                              bc, c);
                LanCircuit *raw = lc.get ();
                lan_circuits_[c.name] = std::move (lc);
                lan_order_.push_back (raw);
                DN_DEBUG ("initialized LAN routing circuit {}", c.name);
            } else {
                auto rc2 = std::make_unique<PtpCircuit> (this, c.name, dl, c);
                PtpCircuit *raw = rc2.get ();
                circuits_[c.name] = std::move (rc2);
                circuit_order_.push_back (raw);
                DN_DEBUG ("initialized routing circuit {}", c.name);
            }
        } catch (const std::exception &e) {
            DN_ERROR ("error initializing routing circuit {}: {}",
                      c.name, e.what ());
        }
    }
}

BaseRouter::~BaseRouter ()
{
    if (node ()) {
        node ()->timers ().stop (&reaper_);
        if (datalink::DatalinkLayer *dll = node ()->datalink ())
            if (dll->dynamic_owner () == this) dll->set_dynamic_owner (nullptr);
    }
}

// ------------------------------------------------------ dynamic circuits

const char *BaseRouter::claim_conflict (Nodeid id, const Circuit *asking,
                                        std::string &where) const
{
    where.clear ();
    if (id == nodeid_) return "it is this node's own address";
    // Up on some circuit, point to point or LAN: adj_up has registered it.
    // One left over on the asking circuit itself is that circuit's own
    // business, not a second claim.
    if (AdjacencyPtr a = find_adjacency (id); a && a->circuit () != asking) {
        if (a->circuit ()) where = a->circuit ()->name ();
        return "it is already a neighbour";
    }
    // Past its routing init but not yet up: verifying.
    for (const PtpCircuit *c : circuit_order_) {
        if (c != asking && c->claims (id)) {
            where = c->name ();
            return "another circuit is initializing with it";
        }
    }
    return nullptr;
}

bool BaseRouter::add_dynamic_circuit (const CircuitConfig &c,
                                      datalink::Datalink *dl)
{
    if (!dl || dynamic_cast<datalink::BcDatalink *> (dl)) return false;
    if (circuits_.count (c.name) || lan_circuits_.count (c.name)) {
        DN_ERROR ("routing circuit {} already exists", c.name);
        return false;
    }
    try {
        auto pc = std::make_unique<PtpCircuit> (this, c.name, dl, c);
        PtpCircuit *raw = pc.get ();
        circuits_[c.name] = std::move (pc);
        circuit_order_.push_back (raw);
        circuit_added (raw);
        raw->start ();
        DN_DEBUG ("added routing circuit {}", c.name);
        return true;
    } catch (const std::exception &e) {
        DN_ERROR ("error adding routing circuit {}: {}", c.name, e.what ());
        if (auto it = circuits_.find (c.name); it != circuits_.end ()) {
            std::erase (circuit_order_, it->second.get ());
            circuits_.erase (it);
        }
        return false;
    }
}

void BaseRouter::retire_dynamic_circuit (const std::string &name,
                                         std::unique_ptr<datalink::Datalink> dl)
{
    Grave g;
    g.when = std::chrono::steady_clock::now ();
    if (auto it = circuits_.find (name); it != circuits_.end ()) {
        PtpCircuit *c = it->second.get ();
        // Takes any adjacency down, so the routes through it are recomputed
        // now, and makes the circuit ignore whatever is still queued for it.
        if (AdjacencyPtr adj = c->retire ()) g.held.push_back (std::move (adj));
        circuit_removed (c, g.remains);
        std::erase (circuit_order_, c);
        g.remains.push_back (std::move (it->second));
        circuits_.erase (it);
        DN_DEBUG ("removed routing circuit {}", name);
    }
    if (dl) g.remains.push_back (std::move (dl));
    graves_.push_back (std::move (g));
    if (node () && !reaper_.linked ())
        node ()->timers ().start (&reaper_, 5.0);
}

void BaseRouter::reap ()
{
    // Anything retired a few seconds ago: every work item that could
    // name it was queued before it was retired, and has long been run.
    const auto cutoff = std::chrono::steady_clock::now ()
                      - std::chrono::seconds (4);
    std::erase_if (graves_, [&] (const Grave &g) { return g.when <= cutoff; });
    if (!graves_.empty () && node ())
        node ()->timers ().start (&reaper_, 5.0);
}

namespace {

// The states the local node state change event reports.  The event has no
// entity: the node it is about is the one that raised it.
constexpr std::uint64_t st_on = 0, st_off = 1;

void node_state (Node *n, std::uint64_t old_state, std::uint64_t new_state)
{
    if (!n) return;
    events::Event e { { 2, 0 }, nice::Entity::make_none () };
    e.coded (0, 0);                         // reason: operator command
    e.coded (1, old_state);
    e.coded (2, new_state);
    n->logevent (e);
}

}   // namespace

void BaseRouter::start ()
{
    DN_DEBUG ("starting routing layer");
    node_state (node (), st_off, st_on);
    if (hold_circuits_)
        DN_DEBUG ("routing circuits held down until wanted");
    else
        start_circuits ();
}

void BaseRouter::stop ()
{
    DN_DEBUG ("stopping routing layer");
    node_state (node (), st_on, st_off);
    stop_circuits ();
}

void BaseRouter::start_circuits ()
{
    for (PtpCircuit *c : circuit_order_) {
        try {
            c->start ();
            DN_DEBUG ("started routing circuit {}", c->name ());
        } catch (const std::exception &e) {
            DN_ERROR ("error starting routing circuit {}: {}",
                      c->name (), e.what ());
        }
    }
    for (LanCircuit *c : lan_order_) {
        try {
            c->start ();
            DN_DEBUG ("started LAN routing circuit {}", c->name ());
        } catch (const std::exception &e) {
            DN_ERROR ("error starting LAN routing circuit {}: {}",
                      c->name (), e.what ());
        }
    }
}

void BaseRouter::stop_circuits ()
{
    for (LanCircuit *c : lan_order_) {
        try {
            c->stop ();
        } catch (const std::exception &e) {
            DN_ERROR ("error stopping LAN routing circuit {}: {}",
                      c->name (), e.what ());
        }
    }
    for (PtpCircuit *c : circuit_order_) {
        try {
            c->stop ();
        } catch (const std::exception &e) {
            DN_ERROR ("error stopping routing circuit {}: {}",
                      c->name (), e.what ());
        }
    }
}

LanCircuit *BaseRouter::lan_circuit (const std::string &name) const
{
    std::string key;
    try {
        key = circname (name);
    } catch (const std::invalid_argument &) {
        return nullptr;
    }
    auto it = lan_circuits_.find (key);
    return it == lan_circuits_.end () ? nullptr : it->second.get ();
}

PtpCircuit *BaseRouter::circuit (const std::string &name) const
{
    std::string key;
    try {
        key = circname (name);
    } catch (const std::invalid_argument &) {
        return nullptr;
    }
    auto it = circuits_.find (key);
    return it == circuits_.end () ? nullptr : it->second.get ();
}

void BaseRouter::adj_up (const AdjacencyPtr &adj)
{
    // Point to point circuits refuse a second neighbour with the same
    // address (claim_conflict); a LAN does not ask, so say when one
    // replaces another.
    if (AdjacencyPtr old = find_adjacency (adj->nodeid ());
        old && old != adj && old->circuit () != adj->circuit ())
        DN_WARN ("adjacency {} on {} replaces the one on {}",
                 adj->nodeid ().str (),
                 adj->circuit () ? adj->circuit ()->name () : std::string ("?"),
                 old->circuit () ? old->circuit ()->name () : std::string ("?"));
    adjacencies_[adj->nodeid ().value ()] = adj;
    DN_DEBUG ("adjacency up: {}", adj->nodeid ().str ());
}

void BaseRouter::adj_down (const AdjacencyPtr &adj)
{
    adjacencies_.erase (adj->nodeid ().value ());
    DN_DEBUG ("adjacency down: {}", adj->nodeid ().str ());
}

AdjacencyPtr BaseRouter::find_adjacency (Nodeid id) const
{
    auto it = adjacencies_.find (id.value ());
    return it == adjacencies_.end () ? nullptr : it->second;
}

// -------------------------------------------------------- EndnodeRouting

EndnodeRouting::EndnodeRouting (Element *parent, const Config &config)
    : BaseRouter (parent, config)
{
    // A Multinet server makes a circuit per connection, and an endnode may
    // only ever have one.
    for (const CircuitConfig &c : config.circuits ())
        if (c.type == "Multinet" && datalink::is_multinet_server (c.device))
            throw std::invalid_argument (
                "circuit " + c.name + ": Multinet server mode needs a "
                "router, not an end node");
    init_circuits (config);
    // The architecture allows an endnode exactly one circuit, of either
    // kind.
    std::size_t n = circuit_order_.size () + lan_order_.size ();
    if (n != 1)
        throw std::invalid_argument (
            "an end node must have exactly one circuit, found "
            + std::to_string (n));
}

void EndnodeRouting::send (Bytes data, Nodeid dest, bool rqr)
{
    // ShortData here; LAN circuits convert to LongData.
    ShortData pkt;
    pkt.rqr     = rqr;
    pkt.dstnode = dest;
    pkt.srcnode = nodeid_;
    pkt.visit   = 0;
    pkt.payload = std::move (data);

    if (dest == nodeid_) {
        // Addressed to ourselves: back up, by way of the work queue.
        deliver_later (std::move (pkt));
        return;
    }
    if (!lan_order_.empty ()) {
        // A LAN circuit converts to the long header itself and never
        // reports failure, so there is nothing to return to sender.
        ++lan_order_.front ()->counters ().orig_sent;
        static_cast<EndnodeLanCircuit *> (lan_order_.front ())->send (pkt);
        return;
    }
    ++circuit_order_.front ()->counters ().orig_sent;
    if (!circuit_order_.front ()->send (pkt) && rqr) {
        // Undeliverable, and the sender asked for it back.
        std::swap (pkt.dstnode, pkt.srcnode);
        pkt.rts = true;
        pkt.rqr = false;
        forward (pkt);
        DN_TRACE ("returned packet to sender");
    }
}

void EndnodeRouting::forward (ShortData &pkt)
{
    if (pkt.dstnode != nodeid_) {
        // An endnode does not forward; anything not for us is dropped.
        DN_TRACE ("dropping packet for {}, we are {}",
                  pkt.dstnode.str (), nodeid_.str ());
        return;
    }
    deliver (pkt);
}

void BaseRouter::deliver_later (ShortData pkt)
{
    if (Node *n = node ()) {
        n->add_work (std::make_unique<CallbackWork> (
            [this, pkt = std::move (pkt)] () mutable { deliver (pkt); }));
    } else {
        deliver (pkt);
    }
}

void BaseRouter::deliver (ShortData &pkt)
{
    ++for_us_;
    // Terminating traffic is counted on the circuit it arrived on.  A packet
    // that originated here as well crossed no circuit, so it is counted
    // nowhere -- the same reasoning as PyDECnet's SelfAdj.send.
    if (pkt.src) ++pkt.src->counters ().term_recv;
    DN_TRACE ("packet for us from {}, {} bytes of payload",
              pkt.srcnode.str (), pkt.payload.size ());
    if (nsp_)
        nsp_->deliver (pkt.srcnode,
                       ByteView (pkt.payload.data (), pkt.payload.size ()));
}

// ------------------------------------------------------------- factory

std::unique_ptr<BaseRouter> make_router (Element *parent, const Config &config)
{
    const auto &rc = config.routing ();
    if (!rc) return nullptr;
    switch (rc->type) {
    case NodeType::endnode:
        return std::make_unique<EndnodeRouting> (parent, config);
    case NodeType::l1router:
        return std::make_unique<L1Router> (parent, config);
    case NodeType::l2router:
        return std::make_unique<L2Router> (parent, config);
    default:
        // PORT: phase3router, phase3endnode and phase2 need their own
        // address handling.
        DN_ERROR ("routing type {} is not supported yet; "
                  "only endnode is implemented", name_of (rc->type));
        return nullptr;
    }
}

}   // namespace decnet::routing
