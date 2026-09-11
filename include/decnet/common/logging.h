// decnet/common/logging.h -- leveled logging.
//
// the Python leans on the Python logging module, including its own TRACE
// level and the deferred "{}" formatting that keeps trace calls cheap when
// tracing is off.  We keep the same levels and the same laziness: the
// arguments to DN_TRACE are not evaluated unless the level is enabled.

#ifndef DECNET_COMMON_LOGGING_H
#define DECNET_COMMON_LOGGING_H

#include <format>
#include <string>
#include <string_view>

namespace decnet::logging {

enum class Level : int {
    trace = 5, debug = 10, info = 20, warning = 30, error = 40, critical = 50
};

// Current threshold; messages below it are dropped.  Read directly by the
// macros so the common case is one integer compare.
extern Level threshold;

inline bool enabled (Level l) noexcept
{ return static_cast<int> (l) >= static_cast<int> (threshold); }

void set_level (Level l) noexcept;
bool set_level (std::string_view name) noexcept;   // "trace", "debug", ...

// Route output to a file instead of stderr.  Returns false if it cannot be
// opened, leaving the previous destination in place.
bool set_logfile (const std::string &path);

// Prefix every line with this thread's name, the way the Python names each
// system's thread after the node.
void set_thread_name (std::string name);

// The one function that actually writes; the macros below format first.
void emit (Level l, std::string_view msg) noexcept;

template <typename... A>
void log (Level l, std::format_string<A...> fmt, A &&...args)
{
    if (enabled (l))
        emit (l, std::format (fmt, std::forward<A> (args)...));
}

}   // namespace decnet::logging

// Deferred evaluation: the format call never happens when the level is off.
#define DN_LOG(lvl, ...)                                                     \
    do {                                                                     \
        if (::decnet::logging::enabled (lvl))                                \
            ::decnet::logging::log (lvl, __VA_ARGS__);                       \
    } while (0)

#define DN_TRACE(...) DN_LOG (::decnet::logging::Level::trace, __VA_ARGS__)
#define DN_DEBUG(...) DN_LOG (::decnet::logging::Level::debug, __VA_ARGS__)
#define DN_INFO(...)  DN_LOG (::decnet::logging::Level::info, __VA_ARGS__)
#define DN_WARN(...)  DN_LOG (::decnet::logging::Level::warning, __VA_ARGS__)
#define DN_ERROR(...) DN_LOG (::decnet::logging::Level::error, __VA_ARGS__)
#define DN_CRIT(...)  DN_LOG (::decnet::logging::Level::critical, __VA_ARGS__)

#endif  // DECNET_COMMON_LOGGING_H
