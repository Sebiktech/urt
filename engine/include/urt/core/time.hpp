#pragma once

// Time and the fixed timestep.
//
// The simulation always advances in steps of the same length (one "tick"),
// no matter how fast the machine is. That keeps physics and gameplay
// deterministic and makes the server and every client step the same way,
// which networking in step 2 depends on.

#include <chrono>
#include <cstdint>

namespace urt {

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;

class FixedTimestep {
public:
    // tick_rate_hz: simulation ticks per second.
    // max_ticks_per_advance: upper limit of ticks returned by one advance().
    //   If the process stalls (debugger, slow frame), running every missed
    //   tick would stall it even longer. Extra time is dropped instead.
    explicit FixedTimestep(double tick_rate_hz, int max_ticks_per_advance = 5);

    // Adds real elapsed time and returns how many ticks are due now.
    int advance(Nanoseconds elapsed);

    // Length of one tick in seconds.
    double dt() const { return dt_seconds_; }
    Nanoseconds tick_length() const { return tick_length_; }

    // How far we are between the last tick and the next one, 0..1.
    // A renderer uses this to interpolate so motion looks smooth.
    double alpha() const;

    // Real time left until the next tick is due.
    Nanoseconds time_until_next_tick() const { return tick_length_ - accumulator_; }

    // Ticks skipped so far because of the max_ticks_per_advance limit.
    std::uint64_t dropped_ticks() const { return dropped_ticks_; }

private:
    // Integer nanoseconds: adding many small doubles would drift over hours.
    Nanoseconds tick_length_;
    Nanoseconds accumulator_{0};
    double dt_seconds_;
    int max_ticks_per_advance_;
    std::uint64_t dropped_ticks_ = 0;
};

} // namespace urt
