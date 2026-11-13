#include <doctest/doctest.h>

#include "urt/core/time.hpp"

#include <chrono>
#include <stdexcept>

using namespace std::chrono_literals;
using urt::FixedTimestep;

TEST_CASE("FixedTimestep returns whole ticks and keeps the remainder") {
    FixedTimestep step(10.0); // one tick = 100 ms

    CHECK(step.dt() == doctest::Approx(0.1));
    CHECK(step.advance(50ms) == 0);
    CHECK(step.alpha() == doctest::Approx(0.5));
    CHECK(step.advance(50ms) == 1);
    CHECK(step.alpha() == doctest::Approx(0.0));
    CHECK(step.advance(250ms) == 2);
    CHECK(step.time_until_next_tick() == 50ms);
}

TEST_CASE("FixedTimestep does not drift over many small frames") {
    FixedTimestep step(30.0);
    int ticks = 0;
    // One hour of 1 ms frames.
    for (int i = 0; i < 3'600'000; ++i) {
        ticks += step.advance(1ms);
    }
    // 30 ticks/s for 3600 s. The tick length is rounded down to whole
    // nanoseconds, so at most one extra tick can appear.
    CHECK(ticks >= 108'000);
    CHECK(ticks <= 108'001);
    CHECK(step.dropped_ticks() == 0);
}

TEST_CASE("FixedTimestep drops ticks after a long stall") {
    FixedTimestep step(10.0, 5);

    CHECK(step.advance(2s) == 5); // 20 ticks were due
    CHECK(step.dropped_ticks() == 15);
    // The backlog is gone: the next frame behaves normally.
    CHECK(step.advance(100ms) == 1);
}

TEST_CASE("FixedTimestep ignores negative time and rejects bad rates") {
    FixedTimestep step(10.0);
    CHECK(step.advance(-1s) == 0);
    CHECK(step.alpha() == doctest::Approx(0.0));

    CHECK_THROWS_AS(FixedTimestep(0.0), std::invalid_argument);
    CHECK_THROWS_AS(FixedTimestep(-5.0), std::invalid_argument);
    CHECK_THROWS_AS(FixedTimestep(30.0, 0), std::invalid_argument);
}
