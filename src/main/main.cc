// src/main/main.cc -- the decnetd entry point.  Port of main.py.

#include "decnet/common/logging.h"
#include "decnet/config.h"
#include "decnet/node.h"
#include "decnet/nodefetch.h"
#include "decnet/version.h"

#include <csignal>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <windows.h>
#endif

namespace {

#ifdef _WIN32

// Windows has no SIGTERM or sigwait.  Console control events (Ctrl-C,
// Ctrl-Break, closing the window, logoff, shutdown) arrive on a thread of
// their own; the handler passes them to main, which does the stopping.
std::mutex              stop_mutex;
std::condition_variable stop_cv;
int                     stop_event = -1;
bool                    stopped = false;

BOOL WINAPI on_console_event (DWORD ev)
{
    std::unique_lock<std::mutex> lk (stop_mutex);
    if (stop_event < 0) stop_event = static_cast<int> (ev);
    stop_cv.notify_all ();
    // The process ends as soon as this returns for a close, logoff or
    // shutdown, so give the node a few seconds to stop cleanly first.
    stop_cv.wait_for (lk, std::chrono::seconds (4), [] { return stopped; });
    return TRUE;
}

#endif

void usage (const char *argv0)
{
    std::fprintf (stderr,
        "usage: %s [options] config-file ...\n"
        "\n"
        "  -L, --log-level LEVEL   trace, debug, info, warning, error\n"
        "  -e, --log-file FILE     log to FILE instead of stderr\n"
        "  -V, --version           print the version and exit\n"
        "      --fetch-nodes       refresh the node name caches and exit\n"
        "  -h, --help              print this message\n",
        argv0);
}

}   // namespace

int main (int argc, char **argv)
{
    std::vector<std::string> config_files;
    bool fetch_nodes = false;
    std::string log_file;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&] (const char *what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf (stderr, "%s: %s needs a value\n", argv[0], what);
                std::exit (2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help")    { usage (argv[0]); return 0; }
        else if (a == "-V" || a == "--version") {
            std::printf ("%s\n", decnet::version::banner ().c_str ());
            return 0;
        }
        else if (a == "-L" || a == "--log-level") {
            std::string lvl = next ("--log-level");
            if (!decnet::logging::set_level (lvl)) {
                std::fprintf (stderr, "%s: unknown log level %s\n",
                              argv[0], lvl.c_str ());
                return 2;
            }
        }
        else if (a == "-e" || a == "--log-file") log_file = next ("--log-file");
        else if (a == "--fetch-nodes") fetch_nodes = true;
        else if (!a.empty () && a[0] == '-') {
            std::fprintf (stderr, "%s: unknown option %s\n", argv[0], a.c_str ());
            usage (argv[0]);
            return 2;
        }
        else config_files.push_back (a);
    }

    if (!log_file.empty () && !decnet::logging::set_logfile (log_file)) {
        std::fprintf (stderr, "%s: cannot open log file %s: %s\n",
                      argv[0], log_file.c_str (), std::strerror (errno));
        return 1;
    }

    if (config_files.empty ()) {
        std::fprintf (stderr, "%s: no configuration file given\n", argv[0]);
        usage (argv[0]);
        return 2;
    }

    DN_INFO ("{} starting", decnet::version::ident ());

    try {
        // PORT: PyDECnet builds one Node per config file and runs them all
        // in one process.  Only the first is started here so far.
        decnet::Config cfg = decnet::Config::from_file (config_files.front ());
        if (config_files.size () > 1)
            DN_WARN ("only the first configuration file is used so far");

        if (fetch_nodes) {
            // Refresh the caches and stop, so this can be run from cron or
            // by hand without a node running.
            if (cfg.node_sources ().empty ()) {
                std::fprintf (stderr, "%s: no \"node @<url>\" line in %s\n",
                              argv[0], config_files.front ().c_str ());
                return 2;
            }
            int bad = 0;
            for (const auto &src : cfg.node_sources ()) {
                decnet::FetchedList got;
                std::string error;
                std::string since = decnet::read_cached_validator (src.cache);
                if (!decnet::fetch_node_list (src.url, got, error, 30, since)) {
                    std::fprintf (stderr, "%s: %s: %s\n", argv[0],
                                  src.url.c_str (), error.c_str ());
                    ++bad;
                    continue;
                }
                if (got.unchanged) {
                    std::printf ("%s unchanged since %s\n", src.url.c_str (),
                                 since.c_str ());
                    continue;
                }
                if (!decnet::write_node_list (src.cache, got.names,
                                              got.last_modified, error)) {
                    std::fprintf (stderr, "%s: %s\n", argv[0], error.c_str ());
                    ++bad;
                    continue;
                }
                std::printf ("%zu node names from %s to %s\n",
                             got.names.size (), src.url.c_str (),
                             src.cache.c_str ());
            }
            return bad ? 1 : 0;
        }

#ifdef _WIN32
        ::SetConsoleCtrlHandler (on_console_event, TRUE);

        decnet::Node node (cfg);
        node.start ();

        int ev;
        {
            std::unique_lock<std::mutex> lk (stop_mutex);
            stop_cv.wait (lk, [] { return stop_event >= 0; });
            ev = stop_event;
        }
        DN_INFO ("caught console event {}, shutting down", ev);
        node.stop ();
        {
            std::lock_guard<std::mutex> lk (stop_mutex);
            stopped = true;
        }
        stop_cv.notify_all ();
#else
        // An object program that exits leaves a pipe with no reader.  Writing
        // to it must fail with EPIPE, not kill the node.
        std::signal (SIGPIPE, SIG_IGN);

        // Handle shutdown signals with sigwait on this thread; the work queue is
        // not async-signal-safe.
        sigset_t stopset;
        sigemptyset (&stopset);
        sigaddset (&stopset, SIGINT);
        sigaddset (&stopset, SIGTERM);
        pthread_sigmask (SIG_BLOCK, &stopset, nullptr);

        decnet::Node node (cfg);
        node.start ();

        int sig = 0;
        while (sigwait (&stopset, &sig) == EINTR)
            ;
        DN_INFO ("caught signal {}, shutting down", sig);
        node.stop ();
#endif
    } catch (const std::exception &e) {
        DN_CRIT ("fatal: {}", e.what ());
        std::fprintf (stderr, "%s: %s\n", argv[0], e.what ());
        return 1;
    }

    DN_INFO ("shut down");
    return 0;
}
