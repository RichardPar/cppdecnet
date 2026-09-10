// Port of tests/test_timers.py: the timer wheel.
//
// The wheel is driven by hand here rather than by its thread, so the tests
// are deterministic.

#include "harness.h"

#include "decnet/common/timers.h"

using namespace decnet;

namespace {

struct CountingTimer : Timer {
    int fired = 0;
    void timeout () override { ++fired; }
};

}   // namespace

DN_TEST (timers, fires_after_the_right_number_of_ticks)
{
    TimerWheel w (nullptr, std::chrono::milliseconds (100), 60);
    CountingTimer t;
    w.start (&t, 0.5);                 // five ticks
    for (int i = 0; i < 4; ++i) w.tick ();
    DN_ASSERT_EQ (t.fired, 0);
    w.tick ();
    DN_ASSERT_EQ (t.fired, 1);
    // A timer that has fired is off the wheel and does not repeat.
    for (int i = 0; i < 20; ++i) w.tick ();
    DN_ASSERT_EQ (t.fired, 1);
}

DN_TEST (timers, minimum_timeout_is_one_tick)
{
    TimerWheel w (nullptr, std::chrono::milliseconds (100), 60);
    CountingTimer t;
    w.start (&t, 0.0);
    w.tick ();
    DN_ASSERT_EQ (t.fired, 1);
}

DN_TEST (timers, stop_cancels)
{
    TimerWheel w (nullptr, std::chrono::milliseconds (100), 60);
    CountingTimer t;
    w.start (&t, 0.3);
    w.tick ();
    w.stop (&t);
    for (int i = 0; i < 10; ++i) w.tick ();
    DN_ASSERT_EQ (t.fired, 0);
}

DN_TEST (timers, restart_moves_the_timer)
{
    TimerWheel w (nullptr, std::chrono::milliseconds (100), 60);
    CountingTimer t;
    w.start (&t, 0.3);
    w.tick ();
    w.start (&t, 0.5);                 // restart, so the old slot is stale
    for (int i = 0; i < 4; ++i) w.tick ();
    DN_ASSERT_EQ (t.fired, 0);
    w.tick ();
    DN_ASSERT_EQ (t.fired, 1);
}

DN_TEST (timers, same_expiry_fires_in_start_order)
{
    // NSP relies on this: timers armed for the same instant must fire in
    // the order they were started, or retransmissions go out of sequence.
    TimerWheel w (nullptr, std::chrono::milliseconds (100), 60);
    struct OrderTimer : Timer {
        std::vector<int> *log;
        int id;
        void timeout () override { log->push_back (id); }
    };
    std::vector<int> log;
    OrderTimer a, b, c;
    a.log = b.log = c.log = &log;
    a.id = 1; b.id = 2; c.id = 3;
    w.start (&a, 0.2);
    w.start (&b, 0.2);
    w.start (&c, 0.2);
    w.tick ();
    w.tick ();
    DN_ASSERT_EQ (log.size (), 3u);
    DN_ASSERT_EQ (log[0], 1);
    DN_ASSERT_EQ (log[1], 2);
    DN_ASSERT_EQ (log[2], 3);
}

DN_TEST (timers, rejects_timeout_beyond_the_wheel)
{
    TimerWheel w (nullptr, std::chrono::milliseconds (100), 10);
    CountingTimer t;
    DN_ASSERT_THROWS (std::overflow_error, w.start (&t, 3600.0));
}

DN_TEST (timers, revcount_tracks_operations)
{
    // The revcount is what lets a stale Timeout be discarded when a layer
    // cancels a timer between expiry and dispatch.
    TimerWheel w (nullptr, std::chrono::milliseconds (100), 60);
    CountingTimer t;
    unsigned before = t.revcount ();
    w.start (&t, 1.0);
    DN_ASSERT_NE (t.revcount (), before);
    unsigned after_start = t.revcount ();
    w.stop (&t);
    DN_ASSERT_NE (t.revcount (), after_start);
}
