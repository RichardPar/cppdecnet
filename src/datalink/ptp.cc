#include "decnet/datalink/ptp.h"

#include "decnet/common/logging.h"
#include "decnet/node.h"

#include <stdexcept>

namespace decnet::datalink {

// --------------------------------------------------------------- PtpPort

PtpPort::PtpPort (PtpDatalink *dl, Element *owner) noexcept
    : Port (dl, owner)
{
}

void PtpPort::send (Bytes msg)
{
    static_cast<PtpDatalink *> (datalink_)->send (std::move (msg));
}

bool PtpPort::start_works () const noexcept { return true; }

// ----------------------------------------------------------- PtpDatalink

PtpDatalink::PtpDatalink (Element *owner, std::string name)
    : Datalink (owner, std::move (name))
{
}

PtpDatalink::~PtpDatalink ()
{
    stop_thread (true);
}

Port *PtpDatalink::create_port (Element *owner)
{
    if (port_)
        throw InternalError ("creating a second port on point to point "
                             "datalink " + name_);
    owned_port_ = std::make_unique<PtpPort> (this, owner);
    port_ = owned_port_.get ();
    return port_;
}

void PtpDatalink::reconnect (bool now)
{
    if (node ()) node ()->add_work (std::make_unique<Reconnect> (this, now));
}

std::string PtpDatalink::statename () const
{
    return name_ + "<state: " + state_name () + ">";
}

void PtpDatalink::report_up ()
{
    if (is_up_) return;
    is_up_ = true;
    if (port_ && node ()) {
        DN_TRACE ("reporting UP to owner of {}", name_);
        node ()->add_work (std::make_unique<DlStatus> (port_->owner (),
                                                       DlStatus::Status::up));
    }
}

void PtpDatalink::report_down ()
{
    if (!is_up_) return;
    is_up_ = false;
    if (port_ && node ()) {
        DN_TRACE ("reporting DOWN to owner of {}", name_);
        node ()->add_work (std::make_unique<DlStatus> (port_->owner (),
                                                       DlStatus::Status::down));
    }
}

// --------------------------------------------------------- receive thread

void PtpDatalink::start_thread ()
{
    if (thread_.joinable ()) return;
    stopnow_.store (false);
    thread_running_.store (true);
    thread_ = std::thread ([this] { run (); });
}

void PtpDatalink::stop_thread (bool wait)
{
    stopnow_.store (true);
    if (wait && thread_.joinable ()) thread_.join ();
}

void PtpDatalink::run ()
{
    logging::set_thread_name ((node () ? node ()->name () + "." : "") + name_);
    DN_TRACE ("receive thread started for {}", name_);
    try {
        bool conn = check_connection ();
        if (!stopnow_.load ()) {
            if (conn) {
                // We have a good connection, so the retry holdoff starts
                // over: the next failure should be retried promptly.
                DN_TRACE ("connected on {}", name_);
                restart_timer_.reset ();
                if (node ())
                    node ()->add_work (std::make_unique<Connected> (this));
                receive_loop ();
            } else {
                DN_TRACE ("connect failed for {}", name_);
            }
        }
    } catch (const std::exception &e) {
        DN_TRACE ("exception in receive thread for {}: {}", name_, e.what ());
    }
    thread_running_.store (false);
    if (node ()) node ()->add_work (std::make_unique<ThreadExit> (this));
}

Bytes PtpDatalink::recvall (std::size_t n)
{
    Bytes ret;
    ret.reserve (n);
    while (ret.size () < n) {
        PollResult p = poll_socket (socket_.fd (), true, false, poll_timeout_ms);
        if (stopnow_.load ())
            throw std::runtime_error ("stop requested");
        if (p.error)
            throw std::runtime_error ("socket error");
        if (p.timeout) continue;
        if (!p.readable) continue;

        std::size_t want = n - ret.size ();
        std::uint8_t buf[4096];
        if (want > sizeof buf) want = sizeof buf;
        ssize_t got = ::recv (socket_.fd (), buf, want, 0);
        if (got <= 0)
            throw std::runtime_error ("connection closed");
        ret.insert (ret.end (), buf, buf + got);
    }
    return ret;
}

// ---------------------------------------------------------- state machine

PtpDatalink::State PtpDatalink::handle_stop ()
{
    // Ask the thread to finish, but do not wait for it here: we are on the
    // node thread, and the thread's exit arrives as a work item.
    stop_thread (false);
    report_down ();
    if (thread_.joinable ())
        return DN_MY_STATE (PtpDatalink, shutdown_state);
    return DN_MY_STATE (PtpDatalink, s0);
}

PtpDatalink::State PtpDatalink::handle_reconnect (const Reconnect &r)
{
    DN_TRACE ("in handle_reconnect, now = {}", r.now ());
    handle_stop ();
    restart_now_ = r.now ();
    set_state (DN_MY_STATE (PtpDatalink, reconnecting));
    if (thread_.joinable ())
        return DN_MY_STATE (PtpDatalink, reconnecting);
    // No thread to wait for, so move straight on by handing the item to the
    // reconnecting state, as PtpDatalink.handle_reconnect does.
    Reconnect again (this, r.now ());
    return reconnecting (again);
}

bool PtpDatalink::validate (Work &w)
{
    // Received is the common case; dispose of it first.
    if (dynamic_cast<Received *> (&w)) return true;

    if (dynamic_cast<Stop *> (&w)) {
        set_state (handle_stop ());
        return false;
    }
    if (auto *r = dynamic_cast<Reconnect *> (&w)) {
        set_state (handle_reconnect (*r));
        return false;
    }
    if (dynamic_cast<Restart *> (&w)) {
        // "Protocol restart", which the routing layer asks for whenever it
        // gives up on a neighbour -- a listen timeout above all.  A
        // datalink with a protocol of its own restarts that and keeps the
        // connection, which is what Ddcmp overrides this to do.  For one
        // with no protocol, Multinet being the case here, the connection
        // is the only thing there is to start over, and with no holdoff:
        // the layer above has already waited out a timeout.  This is what
        // _Multinet.validate does in the Python.
        //
        // Handling it here rather than only in Multinet is deliberate.
        // Dropping this item silently -- which is what used to happen --
        // leaves the routing circuit waiting in ds for a DlStatus UP that
        // the datalink has no reason to send, and the circuit never comes
        // back at all.  See BUGS.md.
        Reconnect again (this, true);
        set_state (handle_reconnect (again));
        return false;
    }
    if (dynamic_cast<ThreadExit *> (&w)) {
        if (!in_state (DN_MY_STATE (PtpDatalink, shutdown_state)))
            set_state (DN_MY_STATE (PtpDatalink, reconnecting));
        if (thread_.joinable ()) thread_.join ();
        report_down ();
        return true;
    }
    return true;
}

PtpDatalink::State PtpDatalink::s0 (Work &w)
{
    // Halted.  Everything is ignored until a Start item sets things going.
    if (dynamic_cast<Start *> (&w)) {
        connect ();
        start_thread ();
        return DN_MY_STATE (PtpDatalink, connecting);
    }
    return nullptr;
}

PtpDatalink::State PtpDatalink::connecting (Work &w)
{
    if (dynamic_cast<Connected *> (&w))
        return connected ();
    if (dynamic_cast<Timeout *> (&w)) {
        // Give up on this attempt and start over.  Immediately: we have
        // already waited once, and waiting twice for one retry is just
        // dead time.
        reconnect (true);
        return nullptr;
    }
    return nullptr;
}

PtpDatalink::State PtpDatalink::reconnecting (Work &w)
{
    if (dynamic_cast<Timeout *> (&w)) {
        if (node ()) node ()->add_work (std::make_unique<Start> (this));
        return DN_MY_STATE (PtpDatalink, s0);
    }
    if (dynamic_cast<ThreadExit *> (&w) || dynamic_cast<Reconnect *> (&w)) {
        disconnect ();
        if (restart_now_) {
            // Skip the holdoff this once, but hold off next time.
            restart_now_ = false;
            if (node ()) node ()->add_work (std::make_unique<Start> (this));
            return DN_MY_STATE (PtpDatalink, s0);
        }
        if (node ()) node ()->timers ().jstart (this, restart_timer_.next ());
        return nullptr;
    }
    return nullptr;
}

PtpDatalink::State PtpDatalink::shutdown_state (Work &w)
{
    if (dynamic_cast<ThreadExit *> (&w)) {
        disconnect ();
        report_down ();
        return DN_MY_STATE (PtpDatalink, s0);
    }
    return nullptr;
}

}   // namespace decnet::datalink
