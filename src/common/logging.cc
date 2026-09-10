#include "decnet/common/logging.h"

#include <chrono>
#include <cstdio>
#include <mutex>

namespace decnet::logging {

Level threshold = Level::info;

namespace {

std::mutex             out_mutex;
std::FILE             *out_file = stderr;
thread_local std::string thread_name = "main";

const char *level_name (Level l) noexcept
{
    switch (l) {
    case Level::trace:    return "TRACE";
    case Level::debug:    return "DEBUG";
    case Level::info:     return "INFO";
    case Level::warning:  return "WARNING";
    case Level::error:    return "ERROR";
    case Level::critical: return "CRITICAL";
    }
    return "?";
}

}   // namespace

void set_level (Level l) noexcept { threshold = l; }

bool set_level (std::string_view name) noexcept
{
    if (name == "trace")         threshold = Level::trace;
    else if (name == "debug")    threshold = Level::debug;
    else if (name == "info")     threshold = Level::info;
    else if (name == "warning")  threshold = Level::warning;
    else if (name == "error")    threshold = Level::error;
    else if (name == "critical") threshold = Level::critical;
    else return false;
    return true;
}

bool set_logfile (const std::string &path)
{
    std::FILE *f = std::fopen (path.c_str (), "ae");
    if (!f) return false;
    std::lock_guard lock (out_mutex);
    if (out_file != stderr) std::fclose (out_file);
    out_file = f;
    return true;
}

void set_thread_name (std::string name) { thread_name = std::move (name); }

void emit (Level l, std::string_view msg) noexcept
{
    using namespace std::chrono;
    auto now = system_clock::now ();
    auto secs = time_point_cast<seconds> (now);
    auto ms   = duration_cast<milliseconds> (now - secs).count ();
    std::time_t t = system_clock::to_time_t (now);
    std::tm tm {};
    localtime_r (&t, &tm);
    char ts[32];
    std::strftime (ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);

    std::lock_guard lock (out_mutex);
    std::fprintf (out_file, "%s.%03d: %s: %s: %.*s\n", ts,
                  static_cast<int> (ms), thread_name.c_str (), level_name (l),
                  static_cast<int> (msg.size ()), msg.data ());
    std::fflush (out_file);
}

}   // namespace decnet::logging
