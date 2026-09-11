// tests/harness.h -- a small unit test harness.
//
// the Python's tests use unittest.  Rather than take on a dependency for the
// port, this provides the same shape: test cases register themselves, the
// runner runs them all and reports failures, and each assertion macro
// prints the file, line and the values involved.

#ifndef DECNET_TESTS_HARNESS_H
#define DECNET_TESTS_HARNESS_H

#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace dntest {

struct TestCase {
    const char           *suite;
    const char           *name;
    std::function<void ()> fn;
};

// Registry.  Tests add themselves through the DN_TEST macro below.
std::vector<TestCase> &registry ();
int run_all (const char *suite_filter = nullptr);

// Thrown by a failing assertion; the runner catches it and moves on.
struct Failure {
    std::string message;
};

void fail (const char *file, int line, const std::string &msg);

// Format a value for a failure message, falling back to a placeholder for
// types with no operator<<.
template <typename T>
std::string show (const T &v)
{
    if constexpr (requires (std::ostream &o, const T &x) { o << x; }) {
        std::ostringstream s;
        s << v;
        return s.str ();
    } else if constexpr (requires (const T &x) { x.str (); }) {
        return v.str ();
    } else {
        return "<value>";
    }
}

struct Registrar {
    Registrar (const char *suite, const char *name, std::function<void ()> fn);
};

}   // namespace dntest

// Define a test: DN_TEST (nodeid, parse) { ... }
#define DN_TEST(suite, name)                                                 \
    static void dntest_##suite##_##name ();                                  \
    static ::dntest::Registrar dntest_reg_##suite##_##name                   \
        (#suite, #name, dntest_##suite##_##name);                            \
    static void dntest_##suite##_##name ()

#define DN_ASSERT(cond)                                                      \
    do {                                                                     \
        if (!(cond))                                                         \
            ::dntest::fail (__FILE__, __LINE__, "assertion failed: " #cond); \
    } while (0)

// The operands are copied, not bound by reference: an accessor returning a
// reference into a temporary (v.parse (x).as_string (), say) would otherwise
// leave the reference dangling before the comparison runs.
#define DN_ASSERT_EQ(a, b)                                                   \
    do {                                                                     \
        const auto dn_a_ = (a);                                              \
        const auto dn_b_ = (b);                                              \
        if (!(dn_a_ == dn_b_))                                               \
            ::dntest::fail (__FILE__, __LINE__,                              \
                            std::string (#a " == " #b " -- got ")            \
                            + ::dntest::show (dn_a_) + " vs "                \
                            + ::dntest::show (dn_b_));                       \
    } while (0)

#define DN_ASSERT_NE(a, b)                                                   \
    do {                                                                     \
        if ((a) == (b))                                                      \
            ::dntest::fail (__FILE__, __LINE__, #a " != " #b " failed");     \
    } while (0)

// Assert that expr throws exception type E.
#define DN_ASSERT_THROWS(E, expr)                                            \
    do {                                                                     \
        bool dn_threw_ = false;                                              \
        try { (void) (expr); }                                               \
        catch (const E &) { dn_threw_ = true; }                              \
        catch (...) {                                                        \
            ::dntest::fail (__FILE__, __LINE__,                              \
                            #expr " threw the wrong exception type");        \
        }                                                                    \
        if (!dn_threw_)                                                      \
            ::dntest::fail (__FILE__, __LINE__,                              \
                            #expr " did not throw " #E);                     \
    } while (0)

#endif  // DECNET_TESTS_HARNESS_H
