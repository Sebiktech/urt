#include "urt/core/time.hpp"

#include <stdexcept>

namespace urt {

FixedTimestep::FixedTimestep(double tick_rate_hz, int max_ticks_per_advance)
    : tick_length_(0), dt_seconds_(0.0), max_ticks_per_advance_(max_ticks_per_advance) {
    if (!(tick_rate_hz > 0.0)) {
        throw std::invalid_argument("FixedTimestep: tick rate must be positive");
    }
    if (max_ticks_per_advance < 1) {
        throw std::invalid_argument("FixedTimestep: max_ticks_per_advance must be at least 1");
    }
    tick_length_ = Nanoseconds(static_cast<Nanoseconds::rep>(1'000'000'000.0 / tick_rate_hz));
    if (tick_length_.count() < 1) {
        throw std::invalid_argument("FixedTimestep: tick rate is too high");
    }
    // Derive dt from the rounded tick length so both always agree.
    dt_seconds_ = static_cast<double>(tick_length_.count()) / 1'000'000'000.0;
}

int FixedTimestep::advance(Nanoseconds elapsed) {
    if (elapsed.count() > 0) {
        accumulator_ += elapsed;
    }

    auto due = accumulator_ / tick_length_; // integer division: whole ticks
    accumulator_ -= due * tick_length_;

    if (due > max_ticks_per_advance_) {
        dropped_ticks_ += static_cast<std::uint64_t>(due - max_ticks_per_advance_);
        due = max_ticks_per_advance_;
    }
    return static_cast<int>(due);
}

double FixedTimestep::alpha() const {
    return static_cast<double>(accumulator_.count()) / static_cast<double>(tick_length_.count());
}

} // namespace urt
