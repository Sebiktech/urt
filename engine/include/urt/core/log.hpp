#pragma once

// Logging. Every message has a level and a channel (a short subsystem name
// such as "net" or "jobs"), so output can be filtered later.
//
//   urt::log::info("net", "listening on port {}", port);

#include <format>
#include <functional>
#include <string_view>
#include <utility>

namespace urt::log {

enum class Level { Trace, Debug, Info, Warn, Error, Off };

void set_level(Level level);
Level level();

// Parses "trace", "debug", "info", "warn", "error" or "off".
// Returns false and leaves `out` untouched for anything else.
bool parse_level(std::string_view text, Level& out);

// Where messages go. The default sink prints to stderr; tests replace it to
// capture output. Passing an empty sink restores the default.
using Sink = std::function<void(Level, std::string_view channel, std::string_view message)>;
void set_sink(Sink sink);

// Sends an already formatted message to the sink. Safe to call from any thread.
void write(Level level, std::string_view channel, std::string_view message);

template <class... Args>
void logf(Level lvl, std::string_view channel, std::format_string<Args...> fmt, Args&&... args) {
    // Check the level first so a filtered-out message costs no formatting.
    if (lvl < level()) {
        return;
    }
    write(lvl, channel, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void trace(std::string_view channel, std::format_string<Args...> fmt, Args&&... args) {
    logf(Level::Trace, channel, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void debug(std::string_view channel, std::format_string<Args...> fmt, Args&&... args) {
    logf(Level::Debug, channel, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void info(std::string_view channel, std::format_string<Args...> fmt, Args&&... args) {
    logf(Level::Info, channel, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void warn(std::string_view channel, std::format_string<Args...> fmt, Args&&... args) {
    logf(Level::Warn, channel, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void error(std::string_view channel, std::format_string<Args...> fmt, Args&&... args) {
    logf(Level::Error, channel, fmt, std::forward<Args>(args)...);
}

} // namespace urt::log
