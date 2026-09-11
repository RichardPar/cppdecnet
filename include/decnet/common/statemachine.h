// decnet/common/statemachine.h -- state machine base class.
//
// Port of statemachine.py.  In Python a state is a bound method and the
// action returns the next state, or None to stay put.  The C++ equivalent
// is a pointer to member function paired with its name, so that ported
// state functions read almost like their originals and trace output still
// names states the way the Python's does:
//
//     State Circuit::ds (Work &w) {
//         if (is<DlUp> (w)) return DN_STATE (Circuit, ri);
//         return nullptr;                     // Python's "return None"
//     }
//
// State machines derive from Timer because, as the Python comment observes,
// as a rule every state machine needs timeouts; a timer expiry is fed in as
// just another input.

#ifndef DECNET_COMMON_STATEMACHINE_H
#define DECNET_COMMON_STATEMACHINE_H

#include "decnet/common/logging.h"
#include "decnet/common/timers.h"
#include "decnet/common/work.h"

#include <string>

namespace decnet {

template <typename Derived>
class StateMachine : public Timer {
public:
    // A state action's result: the next state, or an empty State to stay
    // where we are.  The self reference is why this is a struct rather
    // than a plain pointer to member typedef.
    struct State {
        using Fn = State (Derived::*) (Work &);

        Fn          fn   = nullptr;
        const char *name = "";

        constexpr State () noexcept = default;
        constexpr State (std::nullptr_t) noexcept {}
        constexpr State (Fn f, const char *n) noexcept : fn (f), name (n) {}

        constexpr explicit operator bool () const noexcept
        { return fn != nullptr; }

        friend constexpr bool operator== (const State &a, const State &b) noexcept
        { return a.fn == b.fn; }
    };

    // The initial state is Derived::s0, exactly as in the Python version.
    StateMachine () noexcept : state_ (&Derived::s0, "s0") {}

    // Run the current state's action over one input.
    void dispatch (Work &w)
    {
        Derived &self = static_cast<Derived &> (*this);
        if (!self.validate (w)) {
            DN_TRACE ("{} {} skipped by validate", statename (), w.kind ());
            return;
        }
        set_state ((self.*(state_.fn)) (w));
    }

    // Override to run checks or actions common to every state.  Returning
    // false skips the state action.
    bool validate (Work &) { return true; }

    void set_state (State next, const char *why = "")
    {
        if (!next) {
            DN_TRACE ("{}{}no state change", why, *why ? ", " : "");
            return;
        }
        state_ = next;
        DN_TRACE ("{}{}new state {}", why, *why ? ", " : "", state_.name);
    }

    State state () const noexcept { return state_; }
    bool in_state (State s) const noexcept { return state_ == s; }
    const char *state_name () const noexcept { return state_.name; }

    // "Class<state: name>", as StateMachine.statename produces.
    virtual std::string statename () const
    { return std::string (Derived::class_name) + "<state: " + state_.name + ">"; }

    // A timer expiry is an input like any other.  The Python version passes
    // a Timeout object to the state action; so do we.
    void timeout () override
    {
        Timeout t (nullptr, nullptr, 0);
        dispatch (t);
    }

private:
    State state_;
};

// Spell a state constant: DN_STATE (Circuit, running) -> { &Circuit::running,
// "running" }.  Keeping the name with the pointer is what lets trace output
// match the Python original.
#define DN_STATE(cls, m) typename cls::State (&cls::m, #m)

// The same, used inside the class itself where "typename" is not wanted.
#define DN_MY_STATE(cls, m) State (&cls::m, #m)

}   // namespace decnet

#endif  // DECNET_COMMON_STATEMACHINE_H
