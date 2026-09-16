#include "decnet/routing/adjacency.h"

#include "decnet/common/logging.h"
#include "decnet/node.h"
#include "decnet/routing/ptp.h"
#include "decnet/routing/routing.h"

namespace decnet::routing {

Adjacency::Adjacency (Circuit *circuit, const AdjacencyInfo &info,
                      double circuit_t3)
    : Element (circuit), circuit_ (circuit), info_ (info)
{
    if (!circuit) { t4_ = 0; return; }      // the self adjacency
    // The listen timeout comes from the neighbour's own hello timer where
    // it sent one; Phase III and earlier do not, so our own is used.
    double base = info_.timer ? static_cast<double> (info_.timer) : circuit_t3;
    t4_ = base * PTP_T3MULT;
}

unsigned Adjacency::circuit_cost () const noexcept
{
    return circuit_ ? circuit_->cost () : 0;
}

void Adjacency::up ()
{
    if (up_) return;
    up_ = true;
    // Phase II neighbours are not required to send anything periodically,
    // so they get no listen timer.
    if (info_.ntype != PHASE2 && node ())
        node ()->timers ().start (this, t4_);
    if (auto *r = dynamic_cast<BaseRouter *> (parent ()->parent ()))
        r->adj_up (shared_from_this ());
}

void Adjacency::down ()
{
    if (!up_) return;
    up_ = false;
    if (node ()) node ()->timers ().stop (this);
    if (auto *r = dynamic_cast<BaseRouter *> (parent ()->parent ()))
        r->adj_down (shared_from_this ());
}

void Adjacency::alive ()
{
    if (info_.ntype != PHASE2 && node ())
        node ()->timers ().start (this, t4_);
}

void Adjacency::send (const RoutingPacketBase &pkt)
{
    DN_TRACE ("sending to nexthop {} on {}", info_.id.str (),
              circuit_->name ());
    if (auto *sd = dynamic_cast<const ShortData *> (&pkt)) {
        ShortData copy = *sd;
        circuit_->send_to (copy, *this);
    }
}

// ---------------------------------------------------------- SelfAdjacency

SelfAdjacency::SelfAdjacency (BaseRouter *router, Nodeid id,
                              std::uint8_t ntype)
    : Adjacency (nullptr, AdjacencyInfo { id, ntype, MTU, tiver_ph4, 0, 0, 4 },
                 0.0),
      router_ (router)
{
    // The self adjacency is always up.
    up_ = true;
}

void SelfAdjacency::send (const RoutingPacketBase &pkt)
{
    if (auto *sd = dynamic_cast<const ShortData *> (&pkt)) {
        ShortData copy = *sd;
        router_->deliver (copy);
    }
}

void Adjacency::timeout ()
{
    // The neighbour has gone quiet for longer than the listen timer.
    DN_DEBUG ("adjacency to {} timed out on {}", info_.id.str (),
              circuit_->name ());
    down ();
    circuit_->adj_timeout (this);
}

}   // namespace decnet::routing
