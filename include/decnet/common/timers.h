// decnet/common/timers.h -- the timer wheel.
//
// Direct port of timers.py: a Varghese/Lauck timer wheel, one per node,
// run by a helper thread that ticks every JIFFY and posts a Timeout work
// item for each expired timer.  Timers link themselves into a circular
// queue, so start, restart and cancel are O(1) with no allocation.  That
// is why it is ported as is rather than replaced with a priority queue.

#ifndef DECNET_COMMON_TIMERS_H
#define DECNET_COMMON_TIMERS_H

#include "decnet/common/element.h"
#include "decnet/common/work.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace decnet {

class Node;

// The tick interval, matching common.JIFFY.
inline constexpr std::chrono::milliseconds JIFFY { 100 };

// Base for anything that can be linked into a circular queue.  Port of
// timers.Cque.  An instance doubles as a list head.
class Cque {
public:
    Cque () noexcept { reset (); }
    ~Cque () { unlink (); }

    Cque (const Cque &) = delete;
    Cque &operator= (const Cque &) = delete;

    void reset () noexcept { next_ = prev_ = this; }

    // Bumped by every link and unlink.  The wheel samples it when it
    // notices an expiration and the work item re-checks it at dispatch
    // time; see Timeout below for why.
    unsigned revcount () const noexcept { return revcount_; }

    // Insert item as this object's successor: at the front, for a head.
    void add_after (Cque *item) noexcept
    {
        ++revcount_;
        item->prev_ = this;
        item->next_ = next_;
        next_->prev_ = item;
        next_ = item;
    }

    // Insert item as this object's predecessor: at the back, for a head.
    void add_before (Cque *item) noexcept
    {
        ++revcount_;
        item->next_ = this;
        item->prev_ = prev_;
        prev_->next_ = item;
        prev_ = item;
    }

    // Take this item off whatever queue it is on.  Relinking to self makes
    // a repeated call harmless, as in the Python original.
    void unlink () noexcept
    {
        ++revcount_;
        next_->prev_ = prev_;
        prev_->next_ = next_;
        reset ();
    }

    bool linked () const noexcept { return next_ != this; }

    Cque *next () const noexcept { return next_; }

private:
    Cque    *next_ = nullptr;
    Cque    *prev_ = nullptr;
    unsigned revcount_ = 0;
};

class Timer;

// Work item delivered when a timer expires.  Port of timers.Timeout.
//
// Expiration is noticed on the wheel's thread but delivered on the node's,
// so between the two the owning layer may have cancelled or restarted the
// very timer that is expiring.  Rather than make every layer defend against
// a stale timeout, the wheel records the timer's revcount when it pulls it
// off, and dispatch drops the item if the count has moved since.
class Timeout : public Work {
public:
    Timeout (Element *owner, Timer *timer, unsigned revcount) noexcept
        : Work (owner), timer_ (timer), revcount_ (revcount) {}

    const char *kind () const noexcept override { return "Timeout"; }
    void dispatch () override;

private:
    Timer   *timer_;
    unsigned revcount_;
};

// Anything that can sit on the wheel.  Port of timers.Timer.
class Timer : public Cque {
public:
    virtual ~Timer () = default;

    // Called from the node's main thread when this timer expires.
    virtual void timeout () = 0;

    // The element a Timeout work item is addressed to.  A layer that is
    // both an Element and a Timer returns itself.
    virtual Element *timer_owner () noexcept { return nullptr; }
};

// A timer that calls a function.  Port of timers.CallbackTimer.
class CallbackTimer : public Timer {
public:
    explicit CallbackTimer (std::function<void ()> fn) noexcept
        : fn_ (std::move (fn)) {}
    void timeout () override { if (fn_) fn_ (); }

private:
    std::function<void ()> fn_;
};

// The wheel itself.  Port of timers.TimerWheel.
class TimerWheel {
public:
    // maxtime is the longest interval the wheel can express, in seconds.
    TimerWheel (Node *node, std::chrono::milliseconds tick, unsigned maxtime);
    ~TimerWheel ();

    TimerWheel (const TimerWheel &) = delete;
    TimerWheel &operator= (const TimerWheel &) = delete;

    void startup ();
    void shutdown ();

    // Start (or restart) t so it expires in "seconds".  A timer already
    // running is moved, matching the Python semantics.
    void start (Timer *t, double seconds);
    void start (Timer *t, std::chrono::milliseconds ms);

    // Start with -10%/+0% jitter, as Phase V prescribes and as the older
    // phases benefit from.  Port of TimerWheel.jstart.
    void jstart (Timer *t, double seconds);

    // Cancel t if it is running.  Safe to call on a stopped timer.
    void stop (Timer *t);

    // Advance the wheel one tick and expire whatever is due.  Called by the
    // wheel's own thread; exposed so tests can drive it deterministically.
    void tick ();

    std::size_t slots () const noexcept { return wheel_.size (); }

private:
    void run ();

    Node                       *node_;
    std::chrono::milliseconds   tick_;
    std::vector<Cque>           wheel_;
    std::size_t                 pos_ = 0;
    std::mutex                  mutex_;
    std::thread                 thread_;
    std::atomic<bool>           running_ { false };
};

}   // namespace decnet

#endif  // DECNET_COMMON_TIMERS_H
