#include "harness.h"

#include "decnet/common/logging.h"

#include <cstdlib>

#include <cstdio>
#include <cstring>

namespace dntest {

std::vector<TestCase> &registry ()
{
    static std::vector<TestCase> r;
    return r;
}

Registrar::Registrar (const char *suite, const char *name,
                      std::function<void ()> fn)
{
    registry ().push_back (TestCase { suite, name, std::move (fn) });
}

void fail (const char *file, int line, const std::string &msg)
{
    throw Failure { std::string (file) + ":" + std::to_string (line) + ": " + msg };
}

int run_all (const char *suite_filter)
{
    int passed = 0, failed = 0;
    for (const auto &t : registry ()) {
        if (suite_filter && std::strcmp (suite_filter, t.suite) != 0) continue;
        try {
            t.fn ();
            ++passed;
        } catch (const Failure &f) {
            std::fprintf (stderr, "FAIL %s.%s\n     %s\n",
                          t.suite, t.name, f.message.c_str ());
            ++failed;
        } catch (const std::exception &e) {
            std::fprintf (stderr, "FAIL %s.%s\n     unexpected exception: %s\n",
                          t.suite, t.name, e.what ());
            ++failed;
        } catch (...) {
            std::fprintf (stderr, "FAIL %s.%s\n     unexpected exception\n",
                          t.suite, t.name);
            ++failed;
        }
    }
    std::fprintf (stderr, "         %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}

}   // namespace dntest

int main (int argc, char **argv)
{
    // Tests run quietly by default.  DN_TEST_LOG=trace (or debug, info...)
    // turns the stack's own logging on, which is how you find out what a
    // failing end to end test was actually doing.
    if (const char *lvl = std::getenv ("DN_TEST_LOG"))
        ::decnet::logging::set_level (lvl);
    return dntest::run_all (argc > 1 ? argv[1] : nullptr);
}
