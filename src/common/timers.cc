#include "decnet/common/timers.h"

#include "decnet/common/logging.h"
#include "decnet/node.h"

#include <cmath>
#include <random>
#include <stdexcept>

namespace decnet {

// ---------------------------------------------------------------- Timeout

void Timeout::dispatch ()
{
    // Drop the item if the owner has started or stopped this timer since
    // the wheel decided it had expired.  See the comment in the header.
    if (timer_ && timer_->revcount () != revcount_) {
        DN_TRACE ("discarding stale timeout");
        return;
    }
    if (timer_) timer_->timeout ();
}

// ------------------------------------------------------------- TimerWheel

TimerWheel::TimerWheel (Node *node, std::chrono::milliseconds tick,
                        unsigned maxtime)
    : node_ (node), tick_ (tick),
      wheel_ (static_cast<std::size_t> (
                  (maxtime * 1000 + tick.count ()) / tick.count ()))
{
}

TimerWheel::~TimerWheel () { shutdown (); }

void TimerWheel::startup ()
{
    if (running_.exchange (true)) return;
    DN_DEBUG ("starting timer subsystem");
    thread_ = std::thread ([this] { run (); });
}

void TimerWheel::shutdown ()
{
    if (!running_.exchange (false)) return;
    if (thread_.joinable ()) thread_.join ();
    DN_DEBUG ("timer subsystem shut down");
}

void TimerWheel::start (Timer *t, std::chrono::milliseconds ms)
{
    std::size_t ticks = static_cast<std::size_t> (ms / tick_);
    if (ticks == 0) ticks = 1;                 // minimum timeout is one tick
    if (ticks >= wheel_.size ())
        throw std::overflow_error ("timeout " + std::to_string (ms.count ())
                                   + "ms exceeds the timer wheel span");
    std::lock_guard lock (mutex_);
    std::size_t slot = (pos_ + ticks) % wheel_.size ();
    t->unlink ();
    // Append, so timers for the same tick fire in start order.  NSP relies on
    // this.
    wheel_[slot].add_before (t);
}

void TimerWheel::start (Timer *t, double seconds)
{
    start (t, std::chrono::milliseconds (
               static_cast<long long> (seconds * 1000.0)));
}

void TimerWheel::jstart (Timer *t, double seconds)
{
    if (seconds > 2.0) {
        static thread_local std::mt19937 gen { std::random_device {} () };
        std::uniform_real_distribution<double> jitter (0.9, 1.0);
        seconds *= jitter (gen);
    }
    start (t, seconds);
}

void TimerWheel::stop (Timer *t)
{
    std::lock_guard lock (mutex_);
    t->unlink ();
}

void TimerWheel::tick ()
{
    Cque *head;
    {
        std::lock_guard lock (mutex_);
        pos_ = (pos_ + 1) % wheel_.size ();
        head = &wheel_[pos_];
    }
    // Unlink one at a time under the lock, so that a layer cancelling a
    // timer from another thread cannot corrupt the list we are walking.
    for (;;) {
        Timer   *item;
        unsigned revcount;
        {
            std::lock_guard lock (mutex_);
            if (!head->linked ()) break;
            item = static_cast<Timer *> (head->next ());
            item->unlink ();
            revcount = item->revcount ();
        }
        if (node_)
            node_->add_work (std::make_unique<Timeout> (item->timer_owner (),
                                                        item, revcount));
        else
            item->timeout ();       // standalone use, e.g. in unit tests
    }
}

void TimerWheel::run ()
{
    logging::set_thread_name ("timers");
    auto next = std::chrono::steady_clock::now ();
    auto maxdt = tick_ * 2;
    auto prev = std::chrono::steady_clock::now ();
    while (running_.load ()) {
        next += tick_;
        std::this_thread::sleep_until (next);
        auto now = std::chrono::steady_clock::now ();
        auto dt = std::chrono::duration_cast<std::chrono::milliseconds> (now - prev);
        if (dt > maxdt)
            DN_TRACE ("timer thread excessive tick latency {}ms", dt.count ());
        prev = now;
        tick ();
    }
}

}   // namespace decnet
