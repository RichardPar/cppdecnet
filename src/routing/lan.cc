#include "decnet/routing/lan.h"
#include "decnet/events/events.h"

#include "decnet/common/logging.h"
#include "decnet/config.h"
#include "decnet/node.h"
#include "decnet/routing/routing.h"

#include <algorithm>

namespace decnet::routing {

using datalink::all_endnodes;
using datalink::all_routers;
using datalink::ROUTING_PROTO;

namespace {

// The cache entry lifetime.  Port of NiCacheEntry.cachetime.
constexpr double CACHE_TIME = 60.0;

// DR election order: higher priority wins, ties broken by higher address.
// Port of route_eth.sortkey.
bool better_dr (std::uint8_t prio_a, Nodeid a, std::uint8_t prio_b, Nodeid b)
{
    if (prio_a != prio_b) return prio_a > prio_b;
    return a > b;
}

}   // namespace

// ------------------------------------------------------------ LanCircuit

LanCircuit::LanCircuit (BaseRouter *parent, std::string name,
                        datalink::BcDatalink *dl, const CircuitConfig &config)
    : Circuit (parent, std::move (name)), parent_ (parent), datalink_ (dl)
{
    t3_ = config.t3 ? static_cast<double> (config.t3) : 10.0;
    cost_ = config.cost ? config.cost : 4;
    port_ = dl->create_bc_port (this, ROUTING_PROTO);
    // A DECnet node's LAN address is derived from its node number, which is
    // how a neighbour can address it without any prior exchange.
    port_->set_macaddr (Macaddr::from_nodeid (parent->nodeid ()));
}

LanCircuit::~LanCircuit () = default;

void LanCircuit::start ()
{
    send_hello ();
    if (node ()) node ()->timers ().start (this, t3_);
}

void LanCircuit::stop ()
{
    if (node ()) node ()->timers ().stop (this);
}

std::size_t LanCircuit::adjacency_count () const
{
    return static_cast<std::size_t> (
        std::count_if (adjacencies_.begin (), adjacencies_.end (),
                       [] (const auto &kv)
                       { return kv.second.state == AdjState::up; }));
}

void LanCircuit::timeout ()
{
    send_hello ();
    if (node ()) node ()->timers ().start (this, t3_);
}

std::unique_ptr<RoutingPacketBase>
LanCircuit::decode (const Bytes &frame) const
{
    if (frame.empty ()) {
        DN_DEBUG ("null routing layer packet received on {}", name_);
        return nullptr;
    }
    ByteView buf (frame.data (), frame.size ());

    // A padded packet: the low seven bits of the first byte give the total
    // pad length, the pad header included.  Two layers of padding is not a
    // thing, so a second one means the packet is malformed.
    if (buf[0] & 0x80) {
        std::size_t pad = buf[0] & 0x7f;
        if (pad == 0 || pad >= buf.size ()) {
            DN_DEBUG ("bad padding on {}", name_);
            return nullptr;
        }
        buf = buf.subspan (pad);
        if (buf[0] & 0x80) {
            DN_DEBUG ("double padded packet received on {}", name_);
            return nullptr;
        }
    }
    return RoutingPacketBase::parse_frame (buf);
}

void LanCircuit::dispatch (Work &w)
{
    auto *r = dynamic_cast<Received *> (&w);
    if (!r) return;

    // The datalink hands up the frame's own source address alongside the
    // payload.  That address, rather than one derived from the node id in
    // the packet, is what a neighbour is addressed by; see the note in
    // EndnodeLanCircuit::handle.
    auto pkt = decode (r->packet ());
    if (!pkt) return;
    handle (*pkt, r->src ());
}

bool LanCircuit::send_to (ShortData &pkt, const Adjacency &adj)
{
    // Address the neighbour where it demonstrably receives -- the source of
    // the frames it sends us, recorded when its hello arrived -- rather than
    // the address its node number implies.  See the note in
    // EndnodeLanCircuit::handle: BAJI announces aa-00-04-00-13-04 and
    // answers on 08-00-2b-11-22-33, so forwarding to the derived address
    // loses every packet while the adjacency stays up.  The derived address
    // is the fallback for a neighbour we have somehow not heard from, which
    // is all the Python ever uses (`Adjacency.macid`).
    auto it = adjacencies_.find (adj.nodeid ().value ());
    Macaddr mac = adj.macid ();
    if (it != adjacencies_.end () && it->second.macaddr != Macaddr {})
        mac = it->second.macaddr;
    send_to_mac (pkt, mac);
    return true;                // a LAN has no notion of "unreachable"
}

void LanCircuit::send_update (const Bytes &frame)
{
    if (port_) port_->send (frame, all_routers ());
}

bool LanCircuit::wants_updates (unsigned level) const
{
    // Send only if somebody on this LAN would use it: any router for
    // level 1, an area router for level 2.
    //
    // the Python has no such test -- it sends to ALL_ROUTERS every t1
    // regardless (`routing.py`, Update.dispatch) -- and this was changed to
    // match it on 10-Sep-2026. That made things worse against the real
    // PDP-11 on the wired segment: with the gate the adjacency flapped,
    // without it the adjacency stopped forming at all. Reverted, and the
    // difference is recorded in BUGS.md rather than guessed at.
    //
    // The gate is still questionable on its own terms: a router that has
    // just come up and has not yet heard a peer stays silent, and a peer
    // waiting to hear from it waits for the message being withheld. Worth
    // revisiting with a second router to test against, rather than an
    // endnode that is upset by the extra traffic.
    for (const auto &[key, a] : adjacencies_) {
        if (a.state != AdjState::up || a.ntype == ENDNODE) continue;
        if (level == 2 && a.ntype != L2ROUTER) continue;
        return true;
    }
    return false;
}

void LanCircuit::adj_timeout (Adjacency *adj)
{
    if (adj) adjacency_down (adj->nodeid ().value ());
}

void LanCircuit::lanevent (events::EventId ev, Nodeid neighbour, int reason)
{
    Node *n = node ();
    if (!n) return;
    events::Event e { ev, nice::Entity::make_circuit (name_) };
    e.param (events::param::adjacent_node,
             events::node_value (n->nicenode (neighbour)));
    if (reason >= 0)
        e.coded (events::param::reason, static_cast<std::uint64_t> (reason));
    n->logevent (e);
}

void LanCircuit::adjacency_up (std::uint16_t key, const AdjacencyInfo &info)
{
    LanAdjacency &a = adjacencies_[key];
    a.state = AdjState::up;
    // A router already has an adjacency object, made when its first hello
    // arrived; only an endnode, which needs no handshake, is created here.
    // Reused rather than replaced so that a timeout work item already in
    // flight still refers to a live object.
    if (!a.adj) a.adj = std::make_shared<Adjacency> (this, info, t3_);
    // This is what puts the neighbour into the routing table: a router
    // gets a column, an endnode an entry in the shared endnode column.
    a.adj->up ();
    DN_INFO ("{} adjacency up: {} ({})", name_, info.id.str (),
             ntype_string (info.ntype));
    lanevent ({ 4, 15 }, info.id);          // adjacency up
}

void LanCircuit::adjacency_down (std::uint16_t key)
{
    auto it = adjacencies_.find (key);
    if (it == adjacencies_.end ()) return;
    // An adjacency that never came up is dropped without an event: there
    // was nothing for an operator to have seen come up.  Port of deladj,
    // which logs only for state UP.
    if (it->second.adj && it->second.state == AdjState::up) {
        DN_INFO ("{} adjacency down: {}", name_,
                 it->second.adj->nodeid ().str ());
        lanevent ({ 4, 18 }, it->second.adj->nodeid (),
                  events::reason::listener_timeout);
        it->second.adj->down ();
    }
    adjacencies_.erase (it);
}

void LanCircuit::send_to_mac (ShortData &pkt, Macaddr nexthop)
{
    // A LAN carries the long header, which is what holds the Ethernet
    // addresses.  the Python converts here for the same reason.
    LongData ld;
    ld.rqr     = pkt.rqr;
    ld.rts     = pkt.rts;
    ld.ie      = pkt.ie;
    ld.dstnode = pkt.dstnode;
    ld.srcnode = pkt.srcnode;
    ld.visit   = pkt.visit;
    ld.payload = pkt.payload;
    port_->send (ld.encode_packet (), nexthop);
}

// --------------------------------------------------- EndnodeLanCircuit

EndnodeLanCircuit::EndnodeLanCircuit (BaseRouter *parent, std::string name,
                                      datalink::BcDatalink *dl,
                                      const CircuitConfig &config)
    : LanCircuit (parent, std::move (name), dl, config)
{
    // An endnode listens for the routers' announcements.
    port_->add_multicast (all_endnodes ());
}

void EndnodeLanCircuit::adj_timeout (Adjacency *adj)
{
    // Forget the router as well as the adjacency.  While dr_ still names
    // it, its next hello takes the "the router we are already using" path
    // in handle() below, which only refreshes the listen timer -- and the
    // adjacency it would refresh has just been erased, so nothing rebuilds
    // it and the endnode stays off the network for good.  The Python's
    // EndnodeLanCircuit.adj_timeout clears self.dr for this reason.
    if (adj && dr_ && dr_->first == adj->nodeid ()) dr_.reset ();
    LanCircuit::adj_timeout (adj);
}

void EndnodeLanCircuit::send_hello ()
{
    EndnodeHello h;
    h.tiver    = parent_->tiver ();
    h.id       = parent_->nodeid ();
    h.blksize  = ETHMTU;
    h.timer    = static_cast<std::uint16_t> (t3_);
    h.testdata = hello_testdata (50);
    // Name the router we are using, so it can tell we have chosen it.
    Macaddr n = dr_ ? dr_->second : Macaddr {};
    const auto &nb = n.bytes ();
    h.neighbor.assign (nb.begin (), nb.end ());
    port_->send (h.encode_packet (), all_routers ());
}

void EndnodeLanCircuit::handle (RoutingPacketBase &pkt, Macaddr src)
{
    if (auto *rh = dynamic_cast<RouterHello *> (&pkt)) {
        if (rh->id.area () != parent_->homearea ()) return;   // not ours
    // Address a neighbour by the source of the frame it sent, not by the
    // address derived from its node id.  The specification says the two are
    // the same -- a Phase IV node programs AA-00-04-00-xx-xx into its card
    // -- and the Python relies on that (`Adjacency.macid = Macaddr (nodeid)`).
    // A real PDP-11, BAJI on the test segment, does not: it announces
    // id aa-00-04-00-13-04 while transmitting from 08-00-2b-11-22-33, and a
    // loopback probe gets no answer at all on the derived address.  Since
    // hellos are multicast, believing the derived address gives an adjacency
    // that comes up and stays up while every unicast packet vanishes.  The
    // source we heard is the address the neighbour demonstrably receives on.
        Macaddr rmac = src;

        if (dr_ && dr_->first == rh->id) {
            // The router we are already using; just note it is alive.
            auto it = adjacencies_.find (rh->id.value ());
            if (it != adjacencies_.end () && it->second.adj)
                it->second.adj->alive ();
            return;
        }
        if (dr_) {
            DN_DEBUG ("{} designated router changed from {} to {}", name_,
                      dr_->first.str (), rh->id.str ());
            adjacency_down (dr_->first.value ());
        } else {
            DN_INFO ("{} using designated router {}", name_, rh->id.str ());
        }
        dr_ = std::make_pair (rh->id, rmac);
        adjacencies_[rh->id.value ()].macaddr = rmac;

        AdjacencyInfo info;
        info.id      = rh->id;
        info.ntype   = (rh->ntype == RouterHello::ntype_l2) ? L2ROUTER
                                                            : L1ROUTER;
        info.blksize = std::min (rh->blksize, ETHMTU);
        info.tiver   = rh->tiver;
        info.timer   = rh->timer;
        info.priority = rh->prio;
        adjacency_up (rh->id.value (), info);
        return;
    }
    if (dynamic_cast<EndnodeHello *> (&pkt)) {
        // Another endnode; nothing for us to do with it.
        return;
    }

    // A data packet.  Remember who delivered it, so a reply can go back
    // the same way rather than via the router.
    ShortData sd;
    if (auto *ld = dynamic_cast<LongData *> (&pkt)) {
        sd.rqr = ld->rqr; sd.rts = ld->rts;
        sd.dstnode = ld->dstnode; sd.srcnode = ld->srcnode;
        sd.visit = ld->visit; sd.payload = ld->payload;
    } else if (auto *s = dynamic_cast<ShortData *> (&pkt)) {
        sd = *s;
    } else {
        return;
    }
    expire_cache ();
    cache_[sd.srcnode.value ()] = CacheEntry {
        src, std::chrono::steady_clock::now ()
             + std::chrono::seconds (static_cast<int> (CACHE_TIME)) };
    parent_->forward (sd);
}

void EndnodeLanCircuit::expire_cache ()
{
    auto now = std::chrono::steady_clock::now ();
    for (auto it = cache_.begin (); it != cache_.end (); )
        it = (it->second.expires <= now) ? cache_.erase (it) : std::next (it);
}

bool EndnodeLanCircuit::send (ShortData &pkt, bool tryhard)
{
    expire_cache ();
    std::uint16_t dst = pkt.dstnode.value ();

    if (tryhard) {
        // A retransmit: the cached path may be why the first try failed.
        cache_.erase (dst);
    } else if (auto it = cache_.find (dst); it != cache_.end ()) {
        send_to_mac (pkt, it->second.prevhop);
        return true;
    }
    if (dr_) {
        send_to_mac (pkt, dr_->second);
        return true;
    }
    if (pkt.dstnode == parent_->nodeid ()) return false;
    // No router known: address the destination directly and hope it is on
    // this LAN.  That is all an endnode can do.
    //
    // The derived address has to serve here, because a node id is the only
    // thing we have: nothing has been heard from this destination, so there
    // is no source address to prefer.  A neighbour that does not listen on
    // its derived address is unreachable this way until it sends us
    // something and the cache above picks up where it really lives.
    // the Python has the same fallback.
    send_to_mac (pkt, Macaddr::from_nodeid (pkt.dstnode));
    return true;
}

// --------------------------------------------------- RoutingLanCircuit

RoutingLanCircuit::RoutingLanCircuit (BaseRouter *parent, std::string name,
                                      datalink::BcDatalink *dl,
                                      const CircuitConfig &config)
    : LanCircuit (parent, std::move (name), dl, config),
      drtimer_ ([this] { become_dr (); })
{
    port_->add_multicast (all_routers ());
    if (config.priority) prio_ = static_cast<std::uint8_t> (config.priority);
    if (config.maxrouters) maxrouters_ = config.maxrouters;
}

void RoutingLanCircuit::start ()
{
    LanCircuit::start ();
    calc_dr ();
}

void RoutingLanCircuit::stop ()
{
    if (node ()) node ()->timers ().stop (&drtimer_);
    // Announce an empty router list on the way out, so neighbours drop us
    // promptly instead of waiting for the listen timer.
    if (port_) {
        RouterHello h;
        h.tiver   = parent_->tiver ();
        h.id      = parent_->nodeid ();
        h.ntype   = (parent_->ntype () == L2ROUTER) ? RouterHello::ntype_l2
                                                    : RouterHello::ntype_l1;
        h.blksize = ETHMTU;
        h.prio    = prio_;
        h.timer   = static_cast<std::uint16_t> (t3_);
        h.elist   = build_elist (true);
        port_->send (h.encode_packet (), all_routers ());
    }
    LanCircuit::stop ();
}

Bytes RoutingLanCircuit::build_elist (bool empty) const
{
    Bytes rslist;
    if (!empty) {
        for (const auto &[key, a] : adjacencies_) {
            if (a.ntype == ENDNODE) continue;
            RSent e;
            e.router = Nodeid (static_cast<std::uint16_t> (key));
            e.prio   = a.prio;
            e.twoway = (a.state == AdjState::up);
            Bytes b = e.encode ();
            rslist.insert (rslist.end (), b.begin (), b.end ());
        }
    }
    Elist el;
    el.rslist = std::move (rslist);
    return el.encode ();
}

void RoutingLanCircuit::adjacency_down (std::uint16_t key)
{
    // What it was has to be read before the entry goes: the clean-up
    // depends on whether it was a router.
    auto it = adjacencies_.find (key);
    if (it == adjacencies_.end ()) return;
    bool   was_router = it->second.ntype != ENDNODE;
    Nodeid id (static_cast<std::uint16_t> (key));

    LanCircuit::adjacency_down (key);

    // A designated router we can no longer hear is not the designated
    // router.  Leaving dr_ naming it is quietly serious: calc_dr () below
    // would see no change and do nothing, so nothing on this LAN ever
    // takes over, and while isdr_ stays false this router sends its hellos
    // only to the other routers -- the endnodes hear nobody, lose their
    // own adjacency in turn, and the LAN stays down.  Port of
    // RoutingLanCircuit.deladj.
    if (dr_ == id) dr_ = Nodeid ();
    if (was_router) {
        calc_dr ();
        // The hello's router list has changed, so say so now rather than
        // at the next t3.  The Python's newhello holds this off for T2
        // where one has just gone out; one extra frame on a neighbour
        // loss is not worth the state to track that.
        send_hello ();
    }
}

void RoutingLanCircuit::send_hello ()
{
    RouterHello h;
    h.tiver   = parent_->tiver ();
    h.id      = parent_->nodeid ();
    h.ntype   = (parent_->ntype () == L2ROUTER) ? RouterHello::ntype_l2
                                                : RouterHello::ntype_l1;
    h.blksize = ETHMTU;
    h.prio    = prio_;
    h.timer   = static_cast<std::uint16_t> (t3_);
    h.elist   = build_elist ();

    Bytes frame = h.encode_packet ();
    port_->send (frame, all_routers ());
    // Only the designated router talks to the endnodes; that is the point
    // of electing one.
    if (isdr_) port_->send (frame, all_endnodes ());
}

void RoutingLanCircuit::handle (RoutingPacketBase &pkt, Macaddr src)
{
    if (auto *rh = dynamic_cast<RouterHello *> (&pkt)) {
        // Out of area hellos are ignored, unless both ends are area
        // routers -- the only pair allowed to span areas.
        bool both_l2 = rh->ntype == RouterHello::ntype_l2
                    && parent_->ntype () == L2ROUTER;
        if (rh->id.area () != parent_->homearea () && !both_l2) return;

        bool is_new = adjacencies_.find (rh->id.value ()) == adjacencies_.end ();
        LanAdjacency &a = adjacencies_[rh->id.value ()];
        // See the note in EndnodeLanCircuit::handle: the address a neighbour
        // sends from is the one it receives on, which need not be derived.
        a.macaddr = src;
        a.prio    = rh->prio;
        a.ntype   = (rh->ntype == RouterHello::ntype_l2) ? L2ROUTER : L1ROUTER;
        a.listen_time = rh->timer * BCT3MULT;

        // Give it an adjacency object now, before two-way is confirmed,
        // because that object owns the listen timer -- and something has to
        // age this entry out. A router heard once and then gone otherwise
        // stays in this table for good and goes on winning the designated
        // router election below, so this node never takes over, and the
        // endnodes on the LAN are left with no router at all.  The Python
        // creates the adjacency on the first hello, in state INIT, for the
        // same reason; it starts the timer on the second hello and we start
        // it on the first, which only means a router heard exactly once
        // also expires.
        if (!a.adj) {
            AdjacencyInfo init;
            init.id       = rh->id;
            init.ntype    = a.ntype;
            init.blksize  = std::min (rh->blksize, ETHMTU);
            init.tiver    = rh->tiver;
            init.timer    = rh->timer;
            init.priority = rh->prio;
            a.adj = std::make_shared<Adjacency> (this, init, t3_);
        }
        a.adj->alive ();

        // Look for ourselves in its router list.  Finding it is the only
        // proof we have that this neighbour can hear us.
        bool listed = false;
        Elist el;
        try {
            el.decode (ByteView (rh->elist.data (), rh->elist.size ()));
            ByteView rs (el.rslist.data (), el.rslist.size ());
            while (rs.size () >= RSent::wire_size) {
                RSent e;
                e.decode (rs.subspan (0, RSent::wire_size));
                if (e.router == parent_->nodeid ()) { listed = true; break; }
                rs = rs.subspan (RSent::wire_size);
            }
        } catch (const DecodeError &) {
            DN_TRACE ("bad elist in hello from {} on {}", rh->id.str (), name_);
        }

        if (listed && a.state != AdjState::up) {
            AdjacencyInfo info;
            info.id      = rh->id;
            info.ntype   = a.ntype;
            info.blksize = std::min (rh->blksize, ETHMTU);
            info.tiver   = rh->tiver;
            info.timer   = rh->timer;
            info.priority = rh->prio;
            adjacency_up (rh->id.value (), info);
        } else if (a.state == AdjState::up) {
            // Already up and still talking; alive() above is all it needed.
        } else if (is_new) {
            DN_DEBUG ("{} heard router {}, waiting for two-way", name_,
                      rh->id.str ());
        }
        calc_dr ();
        return;
    }

    if (auto *eh = dynamic_cast<EndnodeHello *> (&pkt)) {
        if (eh->id.area () != parent_->homearea ()) return;
        LanAdjacency &a = adjacencies_[eh->id.value ()];
        if (a.state == AdjState::up) {
            if (a.adj) a.adj->alive ();
            return;
        }
        // As above: believe the frame's source, not the derived address.
        a.macaddr = src;
        a.ntype   = ENDNODE;
        a.listen_time = eh->timer * BCT3MULT;
        // An endnode needs no handshake: hearing it is enough.
        AdjacencyInfo info;
        info.id      = eh->id;
        info.ntype   = ENDNODE;
        info.blksize = std::min (eh->blksize, ETHMTU);
        info.tiver   = eh->tiver;
        info.timer   = eh->timer;
        adjacency_up (eh->id.value (), info);
        return;
    }

    if (auto *rm = dynamic_cast<RoutingMessage *> (&pkt)) {
        // Attribute it to whichever neighbour sent it.
        auto it = adjacencies_.find (
            Nodeid (static_cast<std::uint16_t> (rm->srcnode)).value ());
        if (it != adjacencies_.end () && it->second.adj
            && it->second.state == AdjState::up) {
            it->second.adj->alive ();
            parent_->routing_message (*rm, it->second.adj.get (), cost_);
        } else {
            DN_TRACE ("{} routing message from {} with no adjacency", name_,
                      rm->srcnode);
        }
        return;
    }

    ShortData sd;
    if (auto *ld = dynamic_cast<LongData *> (&pkt)) {
        sd.rqr = ld->rqr; sd.rts = ld->rts;
        sd.dstnode = ld->dstnode; sd.srcnode = ld->srcnode;
        sd.visit = ld->visit; sd.payload = ld->payload;
    } else if (auto *s = dynamic_cast<ShortData *> (&pkt)) {
        sd = *s;
    } else {
        return;
    }
    (void) src;
    parent_->forward (sd);
}

bool RoutingLanCircuit::two_way (Nodeid id) const
{
    auto it = adjacencies_.find (id.value ());
    return it != adjacencies_.end () && it->second.state == AdjState::up;
}

bool RoutingLanCircuit::best_dr (Nodeid &who) const
{
    std::uint8_t best_prio = prio_;
    Nodeid       best_id = parent_->nodeid ();
    bool         self = true;

    for (const auto &[key, a] : adjacencies_) {
        if (a.ntype == ENDNODE) continue;
        // Only routers in our own area can be designated router here.
        Nodeid id (static_cast<std::uint16_t> (key));
        if (id.area () != parent_->homearea ()) continue;
        if (better_dr (a.prio, id, best_prio, best_id)) {
            best_prio = a.prio;
            best_id = id;
            self = false;
        }
    }
    who = best_id;
    return self;
}

void RoutingLanCircuit::calc_dr ()
{
    Nodeid who;
    bool self = best_dr (who);

    if (self) {
        // Do not act on it for DRDELAY seconds: during startup several
        // nodes may each briefly believe they have won, and acting at once
        // would put two designated routers on the LAN.
        if (!isdr_ && !drtimer_running_) {
            DN_DEBUG ("{} designated router will be self, after {} seconds",
                      name_, DRDELAY);
            drtimer_running_ = true;
            if (node ()) node ()->timers ().start (&drtimer_, DRDELAY);
        }
        return;
    }
    if (isdr_) {
        isdr_ = false;
        send_hello ();
    }
    if (dr_ != who) {
        drtimer_running_ = false;
        if (node ()) node ()->timers ().stop (&drtimer_);
        dr_ = who;
        DN_DEBUG ("{} designated router is {}", name_, who.str ());
    }
}

void RoutingLanCircuit::become_dr ()
{
    drtimer_running_ = false;
    Nodeid who;
    if (best_dr (who)) {
        isdr_ = true;
        dr_ = parent_->nodeid ();
        DN_INFO ("{} designated router is self", name_);
        send_hello ();
    } else {
        // Somebody better turned up while we were waiting.
        calc_dr ();
    }
}

}   // namespace decnet::routing
