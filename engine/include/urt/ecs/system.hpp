#pragma once

// A system is one piece of game logic that runs once per tick.

#include <cstdint>
#include <string_view>

namespace urt {

class JobSystem;
struct World;

// What a system knows about the tick it is running in.
struct TickContext {
    std::uint64_t tick; // index of this tick, starting at 0
    double dt;          // length of one tick in seconds, always the same
    double time;        // simulated seconds since start: tick * dt
    JobSystem& jobs;    // for spreading work over threads inside the tick
};

class System {
public:
    virtual ~System() = default;

    virtual std::string_view name() const = 0;
    virtual void tick(World& world, const TickContext& context) = 0;
};

} // namespace urt
