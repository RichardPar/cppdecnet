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

BaseRouter::~BaseRouter () = default;

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

void BaseRouter::stop ()
{
    DN_DEBUG ("stopping routing layer");
    node_state (node (), st_on, st_off);
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
        // Addressed to ourselves: straight back up.
        forward (pkt);
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
