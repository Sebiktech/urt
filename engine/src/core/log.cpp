#include "urt/core/log.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

namespace urt::log {
namespace {

std::atomic<Level> g_level{Level::Info};

// Guards the sink and keeps lines from different threads from interleaving.
std::mutex g_mutex;
Sink g_sink;

const auto g_start = std::chrono::steady_clock::now();

const char* level_name(Level level) {
    switch (level) {
    case Level::Trace: return "TRACE";
    case Level::Debug: return "DEBUG";
    case Level::Info:  return "INFO ";
    case Level::Warn:  return "WARN ";
    case Level::Error: return "ERROR";
    case Level::Off:   break;
    }
    return "?????";
}

} // namespace

void set_level(Level level) { g_level.store(level, std::memory_order_relaxed); }

Level level() { return g_level.load(std::memory_order_relaxed); }

bool parse_level(std::string_view text, Level& out) {
    struct Entry {
        std::string_view name;
        Level level;
    };
    static constexpr std::array<Entry, 6> entries{{
        {"trace", Level::Trace},
        {"debug", Level::Debug},
        {"info", Level::Info},
        {"warn", Level::Warn},
        {"error", Level::Error},
        {"off", Level::Off},
    }};
    for (const Entry& entry : entries) {
        if (entry.name == text) {
            out = entry.level;
            return true;
        }
    }
    return false;
}

void set_sink(Sink sink) {
    std::lock_guard lock(g_mutex);
    g_sink = std::move(sink);
}

void write(Level lvl, std::string_view channel, std::string_view message) {
    if (lvl < level() || lvl == Level::Off) {
        return;
    }

    std::lock_guard lock(g_mutex);
    if (g_sink) {
        g_sink(lvl, channel, message);
        return;
    }

    // Seconds since the process started: short, and enough to read timing.
    const std::chrono::duration<double> uptime = std::chrono::steady_clock::now() - g_start;
    const std::string line =
        std::format("[{:10.3f}] {} {:<6} {}\n", uptime.count(), level_name(lvl), channel, message);
    std::fwrite(line.data(), 1, line.size(), stderr);
}

} // namespace urt::log
