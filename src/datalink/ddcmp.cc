// src/datalink/ddcmp.cc -- the DDCMP protocol engine.  Port of the _DDCMP
// class in ddcmp.py, without the transports: what carries the bytes is
// given to it as callbacks.

#include "decnet/datalink/ddcmp.h"

#include "decnet/common/logging.h"

namespace decnet::datalink::ddcmp {

namespace {

// Which counter a NAK reason belongs to.  PyDECnet's nak_map; R_OVER and
// R_FMT are deliberately unmapped there and here.
bool is_data_error (std::uint8_t reason)
{
    return reason == R_HCRC || reason == R_CRC || reason == R_REP;
}

}   // namespace

Protocol::Protocol (Hooks h, unsigned qmax)
    : hooks_ (std::move (h)), qmax_ (qmax ? qmax : 7)
{
    init_state ();
}

const char *Protocol::state_name () const noexcept
{
    switch (state_) {
    case State::halted:      return "halted";
    case State::istart:      return "starting";
    case State::astart:      return "start ack";
    case State::running:     return "running";
    case State::maintenance: return "maintenance";
    }
    return "?";
}

void Protocol::init_state ()
{
    // Names from the spec.  T and X are not represented: retransmission resends
    // unack_, and sockets do not report transmission complete.
    r_ = Seq (0);
    a_ = Seq (0);
    n_ = Seq (0);
    ackflag_ = false;
    for (auto &m : unack_) m.reset ();
    notsent_.clear ();
    if (hooks_.set_timer) hooks_.set_timer (0);
}

void Protocol::connected ()
{
    init_state ();
    acktmr_.reset ();
    stacktmr_.reset ();
    send_msg (make_start (), stacktmr_.next ());
    state_ = State::istart;
}

void Protocol::halt ()
{
    if (state_ == State::running && hooks_.down) hooks_.down ();
    init_state ();
    state_ = State::halted;
}

void Protocol::restart () { do_restart (); }

void Protocol::do_restart ()
{
    if (state_ == State::running && hooks_.down) hooks_.down ();
    if (state_ == State::istart) return;    // already restarting
    DN_TRACE ("restarting DDCMP");
    connected ();
}

void Protocol::send_msg (const Message &m, double timer)
{
    DN_TRACE ("DDCMP send {}", m.str ());
    if (hooks_.send) hooks_.send (m);
    if (hooks_.set_timer && timer > 0) hooks_.set_timer (timer);
}

void Protocol::enter_running (bool ack)
{
    DN_TRACE ("DDCMP running");
    state_ = State::running;
    if (hooks_.set_timer) hooks_.set_timer (0);
    if (hooks_.up) hooks_.up ();
    if (ack) send_msg (make_ack (r_), 0);
}

void Protocol::flush_ack ()
{
    // An acknowledgement only needs a message of its own if nothing else
    // carried it: a data message going the other way has a resp field.
    if (!ackflag_) return;
    ackflag_ = false;
    send_msg (make_ack (r_), 0);
}

// ---------------------------------------------------------------- input

void Protocol::receive (const Message &m)
{
    DN_TRACE ("DDCMP recv {} in {}", m.str (), state_name ());

    switch (state_) {

    case State::halted:
        return;

    case State::istart:
        // Startup state table, "Istarted": we have sent a Start.
        if (m.kind == MsgKind::start) {
            stacktmr_.reset ();
            send_msg (make_stack (), stacktmr_.next ());
            state_ = State::astart;
        } else if (m.kind == MsgKind::stack) {
            enter_running (true);
        } else if (m.kind == MsgKind::maintenance) {
            init_state ();
            state_ = State::maintenance;
            receive (m);
        }
        // Anything else is ignored rather than answered.  Answering would
        // turn a confused link into a message flood.
        return;

    case State::astart:
        // "Astarted": we have answered a Start with a Stack.
        if (m.kind == MsgKind::start) {
            send_msg (make_stack (), stacktmr_.next ());
        } else if (m.kind == MsgKind::stack) {
            enter_running (true);
        } else if (m.kind == MsgKind::maintenance) {
            init_state ();
            state_ = State::maintenance;
            receive (m);
        } else if ((m.kind == MsgKind::ack || m.kind == MsgKind::data)
                   && m.resp == Seq (0)) {
            // The far end has gone running and is talking to us.  Follow
            // it, then handle this message as running would have.
            enter_running (false);
            receive (m);
        }
        return;

    case State::maintenance:
        if (m.kind == MsgKind::maintenance) {
            DN_TRACE ("DDCMP maintenance message, {} bytes", m.payload.size ());
            // Nothing opens a maintenance port yet, so this is counted and
            // dropped.  PyDECnet does the same, with the same comment.
        } else if (m.kind == MsgKind::start) {
            do_restart ();
        }
        return;

    case State::running:
        break;
    }

    // Running.
    switch (m.kind) {
    case MsgKind::data: {
        // The resp field is acted on whatever else is true of the message.
        process_ack (m);
        Seq next = r_ + 1;
        if (!(m.num == next)) {
            // Out of sequence: ignored, not NAKed.  The far end will find
            // out from our ack, which still names the last one we did get.
            DN_TRACE ("DDCMP out of sequence, want {} got {}", next.value (),
                      m.num.value ());
            break;
        }
        r_ = next;
        ackflag_ = true;
        if (hooks_.deliver) hooks_.deliver (m.payload);
        break;
    }

    case MsgKind::ack:
        process_ack (m);
        break;

    case MsgKind::nak:
        // A NAK acknowledges everything before the error, then asks for
        // the rest again.
        (void) is_data_error (m.subtype);
        if (process_ack (m)) retransmit ();
        break;

    case MsgKind::rep:
        // "Have you got everything up to num?"  If so our ack answers it;
        // if not, a NAK tells the far end where we actually are.
        if (m.num == r_) ackflag_ = true;
        else send_msg (make_nak (r_, R_REP), 0);
        break;

    case MsgKind::maintenance:
        init_state ();
        if (hooks_.down) hooks_.down ();
        state_ = State::maintenance;
        receive (m);
        return;

    case MsgKind::start:
        do_restart ();
        return;

    case MsgKind::stack:
        ackflag_ = true;
        break;
    }

    flush_ack ();
}

void Protocol::receive_error (std::uint8_t reason, const Message *partial)
{
    if (state_ != State::running) return;
    // A header that decoded but whose payload did not still carries a
    // resp field, and that much is good information.
    if (partial && partial->sets_resp ()) process_ack (*partial);
    send_msg (make_nak (r_, reason), 0);
}

void Protocol::timeout ()
{
    switch (state_) {
    case State::istart:
        send_msg (make_start (), stacktmr_.next ());
        return;
    case State::astart:
        send_msg (make_stack (), stacktmr_.next ());
        return;
    case State::running:
        // On timeout, send REP rather than retransmitting.  The far end's reply
        // says what needs resending.
        send_msg (make_rep (n_), acktmr_.next ());
        return;
    default:
        return;
    }
}

// ------------------------------------------------------------- the window

bool Protocol::process_ack (const Message &m)
{
    unsigned count = a_.distance (m.resp);
    unsigned pend  = a_.distance (n_);
    if (count > pend) {
        // Reject acknowledgements outside the window.  Because sequence numbers
        // wrap, a stale ack would otherwise look like an ack for nearly 256
        // messages.
        DN_TRACE ("DDCMP stale ack, resp={} a={} n={}", m.resp.value (),
                  a_.value (), n_.value ());
        return false;
    }

    for (unsigned i = 0; i < count; ++i) {
        ++a_;
        unack_[a_.value ()].reset ();
    }

    if (!(a_ == n_)) {
        if (hooks_.set_timer) hooks_.set_timer (acktmr_.next ());
    } else {
        if (hooks_.set_timer) hooks_.set_timer (0);
        acktmr_.reset ();
    }

    // Send queued messages that now fit in the window.  After a NAK they are
    // sent after the retransmission, to keep order.
    send_queued (m.kind == MsgKind::nak);
    return true;
}

void Protocol::retransmit ()
{
    Seq t = a_;
    unsigned pend = a_.distance (n_);
    for (unsigned i = 0; i < pend; ++i) {
        ++t;
        if (unack_[t.value ()]) send_msg (*unack_[t.value ()], acktmr_.next ());
    }
}

void Protocol::send_queued (bool queue_only)
{
    while (cansend () && !notsent_.empty ()) {
        Bytes data = std::move (notsent_.front ());
        notsent_.pop_front ();

        ++n_;
        Message msg = make_data (n_, r_, std::move (data));
        unack_[n_.value ()] = msg;
        if (!queue_only) send_msg (msg, acktmr_.next ());
    }
}

void Protocol::send (Bytes payload)
{
    if (state_ != State::running) {
        // Not running: dropped.  Routing sees the circuit is down and
        // stops offering; a datalink does not buffer for a dead link.
        DN_TRACE ("DDCMP send while {}, discarded", state_name ());
        return;
    }
    notsent_.push_back (std::move (payload));
    send_queued (false);
}

void Protocol::send_maintenance (Bytes payload)
{
    send_msg (make_maintenance (std::move (payload)), 0);
}

}   // namespace decnet::datalink::ddcmp
