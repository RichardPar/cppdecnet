#include "decnet/routing/ptp.h"

#include "decnet/common/logging.h"
#include "decnet/config.h"
#include "decnet/node.h"
#include "decnet/routing/routing.h"

namespace decnet::routing {

using datalink::DlStatus;

PtpCircuit::PtpCircuit (BaseRouter *parent, std::string name,
                        datalink::Datalink *dl, const CircuitConfig &config)
    : Circuit (parent, std::move (name)), parent_ (parent)
{
    t3_ = config.t3 ? static_cast<double> (config.t3) : 60.0;
    cost_ = config.cost ? config.cost : 4;
    port_ = dl->create_port (this);
    if (!config.verify.empty ()) {
        // A verify string means we require the neighbour to authenticate.
        request_verify_ = true;
        expect_verify_.assign (config.verify.begin (), config.verify.end ());
    }
    clear_neighbour ();
    // The init message is built when sent, since the router's node type is
    // not available during construction.
}

void PtpCircuit::build_initmsg ()
{
    // PORT: Phase II and III init messages.
    initmsg_ = PtpInit {};
    initmsg_.srcnode = parent_->nodeid ();
    initmsg_.ntype   = parent_->ntype ();
    initmsg_.timer   = static_cast<std::uint16_t> (t3_);
    initmsg_.verif   = request_verify_;
    initmsg_.blksize = MTU;
    initmsg_.tiver   = parent_->tiver ();
}

void PtpCircuit::clear_neighbour ()
{
    info_ = AdjacencyInfo {};
    info_.ntype   = UNKNOWN;
    info_.blksize = MTU;      // until the neighbour tells us otherwise
    info_.rphase  = 0;        // we do not know its phase yet
    adj_.reset ();
}

void PtpCircuit::start ()
{
    if (node ()) node ()->add_work (std::make_unique<CircuitStart> (this));
}

void PtpCircuit::stop ()
{
    if (node ()) node ()->add_work (std::make_unique<CircuitStop> (this));
}

std::string PtpCircuit::statename () const
{
    return name_ + "<state: " + state_name () + ">";
}

bool PtpCircuit::running () const noexcept
{
    return const_cast<PtpCircuit *> (this)->in_state (
        State (&PtpCircuit::ru, "ru"));
}

int PtpCircuit::nice_substate () const noexcept
{
    // nice_substate_synchronizing / _starting in nicedefs; the values are
    // the ones route_ptp.py's @setcode decorators carry.
    auto *self = const_cast<PtpCircuit *> (this);
    if (self->in_state (State (&PtpCircuit::s0, "ha"))
        || self->in_state (State (&PtpCircuit::ds, "ds")))
        return 10;                      // Synchronizing
    if (self->in_state (State (&PtpCircuit::ri, "ri"))
        || self->in_state (State (&PtpCircuit::rv, "rv")))
        return 0;                       // Starting
    return -1;                          // Running: no substate
}

void PtpCircuit::dlsend (const RoutingPacketBase &pkt)
{
    port_->send (pkt.encode_packet ());
}

void PtpCircuit::timeout ()
{
    Timeout t (nullptr, nullptr, 0);
    StateMachine<PtpCircuit>::dispatch (t);
}

// ---------------------------------------------------------------- restart

void PtpCircuit::routeevent (events::EventId ev, int reason,
                             Bytes packet_beginning)
{
    Node *n = node ();
    if (!n) return;
    events::Event e { ev, nice::Entity::make_circuit (name_) };
    // The neighbour, when we have got far enough to know who it is.
    if (info_.id)
        e.param (events::param::adjacent_node,
                 events::node_value (n->nicenode (info_.id)));
    if (reason >= 0)
        e.coded (events::param::reason, static_cast<std::uint64_t> (reason));
    if (!packet_beginning.empty ())
        e.image (events::param::packet_beginning, std::move (packet_beginning));

    // Report the event after this dispatch, once the state change has been
    // applied.
    n->add_work (std::make_unique<CallbackWork> ([n, e] () mutable {
        n->logevent (e);
    }));
}

PtpCircuit::State PtpCircuit::restart (const char *why, events::EventId ev,
                                       int reason)
{
    routeevent (ev, reason);
    return restart (why);
}

PtpCircuit::State PtpCircuit::restart (const char *why)
{
    DN_TRACE ("{} restart due to {}", name_, why);
    if (running ()) down ();
    clear_neighbour ();
    if (node ()) node ()->timers ().stop (this);

    if (port_->start_works ()) {
        // Ask the datalink to restart.  It will report DlStatus UP when it
        // is running again, which the ds state waits for.
        port_->restart ();
    } else {
        // Multinet over UDP cannot report a remote restart, so there is
        // nothing to wait for: synthesise the UP ourselves.
        if (node ())
            node ()->add_work (
                std::make_unique<DlStatus> (this, DlStatus::Status::up));
    }
    return DN_MY_STATE (PtpCircuit, ds);
}

// -------------------------------------------------------------- validate

bool PtpCircuit::validate (Work &w)
{
    packet_ = nullptr;
    auto *r = dynamic_cast<Received *> (&w);
    if (!r) return true;

    if (r->packet ().empty ()) {
        DN_DEBUG ("null routing layer packet received on {}", name_);
        return false;
    }
    // Decode once, here, so each state works with a typed packet -- which
    // is what PyDECnet's validate does before dispatching to the state.
    decoded_ = RoutingPacketBase::parse_frame (r->packet ());
    if (!decoded_) {
        DN_DEBUG ("undecodable routing packet on {}: {}", name_,
                  hexdump (r->packet ()));
        // The first six bytes are what the event carries: enough for
        // someone reading the log to tell what arrived.
        const Bytes &p = r->packet ();
        routeevent ({ 4, 4 }, -1,
                    Bytes (p.begin (),
                           p.begin () + std::min<std::size_t> (6, p.size ())));
        return false;
    }
    packet_ = decoded_.get ();
    return true;
}

// ----------------------------------------------------------------- states

PtpCircuit::State PtpCircuit::s0 (Work &w)
{
    // "Halted".  A Start item, or a retry timeout, sets things going.
    if (dynamic_cast<CircuitStart *> (&w) || dynamic_cast<Timeout *> (&w)) {
        DN_TRACE ("starting {}", name_);
        port_->open ();
        clear_neighbour ();
        return DN_MY_STATE (PtpCircuit, ds);
    }
    return nullptr;
}

PtpCircuit::State PtpCircuit::ds (Work &w)
{
    // "Datalink started": waiting for the datalink to come up.  There is
    // deliberately no timeout here; see the note at the top of the header.
    if (auto *s = dynamic_cast<DlStatus *> (&w)) {
        if (s->is_up ()) {
            clear_neighbour ();
            build_initmsg ();
            dlsend (initmsg_);
            if (node ()) node ()->timers ().start (this, t3_);
            return DN_MY_STATE (PtpCircuit, ri);
        }
        // The datalink lost sync under us.
        return restart ("datalink down", { 4, 11 },
                        events::reason::sync_lost);
    }
    if (dynamic_cast<CircuitDown *> (&w)) return restart ("circuit down");
    if (dynamic_cast<CircuitStop *> (&w)) {
        routeevent ({ 4, 9 });              // circuit down, operator initiated
        port_->close ();
        return DN_MY_STATE (PtpCircuit, s0);
    }
    return nullptr;
}

PtpCircuit::State PtpCircuit::ri (Work &w)
{
    // "Routing init": our init is sent, we are waiting for theirs.
    if (dynamic_cast<Timeout *> (&w)) return restart ("init timeout");

    if (dynamic_cast<CircuitDown *> (&w)) return restart ("circuit down");
    if (auto *s = dynamic_cast<DlStatus *> (&w)) {
        if (!s->is_up ()) return restart ("datalink status");
        return nullptr;
    }
    if (dynamic_cast<CircuitStop *> (&w)) {
        routeevent ({ 4, 9 });              // circuit down, operator initiated
        port_->close ();
        return DN_MY_STATE (PtpCircuit, s0);
    }
    if (!packet_) return nullptr;

    if (auto *init = dynamic_cast<PtpInit *> (packet_)) {
        // A Phase IV neighbour.
        if (parent_->phase () < Phase::ph4) {
            DN_TRACE ("ignoring phase 4 init on {}", name_);
            return nullptr;
        }
        if (init->ntype != ENDNODE && init->ntype != L1ROUTER
            && init->ntype != L2ROUTER) {
            return restart ("bad ntype", { 4, 12 },
                            events::reason::unexpected_packet_type);
        }
        if (init->blo) return restart ("blocking requested");
        if (!init->check ()) return restart ("node id out of range");

        unsigned area = init->srcnode.area ();
        unsigned tid  = init->srcnode.tid ();
        // In range if the node number fits and the area matches, unless both ends
        // are area routers.
        bool both_l2 = init->ntype == L2ROUTER
                    && parent_->ntype () == L2ROUTER;
        bool router = parent_->ntype () == L1ROUTER
                   || parent_->ntype () == L2ROUTER;
        if ((router && tid > parent_->maxnodes ())
            || (both_l2 && !(area >= 1 && area <= parent_->maxarea ()))
            || (!both_l2 && area != parent_->homearea ())) {
            return restart ("node id out of range", { 4, 12 },
                            events::reason::address_out_of_range);
        }

        info_.rphase  = 4;
        info_.id      = init->srcnode;
        info_.timer   = init->timer;
        info_.ntype   = init->ntype;
        // Obey the smaller of the two block sizes: some implementations
        // send silly values, so PyDECnet clamps to its own MTU too.
        info_.blksize = std::min (init->blksize, MTU);
        info_.tiver   = init->tiver;

        hellomsg_ = PtpHello {};
        hellomsg_.srcnode  = parent_->nodeid ();
        hellomsg_.testdata = hello_testdata ();

        if (init->verif) {
            // The neighbour wants us to authenticate.
            PtpVerify v;
            v.fcnval = verify_;
            set_src (v.srcnode);
            if (verify_.empty ())
                DN_TRACE ("{} verification requested but not set, "
                          "attempting null string", name_);
            dlsend (v);
        }

        adj_ = std::make_shared<Adjacency> (this, info_, t3_);

        if (request_verify_) {
            // We asked them to authenticate; wait for it.
            if (node ()) node ()->timers ().start (this, t3_);
            return DN_MY_STATE (PtpCircuit, rv);
        }
        up ();
        return DN_MY_STATE (PtpCircuit, ru);
    }

    if (dynamic_cast<PtpInit3 *> (packet_)) {
        // PORT: Phase III neighbours.  Restart the circuit for now.
        DN_DEBUG ("{} phase III neighbour not supported yet", name_);
        return restart ("phase III neighbour");
    }

    if (dynamic_cast<PtpVerify *> (packet_))
        return restart ("unexpected verify", { 4, 12 },
                        events::reason::unexpected_packet_type);

    return nullptr;
}

PtpCircuit::State PtpCircuit::rv (Work &w)
{
    // "Routing verify": waiting for the neighbour's verification.
    if (dynamic_cast<Timeout *> (&w)) return restart ("verification timeout");
    if (dynamic_cast<CircuitDown *> (&w)) return restart ("circuit down");
    if (auto *s = dynamic_cast<DlStatus *> (&w)) {
        if (!s->is_up ()) return restart ("datalink status");
        return nullptr;
    }
    if (dynamic_cast<CircuitStop *> (&w)) {
        routeevent ({ 4, 9 });              // circuit down, operator initiated
        port_->close ();
        return DN_MY_STATE (PtpCircuit, s0);
    }
    if (!packet_) return nullptr;

    if (auto *v = dynamic_cast<PtpVerify *> (packet_)) {
        if (!check_src (v->srcnode))
            return restart ("verify from wrong node");
        if (v->fcnval != expect_verify_) {
            DN_DEBUG ("{} verification value mismatch", name_);
            return restart ("verification reject", { 4, 6 },
                            events::reason::invalid_verification);
        }
        up ();
        return DN_MY_STATE (PtpCircuit, ru);
    }
    return nullptr;
}

PtpCircuit::State PtpCircuit::ru (Work &w)
{
    // "Running": the circuit is up at the routing control layer.
    if (dynamic_cast<Timeout *> (&w)) {
        send_hello ();
        return nullptr;
    }
    if (dynamic_cast<CircuitDown *> (&w)) return restart ("circuit down");
    if (auto *s = dynamic_cast<DlStatus *> (&w)) {
        if (!s->is_up ()) return restart ("datalink status");
        return nullptr;
    }
    if (dynamic_cast<CircuitStop *> (&w)) {
        down ();
        port_->close ();
        return DN_MY_STATE (PtpCircuit, s0);
    }
    if (!packet_) return nullptr;

    // Anything received counts as the neighbour being alive.
    if (adj_) adj_->alive ();

    if (auto *h = dynamic_cast<PtpHello *> (packet_)) {
        if (!check_src (h->srcnode)) return restart ("hello from wrong node");
        if (!h->testdata_valid ())   return restart ("invalid test data");
        return nullptr;             // a good hello needs nothing further
    }
    if (auto *v = dynamic_cast<PtpVerify *> (packet_)) {
        if (!check_src (v->srcnode)) return restart ("verify from wrong node");
        return nullptr;
    }
    if (auto *sd = dynamic_cast<ShortData *> (packet_)) {
        parent_->forward (*sd);
        return nullptr;
    }
    if (auto *rm = dynamic_cast<RoutingMessage *> (packet_)) {
        // A routing message goes to the routing layer with the cost of the
        // circuit it arrived on, which the route computation folds in.
        if (adj_) parent_->routing_message (*rm, adj_.get (), cost_);
        return nullptr;
    }
    if (auto *ld = dynamic_cast<LongData *> (packet_)) {
        // Convert to the short form before handing it up; on a point to
        // point circuit the two carry the same information.
        ShortData sd;
        sd.rqr     = ld->rqr;
        sd.rts     = ld->rts;
        sd.dstnode = ld->dstnode;
        sd.srcnode = ld->srcnode;
        sd.visit   = ld->visit;
        sd.payload = ld->payload;
        parent_->forward (sd);
        return nullptr;
    }
    return nullptr;
}

// ------------------------------------------------------------------ misc

bool PtpCircuit::check_src (Nodeid src) const noexcept
{
    // The source of a control packet must match what the init told us.
    if (info_.rphase == 4) return src == info_.id;
    return src.tid () == info_.id.tid ();
}

void PtpCircuit::set_src (Nodeid &f) const noexcept
{
    f = (info_.rphase == 4) ? parent_->nodeid ()
                            : Nodeid (0u, parent_->tid ());
}

void PtpCircuit::up ()
{
    if (adj_) adj_->up ();
    DN_INFO ("circuit {} up, neighbour {} ({}), block size {}", name_,
             info_.id.str (), ntype_string (info_.ntype), info_.blksize);
    routeevent ({ 4, 10 });                 // circuit up
    if (node ()) node ()->timers ().start (this, t3_);
}

void PtpCircuit::down ()
{
    if (adj_) {
        DN_INFO ("circuit {} down, neighbour was {}", name_, info_.id.str ());
        adj_->down ();
    }
    adj_.reset ();
}

void PtpCircuit::send_hello ()
{
    if (!running ()) return;
    dlsend (hellomsg_);
    if (node ()) node ()->timers ().start (this, t3_);
}

void PtpCircuit::adj_timeout (Adjacency *)
{
    // Listen timer expired.  Raise the event (before clearing the neighbour)
    // and restart the circuit.  Port of PtpCircuit.adj_timeout.
    routeevent ({ 4, 8 }, events::reason::listener_timeout);
    adj_.reset ();
    set_state (restart ("listen timeout"));
}

void PtpCircuit::send_raw (const Bytes &frame)
{
    if (!running ()) return;
    port_->send (frame);
}

bool PtpCircuit::send (ShortData &pkt)
{
    if (!running ()) return false;
    // An endnode neighbour will only accept packets addressed to itself.
    if (info_.ntype == ENDNODE && pkt.dstnode != info_.id) {
        DN_DEBUG ("sending packet to wrong address {} on {} (expected {})",
                  pkt.dstnode.str (), name_, info_.id.str ());
        return false;
    }
    dlsend (pkt);
    return true;
}

}   // namespace decnet::routing
