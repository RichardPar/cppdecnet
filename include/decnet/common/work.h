// decnet/common/work.h -- work items and the node work queue.
//
// One thread per node takes Work objects off the queue and dispatches
// them.  Helper threads (datalink receive, HTTP, timers) do blocking I/O
// and post work back.

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

    // Source MAC address of a frame received on a broadcast circuit (work.src
    // in PyDECnet).  Empty for point to point circuits.
    Received (Element *owner, Bytes packet, Macaddr src) noexcept
        : Work (owner), packet_ (std::move (packet)), src_ (src) {}

    const char *kind () const noexcept override { return "Received"; }

    const Bytes &packet () const noexcept { return packet_; }
    Bytes &packet () noexcept { return packet_; }
    Macaddr src () const noexcept { return src_; }

private:
    Bytes packet_;
    Macaddr src_ {};
};

// Run a function on the node thread.  Helper threads use this to touch
// layer state.
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
