#include "decnet/nsp/nsp.h"
#include "decnet/events/events.h"

#include "decnet/common/logging.h"
#include "decnet/config.h"
#include "decnet/node.h"
#include "decnet/routing/routing.h"

#include <algorithm>
#include <random>

namespace decnet::nsp {

namespace {
// The largest payload a data segment may carry.  Port of common.MSS.
constexpr std::uint16_t MSS = 576 - 13;
constexpr unsigned MAX_RETRIES = 5;
// An interrupt message carries at most sixteen bytes.
constexpr std::size_t MAX_INTERRUPT = 16;
}

// ============================================================ Connection

Connection::Connection (NSP *parent, std::uint16_t srcaddr, Nodeid dest)
    : Element (parent), parent_ (parent), srcaddr_ (srcaddr), dest_ (dest)
{
    segsize_ = MSS;
    qmax_ = parent->qmax ();
}

std::string Connection::statename () const
{
    return "link " + std::to_string (srcaddr_) + "<state: " + state_name () + ">";
}

bool Connection::running () const noexcept
{
    return const_cast<Connection *> (this)->in_state (
        State (&Connection::run, "run"));
}

bool Connection::closed () const noexcept
{
    return const_cast<Connection *> (this)->in_state (
        State (&Connection::cl, "cl"));
}

SessionControl *Connection::session () const
{
    return parent_->session_control ();
}

void Connection::set_phase (std::uint32_t info)
{
    // Only the bottom two bits are the version code, whatever else the
    // extensible field carries.
    rphase_ = phase_of_version (info & 3);
    unsigned ours = static_cast<unsigned> (node () ? node ()->phase ()
                                                   : Phase::ph4);
    cphase_ = std::min (rphase_, ours);
}

void Connection::arm_timer (double seconds)
{
    if (node ()) node ()->timers ().start (this, seconds);
}

void Connection::send (const NspPacketBase &pkt)
{
    parent_->send_to (dest_, pkt.encode_packet ());
}

void Connection::send_ack ()
{
    AckData a;
    a.dstaddr = dstaddr_;
    a.srcaddr = srcaddr_;
    // Acknowledge everything up to but not including what we expect next.
    a.acknum = AckNum { next_expect_ - Seq (1), AckNum::ACKQ };
    send (a);
}

// -------------------------------------------------------------- opening

void Connection::start_outbound (Bytes payload)
{
    ConnInit ci_pkt;
    ci_pkt.dstaddr = 0;                  // not known until they answer
    ci_pkt.srcaddr = srcaddr_;
    ci_pkt.fcopt   = SVC_NONE;           // see the note in the header
    ci_pkt.info    = parent_->nsp_version ();
    ci_pkt.segsize = MSS;
    ci_pkt.payload = std::move (payload);

    set_state (DN_MY_STATE (Connection, ci));
    // The connect message is sequence number 0, so its acknowledgement is
    // what the data subchannel treats as acknowledging number 0.  It is
    // not subject to flow control: it is what asks for it.
    TxEntry e;
    e.seq   = Seq (0);
    e.frame = ci_pkt.encode_packet ();
    e.sent  = true;
    txq_.push_back (std::move (e));
    send (ci_pkt);
    timer_is_retransmit_ = false;
    arm_timer (conn_timeout_);
}

void Connection::start_inbound (const ConnInit &pkt)
{
    dstaddr_ = pkt.srcaddr;
    set_phase (pkt.info);
    segsize_ = std::min (pkt.segsize, MSS);
    // What the peer asked for governs what we may send it.
    flow_ = pkt.fcopt;

    if (cphase_ > 2) {
        // Phase III and later acknowledge the connect message at once, so
        // the far end can stop retransmitting while the application
        // decides what to do.
        AckConn ca;
        ca.dstaddr = dstaddr_;
        send (ca);
    }
    set_state (DN_MY_STATE (Connection, cr));
    timer_is_retransmit_ = false;
    arm_timer (conn_timeout_);

    if (session ())
        session ()->connect_received (
            *this, ByteView (pkt.payload.data (), pkt.payload.size ()));
}

// ----------------------------------------------- the session control API

void Connection::accept (Bytes data, std::uint8_t fcopt)
{
    if (!in_state (DN_MY_STATE (Connection, cr))) {
        DN_DEBUG ("accept on link {} in state {}", srcaddr_, state_name ());
        return;
    }
    ConnConf cc_pkt;
    cc_pkt.dstaddr  = dstaddr_;
    cc_pkt.srcaddr  = srcaddr_;
    cc_pkt.fcopt    = fcopt;
    cc_pkt.info     = parent_->nsp_version ();
    cc_pkt.segsize  = MSS;
    cc_pkt.data_ctl = std::move (data);
    send (cc_pkt);

    set_state (DN_MY_STATE (Connection, cc));
    arm_timer (conn_timeout_);
    timer_is_retransmit_ = false;
}

void Connection::reject (unsigned reason, Bytes data)
{
    DiscInit d;
    d.dstaddr  = dstaddr_;
    d.srcaddr  = srcaddr_;
    d.reason   = static_cast<std::uint16_t> (reason);
    d.data_ctl = std::move (data);
    send (d);
    set_state (close (reason, {}, false));
}

void Connection::disconnect (unsigned reason, Bytes data)
{
    if (closed ()) return;
    DiscInit d;
    d.dstaddr  = dstaddr_;
    d.srcaddr  = srcaddr_;
    d.reason   = static_cast<std::uint16_t> (reason);
    d.data_ctl = std::move (data);
    send (d);
    set_state (DN_MY_STATE (Connection, di));
    timer_is_retransmit_ = false;
    arm_timer (conn_timeout_);
}

void Connection::send_data (Bytes data)
{
    if (!running ()) {
        DN_DEBUG ("send on link {} which is in state {}", srcaddr_,
                  state_name ());
        return;
    }
    send_segments (data);
}

void Connection::send_segments (const Bytes &data)
{
    // Split the message into segments the far end said it would accept,
    // marking the first and last.  A zero length message is still one
    // segment: it is a message, and the far end must see it as one.
    std::size_t limit = segsize_ ? segsize_ : MSS;
    std::size_t off = 0;
    bool first = true;
    do {
        std::size_t n = std::min (limit, data.size () - off);
        DataSeg seg;
        seg.dstaddr = dstaddr_;
        seg.srcaddr = srcaddr_;
        seg.set_segnum (next_send_);
        seg.payload.assign (data.begin () + static_cast<long> (off),
                            data.begin () + static_cast<long> (off + n));
        bool last = (off + n >= data.size ());
        seg.msgflag = static_cast<std::uint8_t> (
            DataSeg::flag | (first ? 0x20 : 0) | (last ? 0x40 : 0));
        // Piggyback what we have received on the outgoing segment.
        seg.acknum = AckNum { next_expect_ - Seq (1), AckNum::ACKQ };

        TxEntry e;
        e.seq     = next_send_;
        e.segnum  = next_segnum_++;
        e.msgnum  = next_msgnum_;
        e.frame   = seg.encode_packet ();
        e.is_data = true;
        txq_.push_back (std::move (e));

        if (last) ++next_msgnum_;
        ++next_send_;
        off += n;
        first = false;
    } while (off < data.size ());

    send_blocked ();
}

std::size_t Connection::in_flight () const noexcept
{
    std::size_t n = 0;
    for (const TxEntry &e : txq_) if (e.sent) ++n;
    return n;
}

bool Connection::flow_ok (const TxEntry &e) const
{
    // Control packets on this subchannel -- the connect and disconnect
    // messages -- are not flow controlled.
    if (!e.is_data) return true;
    if (!xon_) return false;

    // The window: at most qmax segments outstanding, measured from the
    // oldest unacknowledged one.
    unsigned maxq = txq_.empty () ? qmax_ - 1
                                  : txq_.front ().segnum + qmax_ - 1;
    if (e.segnum > maxq) return false;

    switch (flow_) {
    case SVC_SEG: return e.segnum <= max_seg_;
    case SVC_MSG: return e.msgnum <= max_msg_;
    default:      return true;          // SVC_NONE
    }
}

void Connection::send_blocked ()
{
    bool sent_any = false;
    for (TxEntry &e : txq_) {
        if (e.sent) continue;
        if (!flow_ok (e)) break;        // and everything after it waits too
        parent_->send_to (dest_, e.frame);
        e.sent = true;
        sent_any = true;
    }
    if (sent_any) {
        retries_ = 0;
        timer_is_retransmit_ = true;
        arm_timer (retransmit_time_);
    }
}

// ---------------------------------------------------- receiving packets

void Connection::process_ack (Seq num)
{
    // Everything up to and including num is acknowledged.  Only entries
    // that were actually sent can be: an unsent one further along the
    // queue is not covered by an acknowledgement.
    while (!txq_.empty () && txq_.front ().sent && !(num < txq_.front ().seq)) {
        max_acked_seg_ = txq_.front ().segnum;
        txq_.pop_front ();
    }
    highest_acked_ = num;
    if (txq_.empty ()) {
        retries_ = 0;
        if (timer_is_retransmit_ && node ()) node ()->timers ().stop (this);
    }
    // Room in the window may have appeared.
    send_blocked ();
}

void Connection::route_ack (const std::optional<AckNum> &a, bool on_data)
{
    if (!a) return;
    // A cross acknowledgement refers to the subchannel the packet did not
    // arrive on.
    bool for_data = a->is_cross () ? !on_data : on_data;
    if (for_data) process_ack (a->num);
    else          process_int_ack (a->num);
}

void Connection::process_int_ack (Seq num)
{
    while (!int_txq_.empty () && int_txq_.front ().sent
           && !(num < int_txq_.front ().seq))
        int_txq_.pop_front ();
}

bool Connection::can_interrupt () const noexcept
{
    return int_max_msg_ >= int_next_msg_;
}

bool Connection::send_interrupt (Bytes data)
{
    if (!running ()) {
        DN_DEBUG ("interrupt on link {} which is in state {}", srcaddr_,
                  state_name ());
        return false;
    }
    if (data.size () > MAX_INTERRUPT) {
        DN_DEBUG ("interrupt on link {} is {} bytes, limit is {}", srcaddr_,
                  data.size (), MAX_INTERRUPT);
        return false;
    }
    if (!can_interrupt ()) {
        // The far end has not released another.  This is the normal way
        // to be told to wait, not an error.
        DN_TRACE ("link {} has no interrupt credit", srcaddr_);
        return false;
    }

    IntMsg m;
    m.dstaddr = dstaddr_;
    m.srcaddr = srcaddr_;
    m.segnum  = int_next_send_;
    m.payload = std::move (data);
    // Acknowledge data we have received, on the other subchannel.
    m.acknum2 = AckNum { next_expect_ - Seq (1), AckNum::XACK };

    TxEntry e;
    e.seq    = int_next_send_;
    e.msgnum = int_next_msg_;
    e.frame  = m.encode_packet ();
    e.sent   = true;
    int_txq_.push_back (std::move (e));
    parent_->send_to (dest_, int_txq_.back ().frame);

    ++int_next_send_;
    ++int_next_msg_;
    return true;
}

void Connection::handle_interrupt (const IntMsg &msg)
{
    Seq got = msg.segnum;
    if (got < int_next_expect_) {
        // Seen it already; acknowledge again in case that was what got
        // lost.
        AckOther a;
        a.dstaddr = dstaddr_;
        a.srcaddr = srcaddr_;
        a.acknum = AckNum { int_next_expect_ - Seq (1), AckNum::ACKQ };
        send (a);
        return;
    }
    // Interrupts are not reordered or held: there is at most one in
    // flight in each direction under normal flow control, and pydecnet
    // does not police inbound interrupt credit either.
    int_next_expect_ = got + Seq (1);

    AckOther a;
    a.dstaddr = dstaddr_;
    a.srcaddr = srcaddr_;
    a.acknum = AckNum { got, AckNum::ACKQ };
    send (a);

    if (session ())
        session ()->interrupt_received (
            *this, ByteView (msg.payload.data (), msg.payload.size ()));
}

void Connection::handle_link_service (const LinkSvcMsg &ls)
{
    if (!ls.valid ()) {
        DN_DEBUG ("link {} reserved link service value", srcaddr_);
        return;
    }
    // A link service message travels on the other subchannel.
    route_ack (ls.acknum, false);
    route_ack (ls.acknum2, false);

    if (ls.fcval_int == LinkSvcMsg::INT_REQ) {
        // Credit for interrupts.  Only a request for more is meaningful;
        // the count cannot be taken back.
        std::int64_t d = ls.fcval;
        if (d < 0) {
            DN_DEBUG ("link {} negative interrupt credit {}", srcaddr_, d);
            return;
        }
        int_max_msg_ = static_cast<unsigned> (int_max_msg_ + d);
        DN_TRACE ("link {} interrupt credit now {}", srcaddr_,
                  interrupt_credit ());
        return;
    }
    if (ls.fcval_int != LinkSvcMsg::DATA_REQ) return;

    std::int64_t delta = ls.fcval;
    if (flow_ == SVC_MSG) {
        // Credit is counted in messages.  The far end may not open the
        // window more than 127 beyond what it has acknowledged.
        if (delta >= 0
            && static_cast<std::int64_t> (max_msg_) + delta
                   < static_cast<std::int64_t> (max_acked_seg_) + 128)
            max_msg_ = static_cast<unsigned> (max_msg_ + delta);
        else {
            DN_DEBUG ("link {} invalid message mode credit {}", srcaddr_,
                      delta);
            return;
        }
    } else if (flow_ == SVC_SEG) {
        std::int64_t want = static_cast<std::int64_t> (max_seg_) + delta;
        if (want >= static_cast<std::int64_t> (max_acked_seg_)
            && want < static_cast<std::int64_t> (max_acked_seg_) + 128)
            max_seg_ = static_cast<unsigned> (want);
        else {
            DN_DEBUG ("link {} invalid segment mode credit {}", srcaddr_,
                      delta);
            return;
        }
    }
    // A modifier of XON or XOFF turns transmission on or off regardless of
    // credit; NO_CHANGE leaves it as it was.
    if (ls.fcmod != LinkSvcMsg::NO_CHANGE)
        xon_ = (ls.fcmod == LinkSvcMsg::XON);

    DN_TRACE ("link {} credit now seg {} msg {}, xon {}", srcaddr_, max_seg_,
              max_msg_, xon_);
    send_blocked ();
}

void Connection::accept_segment (const DataSeg &seg)
{
    if (seg.bom ()) {
        assembly_.clear ();
        assembling_ = true;
    }
    if (!assembling_) {
        // A continuation with no beginning: nothing sensible to do with it.
        DN_DEBUG ("link {} segment with no start of message", srcaddr_);
        return;
    }
    assembly_.insert (assembly_.end (), seg.payload.begin (),
                      seg.payload.end ());
    if (seg.eom ()) {
        assembling_ = false;
        if (session ())
            session ()->data_received (
                *this, ByteView (assembly_.data (), assembly_.size ()));
        assembly_.clear ();
    }
}

void Connection::handle_data (const DataSeg &seg)
{
    route_ack (seg.acknum, true);
    route_ack (seg.acknum2, true);

    Seq got = seg.segnum ();
    if (got < next_expect_) {
        // Already had it.  Say so again, in case the acknowledgement was
        // what went missing.
        send_ack ();
        return;
    }
    if (got != next_expect_) {
        // Ahead of its turn.  Keep it: the sender need only retransmit
        // what is actually missing, not everything after it.
        if (ooo_.size () < qmax_) {
            DN_TRACE ("link {} holding out of order segment {}, expecting {}",
                      srcaddr_, got.value (), next_expect_.value ());
            ooo_.emplace (static_cast<std::uint16_t> (got.value ()), seg);
        }
        send_ack ();
        return;
    }

    // In sequence.  Take this one, then anything held that now follows on.
    accept_segment (seg);
    ++next_expect_;
    for (;;) {
        auto it = ooo_.find (static_cast<std::uint16_t> (next_expect_.value ()));
        if (it == ooo_.end ()) break;
        DataSeg held = it->second;
        ooo_.erase (it);
        accept_segment (held);
        ++next_expect_;
    }
    send_ack ();
}

void Connection::retransmit ()
{
    std::size_t n = in_flight ();
    if (n == 0) {
        // Nothing outstanding.  If anything is queued it is waiting on
        // flow control, not on the wire, so there is nothing to resend.
        return;
    }
    if (++retries_ > MAX_RETRIES) {
        DN_DEBUG ("link {} giving up after {} retransmissions", srcaddr_,
                  MAX_RETRIES);
        set_state (close (DISC_NO_LINK, {}, true));
        return;
    }
    DN_TRACE ("link {} retransmitting {} packet(s)", srcaddr_, n);
    for (const TxEntry &e : txq_)
        if (e.sent) parent_->send_to (dest_, e.frame);
    timer_is_retransmit_ = true;
    arm_timer (retransmit_time_);
}

void Connection::timeout ()
{
    if (timer_is_retransmit_) {
        retransmit ();
        return;
    }
    Timeout t (nullptr, nullptr, 0);
    StateMachine<Connection>::dispatch (t);
}

// ------------------------------------------------------------- closing

Connection::State Connection::close (unsigned reason, ByteView data,
                                     bool tell_session)
{
    if (node ()) node ()->timers ().stop (this);
    txq_.clear ();
    int_txq_.clear ();
    ooo_.clear ();
    if (tell_session && session ())
        session ()->disconnected (*this, reason, data);
    // NSP forgets the link here; whoever else holds a reference sees a
    // connection in the closed state.
    parent_->close_connection (this);
    return DN_MY_STATE (Connection, cl);
}

// -------------------------------------------------------------- states

void Connection::receive (const NspPacketBase &pkt)
{
    // The packet is delivered to the state machine as a work item so that
    // every state sees inputs the same way.
    struct PacketWork : Work {
        const NspPacketBase *pkt;
        explicit PacketWork (const NspPacketBase *p) noexcept
            : Work (nullptr), pkt (p) {}
        const char *kind () const noexcept override { return "NspPacket"; }
    };
    PacketWork w (&pkt);
    received_ = &pkt;
    StateMachine<Connection>::dispatch (w);
    received_ = nullptr;
}

Connection::State Connection::s0 (Work &)
{
    // A connection is put straight into ci or cr by its constructor.
    return nullptr;
}

Connection::State Connection::ci (Work &w)
{
    if (!received_) {
        if (dynamic_cast<Timeout *> (&w)) {
            // Nobody answered.  Tell session control and give up; the
            // protocol has no way to withdraw a connect initiate.
            return close (DISC_OBJ_FAIL, {}, true);
        }
        return nullptr;
    }
    if (dynamic_cast<const AckConn *> (received_)) {
        // The far end has the connect message; stop retransmitting it and
        // wait for the application's answer.
        process_ack (Seq (0));
        arm_timer (conn_timeout_);
        timer_is_retransmit_ = false;
        return DN_MY_STATE (Connection, cd);
    }
    return cd (w);
}

Connection::State Connection::cd (Work &w)
{
    if (!received_) {
        if (dynamic_cast<Timeout *> (&w))
            return close (DISC_OBJ_FAIL, {}, true);
        return nullptr;
    }
    if (auto *cc_pkt = dynamic_cast<const ConnConf *> (received_)) {
        // Accepted.
        dstaddr_ = cc_pkt->srcaddr;
        set_phase (cc_pkt->info);
        segsize_ = std::min (cc_pkt->segsize, MSS);
        flow_ = cc_pkt->fcopt;
        process_ack (Seq (0));
        if (cphase_ > 2) send_ack ();
        arm_timer (inact_time_);
        timer_is_retransmit_ = false;
        if (session ())
            session ()->connect_confirmed (
                *this, ByteView (cc_pkt->data_ctl.data (),
                                 cc_pkt->data_ctl.size ()));
        return DN_MY_STATE (Connection, run);
    }
    if (auto *d = dynamic_cast<const DiscInit *> (received_)) {
        // Rejected.  Acknowledge it so the far end can let go.
        dstaddr_ = d->srcaddr;
        DiscConf dc;
        dc.dstaddr = dstaddr_;
        dc.srcaddr = srcaddr_;
        dc.reason  = DiscConf::DISC_COMPLETE;
        send (dc);
        if (session ())
            session ()->connect_rejected (
                *this, d->reason,
                ByteView (d->data_ctl.data (), d->data_ctl.size ()));
        return close (d->reason, {}, false);
    }
    if (auto *dc = dynamic_cast<const DiscConf *> (received_)) {
        // No resources, or a Phase II style reject.
        if (session ())
            session ()->connect_rejected (*this, dc->reason, {});
        return close (dc->reason, {}, false);
    }
    return nullptr;
}

Connection::State Connection::cr (Work &w)
{
    if (!received_) {
        if (dynamic_cast<Timeout *> (&w)) {
            // The application took too long.  Reject on its behalf, and
            // tell it the link is gone.
            reject (DISC_OBJ_FAIL);
            return nullptr;             // reject() already changed state
        }
        return nullptr;
    }
    if (dynamic_cast<const ConnInit *> (received_)) {
        // A retransmitted connect initiate: acknowledge it again.
        if (cphase_ > 2) {
            AckConn ca;
            ca.dstaddr = dstaddr_;
            send (ca);
        }
    }
    return nullptr;
}

Connection::State Connection::cc (Work &w)
{
    if (received_) {
        // Any data or acknowledgement confirms they have our accept.
        if (dynamic_cast<const DataSeg *> (received_)
            || dynamic_cast<const AckData *> (received_)) {
            process_ack (Seq (0));
            set_state (DN_MY_STATE (Connection, run));
            return run (w);
        }
        if (auto *d = dynamic_cast<const DiscInit *> (received_)) {
            DiscConf dc;
            dc.dstaddr = dstaddr_;
            dc.srcaddr = srcaddr_;
            dc.reason  = DiscConf::DISC_COMPLETE;
            send (dc);
            return close (d->reason,
                          ByteView (d->data_ctl.data (), d->data_ctl.size ()),
                          true);
        }
        return nullptr;
    }
    if (dynamic_cast<Timeout *> (&w))
        return close (DISC_OBJ_FAIL, {}, true);
    return nullptr;
}

Connection::State Connection::run (Work &w)
{
    if (received_) {
        if (cphase_ > 2 && !timer_is_retransmit_) arm_timer (inact_time_);

        if (auto *seg = dynamic_cast<const DataSeg *> (received_)) {
            handle_data (*seg);
            return nullptr;
        }
        if (auto *ack = dynamic_cast<const AckData *> (received_)) {
            route_ack (ack->acknum, true);
            route_ack (ack->acknum2, true);
            return nullptr;
        }
        if (auto *ack = dynamic_cast<const AckOther *> (received_)) {
            route_ack (ack->acknum, false);
            route_ack (ack->acknum2, false);
            return nullptr;
        }
        if (auto *ls = dynamic_cast<const LinkSvcMsg *> (received_)) {
            handle_link_service (*ls);
            return nullptr;
        }
        if (auto *im = dynamic_cast<const IntMsg *> (received_)) {
            handle_interrupt (*im);
            return nullptr;
        }
        if (auto *d = dynamic_cast<const DiscInit *> (received_)) {
            // The far end is closing.  Confirm, then tell the application.
            DiscConf dc;
            dc.dstaddr = dstaddr_;
            dc.srcaddr = srcaddr_;
            dc.reason  = DiscConf::DISC_COMPLETE;
            send (dc);
            return close (d->reason,
                          ByteView (d->data_ctl.data (), d->data_ctl.size ()),
                          true);
        }
        if (dynamic_cast<const DiscConf *> (received_))
            return close (DISC_NORMAL, {}, true);
        return nullptr;
    }
    if (dynamic_cast<Timeout *> (&w)) {
        // The inactivity timer.  Nothing has been heard for a long time,
        // so prove the link is still there.
        send_ack ();
        arm_timer (inact_time_);
    }
    return nullptr;
}

Connection::State Connection::di (Work &w)
{
    if (received_) {
        if (dynamic_cast<const DiscConf *> (received_)
            || dynamic_cast<const DiscInit *> (received_))
            return close (DISC_NORMAL, {}, true);
        return nullptr;
    }
    if (dynamic_cast<Timeout *> (&w)) {
        // No confirmation.  Let go anyway: the link is not usable.
        return close (DISC_NORMAL, {}, true);
    }
    return nullptr;
}

Connection::State Connection::cl (Work &) { return nullptr; }

// =================================================================== NSP

NSP::NSP (Element *parent, const Config &config)
    : Element (parent)
{
    DN_DEBUG ("initializing NSP");
    maxconns_ = config.nsp ().max_connections;
    qmax_     = config.nsp ().qmax;
    // PORT: the NSP timers are still fixed; pydecnet takes them from the
    // same configuration line.
    unsigned ph = static_cast<unsigned> (node () ? node ()->phase ()
                                                 : Phase::ph4);
    nspver_ = (ph == 2) ? VER_PH2 : (ph == 3) ? VER_PH3 : VER_PH4;
    init_ids ();
}

NSP::~NSP () = default;

void NSP::start ()
{
    DN_DEBUG ("starting NSP");
    routing_ = node () ? node ()->routing () : nullptr;
}

void NSP::stop ()
{
    DN_DEBUG ("stopping NSP");
    by_remote_.clear ();
    by_addr_.clear ();
}

void NSP::init_ids ()
{
    // The addresses are drawn so that the low order bits are unique and
    // non-zero, with a random high order part; a returned address goes to
    // the back of the queue with its high part bumped, so it is not reused
    // until every other address has been.
    std::mt19937 gen { std::random_device {} () };
    unsigned c = maxconns_ + 1;
    std::vector<std::uint16_t> ids;
    ids.reserve (maxconns_);
    std::uniform_int_distribution<unsigned> high (0, 65535 / c);
    for (unsigned i = 1; i <= maxconns_; ++i)
        ids.push_back (static_cast<std::uint16_t> (i + high (gen) * c));
    std::shuffle (ids.begin (), ids.end (), gen);
    free_ids_.assign (ids.begin (), ids.end ());
}

bool NSP::get_id (std::uint16_t &out)
{
    if (free_ids_.empty ()) return false;
    out = free_ids_.front ();
    free_ids_.pop_front ();
    return true;
}

void NSP::return_id (std::uint16_t id)
{
    free_ids_.push_back (
        static_cast<std::uint16_t> ((id + maxconns_ + 1) & 0xffff));
}

Connection *NSP::find (std::uint16_t srcaddr) const
{
    auto it = by_addr_.find (srcaddr);
    return it == by_addr_.end () ? nullptr : it->second.get ();
}

void NSP::send_to (Nodeid dest, const Bytes &frame)
{
    if (!routing_) {
        DN_DEBUG ("no routing layer; NSP packet to {} dropped", dest.str ());
        return;
    }
    routing_->send_nsp (frame, dest);
}

Connection *NSP::connect (Nodeid dest, Bytes payload)
{
    std::uint16_t addr = 0;
    if (!get_id (addr)) {
        DN_ERROR ("connection limit reached");
        return nullptr;
    }
    auto conn = std::make_unique<Connection> (this, addr, dest);
    Connection *raw = conn.get ();
    by_addr_[addr] = std::move (conn);
    raw->start_outbound (std::move (payload));
    return raw;
}

void NSP::close_connection (Connection *c)
{
    std::uint16_t addr = c->srcaddr ();
    by_remote_.erase ({ static_cast<std::uint16_t> (c->dest ().value ()),
                        c->dstaddr () });
    return_id (addr);
    // The object stays alive: a caller may still hold a pointer, and it
    // reports itself closed.  Only the tables forget it.
    auto it = by_addr_.find (addr);
    if (it != by_addr_.end ()) {
        closed_.push_back (std::move (it->second));
        by_addr_.erase (it);
    }
}

void NSP::deliver (Nodeid src, ByteView payload)
{
    auto pkt = NspPacketBase::parse_frame (payload);
    if (!pkt) {
        DN_DEBUG ("ill formatted NSP packet from {}", src.str ());
        if (Node *n = node ()) {
            // Event 3.0, invalid message.  It carries the message itself
            // and the node it came from, which is what an operator needs
            // to work out whose implementation is at fault.
            events::Event e { { 3, 0 }, nice::Entity::make_none () };
            e.param (0, nice::Value::hi (Bytes (payload.begin (),
                                                payload.end ())));
            e.param (2, events::node_value (n->nicenode (src)));
            n->logevent (e);
        }
        return;
    }

    if (auto *ci_pkt = dynamic_cast<ConnInit *> (pkt.get ())) {
        // A connect initiate: a retransmission of one we have, or a new
        // inbound connection.
        auto key = std::make_pair (static_cast<std::uint16_t> (src.value ()),
                                   ci_pkt->srcaddr);
        auto it = by_remote_.find (key);
        if (it != by_remote_.end ()) {
            it->second->receive (*pkt);
            return;
        }
        std::uint16_t addr = 0;
        if (!get_id (addr)) {
            // Nothing left to allocate: say so rather than ignoring it.
            DiscConf dc;
            dc.dstaddr = ci_pkt->srcaddr;
            dc.srcaddr = 0;
            dc.reason  = DiscConf::NO_RESOURCES;
            send_to (src, dc.encode_packet ());
            return;
        }
        auto conn = std::make_unique<Connection> (this, addr, src);
        Connection *raw = conn.get ();
        by_addr_[addr] = std::move (conn);
        by_remote_[key] = raw;
        raw->start_inbound (*ci_pkt);
        return;
    }

    // Everything else is addressed to one of our link addresses.
    Connection *conn = find (pkt->link_address ());
    std::uint16_t dst = pkt->link_address ();
    if (!conn) {
        DN_TRACE ("NSP packet from {} for unknown link {}", src.str (), dst);
        return;
    }
    conn->receive (*pkt);
}

}   // namespace decnet::nsp
