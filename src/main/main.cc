// src/main/main.cc -- the decnetd entry point.  Port of main.py.

#include "decnet/common/logging.h"
#include "decnet/config.h"
#include "decnet/node.h"
#include "decnet/version.h"

#include <csignal>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

void usage (const char *argv0)
{
    std::fprintf (stderr,
        "usage: %s [options] config-file ...\n"
        "\n"
        "  -L, --log-level LEVEL   trace, debug, info, warning, error\n"
        "  -e, --log-file FILE     log to FILE instead of stderr\n"
        "  -V, --version           print the version and exit\n"
        "  -h, --help              print this message\n",
        argv0);
}

}   // namespace

int main (int argc, char **argv)
{
    std::vector<std::string> config_files;
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
        // PORT: pydecnet builds one Node per config file and runs them all
        // in one process.  Only the first is started here so far.
        decnet::Config cfg = decnet::Config::from_file (config_files.front ());
        if (config_files.size () > 1)
            DN_WARN ("only the first configuration file is used so far");

        // Take delivery of the shutdown signals on this thread with
        // sigwait rather than in a handler: the handler would have to reach
        // the work queue, and none of the queue is async-signal-safe.
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
    } catch (const std::exception &e) {
        DN_CRIT ("fatal: {}", e.what ());
        std::fprintf (stderr, "%s: %s\n", argv[0], e.what ());
        return 1;
    }

    DN_INFO ("shut down");
    return 0;
}
