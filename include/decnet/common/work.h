// decnet/common/work.h -- work items and the node work queue.
//
// pydecnet's concurrency model: one thread per node pulling Work objects
// off a queue and dispatching them to their owner, with helper threads
// (datalink receive, HTTP, timers) doing blocking I/O and posting work
// back.  It ports directly, and it is what makes the single-threaded
// reasoning in the DNA specs carry over to the implementation.

#ifndef DECNET_COMMON_WORK_H
#define DECNET_COMMON_WORK_H

#include "decnet/common/types.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace decnet {

class Element;

// Base for anything that can be queued to a node.  Port of common.Work.
class Work {
public:
    explicit Work (Element *owner) noexcept : owner_ (owner) {}
    virtual ~Work () = default;

    Work (const Work &) = delete;
    Work &operator= (const Work &) = delete;

    Element *owner () const noexcept { return owner_; }
    void set_owner (Element *e) noexcept { owner_ = e; }

    // Name used in trace logging and in the work statistics histogram.
    virtual const char *kind () const noexcept = 0;

    // Hand this item to its owner.  The default routes to
    // Element::dispatch; subclasses rarely need to override it.
    virtual void dispatch ();

private:
    Element *owner_;
};

using WorkPtr = std::unique_ptr<Work>;

// The sentinel that stops a node's main loop.  Port of common.Shutdown.
class Shutdown : public Work {
public:
    explicit Shutdown (Element *owner = nullptr) noexcept : Work (owner) {}
    const char *kind () const noexcept override { return "Shutdown"; }
    void dispatch () override {}
};

// An inbound message, from a datalink to the layer above.  Port of
// common.Received.
class Received : public Work {
public:
    Received (Element *owner, Bytes packet) noexcept
        : Work (owner), packet_ (std::move (packet)) {}

    const char *kind () const noexcept override { return "Received"; }

    const Bytes &packet () const noexcept { return packet_; }
    Bytes &packet () noexcept { return packet_; }

private:
    Bytes packet_;
};

// Run a function on the node thread.  Helper threads that must touch
// layer state -- a subprocess reader thread, say -- post one of these
// rather than reaching in from outside, which keeps the single threaded
// reasoning of the whole design intact.
class CallbackWork : public Work {
public:
    explicit CallbackWork (std::function<void ()> fn) noexcept
        : Work (nullptr), fn_ (std::move (fn)) {}

    const char *kind () const noexcept override { return "Callback"; }
    void dispatch () override { if (fn_) fn_ (); }

private:
    std::function<void ()> fn_;
};

// A thread safe FIFO of work items.  Any thread may put; one thread gets.
class WorkQueue {
public:
    void put (WorkPtr w)
    {
        {
            std::lock_guard lock (mutex_);
            queue_.push_back (std::move (w));
        }
        cond_.notify_one ();
    }

    // Blocks until an item is available.
    WorkPtr get ()
    {
        std::unique_lock lock (mutex_);
        cond_.wait (lock, [this] { return !queue_.empty (); });
        WorkPtr w = std::move (queue_.front ());
        queue_.pop_front ();
        return w;
    }

    std::size_t size () const
    {
        std::lock_guard lock (mutex_);
        return queue_.size ();
    }

private:
    mutable std::mutex      mutex_;
    std::condition_variable cond_;
    std::deque<WorkPtr>     queue_;
};

}   // namespace decnet

#endif  // DECNET_COMMON_WORK_H
