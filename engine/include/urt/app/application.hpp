#pragma once

// Application: owns the world, the job system and the list of systems, and
// drives them with a fixed tick. The server and the client are both an
// Application; they differ in which systems they add.

#include "urt/core/jobs.hpp"
#include "urt/ecs/system.hpp"
#include "urt/ecs/world.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace urt {

enum class Role { Server, Client };

struct AppConfig {
    std::string name = "urt";
    Role role = Role::Server;
    double tick_rate = 30.0;             // simulation ticks per second
    std::uint64_t max_ticks = 0;         // run() stops after this many ticks; 0 = no limit
    double stats_interval_seconds = 5.0; // how often run() logs tick timing; 0 = never
    unsigned worker_threads = 0;         // 0 = pick from the hardware
};

class Application {
public:
    explicit Application(AppConfig config);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    const AppConfig& config() const { return config_; }
    World& world() { return world_; }
    JobSystem& jobs() { return jobs_; }

    // Number of ticks simulated so far.
    std::uint64_t tick() const { return tick_; }

    // Changes how many ticks run per second, from the next tick on. A client
    // uses it to tick at the rate its server announced.
    void set_tick_rate(double ticks_per_second) { config_.tick_rate = ticks_per_second; }

    // Adds a system. Systems run every tick in the order they were added.
    template <class T, class... Args>
    T& add_system(Args&&... args) {
        auto system = std::make_unique<T>(std::forward<Args>(args)...);
        T& reference = *system;
        systems_.push_back(std::move(system));
        return reference;
    }

    // Simulates `ticks` ticks immediately, ignoring the clock.
    // Used by tests and tools: the result is the same on every machine.
    void step(std::uint64_t ticks = 1);

    // Runs in real time until stop() is called, the OS asks the process to
    // quit (Ctrl+C), or max_ticks is reached. Returns the process exit code.
    int run();

    // Asks run() to return after the current tick. Safe from any thread.
    void stop() { stop_requested_.store(true); }

private:
    void tick_once(double dt);

    AppConfig config_;
    JobSystem jobs_;
    World world_;
    std::vector<std::unique_ptr<System>> systems_;
    std::uint64_t tick_ = 0;
    std::atomic<bool> stop_requested_{false};
};

} // namespace urt
