#include "urt/app/application.hpp"

#include "urt/core/log.hpp"
#include "urt/core/platform.hpp"
#include "urt/core/time.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace urt {
namespace {

const char* role_name(Role role) { return role == Role::Server ? "server" : "client"; }

double to_milliseconds(Nanoseconds duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

} // namespace

Application::Application(AppConfig config)
    : config_(std::move(config)), jobs_(config_.worker_threads) {}

// Systems are destroyed before the world and the job system (members are
// destroyed in reverse order of declaration), so a system may safely hold
// references to both.
Application::~Application() = default;

void Application::tick_once(double dt) {
    const TickContext context{
        .tick = tick_,
        .dt = dt,
        .time = static_cast<double>(tick_) * dt,
        .jobs = jobs_,
    };
    for (const std::unique_ptr<System>& system : systems_) {
        system->tick(world_, context);
    }
    ++tick_;
}

void Application::step(std::uint64_t ticks) {
    const double dt = FixedTimestep(config_.tick_rate).dt();
    for (std::uint64_t i = 0; i < ticks; ++i) {
        tick_once(dt);
    }
}

int Application::run() {
    platform::install_shutdown_handler();

    FixedTimestep timestep(config_.tick_rate);
    log::info("app", "{} ({}) running at {} ticks/s with {} worker threads", config_.name,
              role_name(config_.role), config_.tick_rate, jobs_.worker_count());

    // Timing statistics for the periodic log line.
    const bool stats_enabled = config_.stats_interval_seconds > 0.0;
    const auto stats_interval = std::chrono::duration_cast<Nanoseconds>(
        std::chrono::duration<double>(config_.stats_interval_seconds));
    Nanoseconds stats_total{0};
    Nanoseconds stats_worst{0};
    std::uint64_t stats_ticks = 0;
    std::uint64_t reported_dropped = 0;

    Clock::time_point last = Clock::now();
    Clock::time_point stats_start = last;

    auto keep_running = [this] {
        return !stop_requested_.load() && !platform::shutdown_requested();
    };

    while (keep_running()) {
        const Clock::time_point now = Clock::now();
        const int due = timestep.advance(now - last);
        last = now;

        for (int i = 0; i < due && keep_running(); ++i) {
            const Clock::time_point tick_start = Clock::now();
            tick_once(timestep.dt());
            const Nanoseconds tick_time = Clock::now() - tick_start;

            stats_total += tick_time;
            stats_worst = std::max(stats_worst, tick_time);
            ++stats_ticks;

            if (config_.max_ticks != 0 && tick_ >= config_.max_ticks) {
                stop();
            }
        }

        if (stats_enabled && now - stats_start >= stats_interval && stats_ticks > 0) {
            log::info("app", "tick {}: avg {:.3f} ms, worst {:.3f} ms, budget {:.3f} ms, {} entities",
                      tick_, to_milliseconds(stats_total) / static_cast<double>(stats_ticks),
                      to_milliseconds(stats_worst), to_milliseconds(timestep.tick_length()),
                      world_.entity_count());

            // Dropped ticks mean the simulation cannot keep up with real time.
            const std::uint64_t dropped = timestep.dropped_ticks();
            if (dropped > reported_dropped) {
                log::warn("app", "dropped {} ticks, the server is overloaded",
                          dropped - reported_dropped);
                reported_dropped = dropped;
            }

            stats_total = Nanoseconds{0};
            stats_worst = Nanoseconds{0};
            stats_ticks = 0;
            stats_start = now;
        }

        // Nothing to render yet, so sleep until the next tick instead of
        // spinning a core at 100 %.
        if (keep_running()) {
            std::this_thread::sleep_for(timestep.time_until_next_tick());
        }
    }

    log::info("app", "{} stopped after {} ticks", config_.name, tick_);
    return 0;
}

} // namespace urt
