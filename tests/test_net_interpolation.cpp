#include <doctest/doctest.h>

#include "urt/net/interpolation.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using urt::net::lerp_angle;
using urt::net::PlaybackClock;
using urt::net::Pose;
using urt::net::PoseHistory;

namespace {

Pose at_x(float x, float yaw = 0.0f) { return Pose{.position = {x, 0.0f, 0.0f}, .yaw = yaw}; }

} // namespace

TEST_CASE("PoseHistory blends between the surrounding samples") {
    PoseHistory history;
    Pose pose;
    CHECK_FALSE(history.sample(1.0, pose)); // nothing known yet

    history.push(1.0, at_x(10.0f));
    REQUIRE(history.sample(0.0, pose)); // a single sample answers every time
    CHECK(pose.position.x == 10.0f);

    history.push(2.0, at_x(20.0f));
    history.push(3.0, at_x(40.0f));

    REQUIRE(history.sample(1.5, pose));
    CHECK(pose.position.x == doctest::Approx(15.0f));
    REQUIRE(history.sample(2.25, pose));
    CHECK(pose.position.x == doctest::Approx(25.0f));
    REQUIRE(history.sample(2.0, pose)); // exactly on a sample
    CHECK(pose.position.x == doctest::Approx(20.0f));

    // Outside the known range it holds the nearest end instead of guessing.
    REQUIRE(history.sample(0.5, pose));
    CHECK(pose.position.x == 10.0f);
    REQUIRE(history.sample(99.0, pose));
    CHECK(pose.position.x == 40.0f);
}

TEST_CASE("PoseHistory keeps the newest samples and ignores stale ones") {
    PoseHistory history;
    for (int i = 0; i < 20; ++i) {
        history.push(static_cast<double>(i), at_x(static_cast<float>(i)));
    }
    CHECK(history.size() == PoseHistory::capacity);

    Pose pose;
    REQUIRE(history.sample(18.5, pose));
    CHECK(pose.position.x == doctest::Approx(18.5f));
    // Samples 0..11 were overwritten; 12 is the oldest that is left.
    REQUIRE(history.sample(3.0, pose));
    CHECK(pose.position.x == 12.0f);

    history.push(5.0, at_x(-100.0f));  // older than the newest: dropped
    history.push(19.0, at_x(-100.0f)); // same time as the newest: dropped
    REQUIRE(history.sample(19.0, pose));
    CHECK(pose.position.x == 19.0f);
}

TEST_CASE("angles blend the short way round") {
    const float pi = std::numbers::pi_v<float>;
    const float degree = pi / 180.0f;

    CHECK(lerp_angle(0.0f, 90.0f * degree, 0.5f) == doctest::Approx(45.0f * degree));
    // 350 to 10 degrees passes through 0 (= 360), not back through 180.
    CHECK(lerp_angle(350.0f * degree, 10.0f * degree, 0.5f) == doctest::Approx(360.0f * degree));
    CHECK(lerp_angle(10.0f * degree, 350.0f * degree, 0.5f) == doctest::Approx(0.0f).epsilon(1e-4));

    PoseHistory history;
    history.push(0.0, at_x(0.0f, 170.0f * degree));
    history.push(1.0, at_x(0.0f, -170.0f * degree));
    Pose pose;
    REQUIRE(history.sample(0.5, pose));
    CHECK(std::cos(pose.yaw) == doctest::Approx(-1.0f)); // 180 degrees, not 0
}

TEST_CASE("PlaybackClock runs behind the server and corrects itself gently") {
    PlaybackClock clock(0.1);
    clock.advance(5.0); // before the first snapshot there is nothing to play
    CHECK(clock.time() == 0.0);

    clock.observe(10.0); // first snapshot: start exactly `delay` behind it
    CHECK(clock.time() == doctest::Approx(9.9));

    clock.advance(0.016);
    CHECK(clock.time() == doctest::Approx(9.916));

    // The snapshot says we are 0.05 s behind where we should be: close a tenth.
    clock.observe(10.066);
    CHECK(clock.time() == doctest::Approx(9.916 + 0.05 * 0.1));

    // Far out of step (the program was paused): jump instead of creeping.
    clock.observe(60.0);
    CHECK(clock.time() == doctest::Approx(59.9));
}

TEST_CASE("interpolated motion is smooth although snapshots arrive unevenly") {
    // An entity walks at 4 m/s. The server sends its position 30 times a
    // second, each packet arrives up to 12 ms early or late, and one in ten
    // is lost. The screen draws 60 times a second.
    constexpr double speed = 4.0;
    constexpr double snapshot_interval = 1.0 / 30.0;
    constexpr double frame = 1.0 / 60.0;

    std::mt19937 random(7);
    std::uniform_real_distribution<double> jitter(-0.012, 0.012);
    std::uniform_int_distribution<int> lost(0, 9);

    struct Arrival {
        double local_time;
        double server_time;
    };
    std::vector<Arrival> arrivals;
    for (int i = 0; i < 300; ++i) {
        if (i > 5 && lost(random) == 0) {
            continue;
        }
        const double server_time = i * snapshot_interval;
        arrivals.push_back({server_time + 0.03 + jitter(random), server_time}); // 30 ms of latency
    }
    std::sort(arrivals.begin(), arrivals.end(),
              [](const Arrival& a, const Arrival& b) { return a.local_time < b.local_time; });

    PoseHistory history;
    PlaybackClock clock(0.1);
    std::size_t next_arrival = 0;
    std::vector<double> steps;       // how far the drawn entity moved each frame
    std::vector<double> naive_steps; // the same if we just drew the newest snapshot
    double previous_x = 0.0;
    double previous_naive_x = 0.0;
    double newest_x = 0.0;

    for (double local_time = 0.0; local_time < 9.5; local_time += frame) {
        clock.advance(frame);
        while (next_arrival < arrivals.size() && arrivals[next_arrival].local_time <= local_time) {
            const Arrival& arrival = arrivals[next_arrival++];
            newest_x = arrival.server_time * speed;
            history.push(arrival.server_time, at_x(static_cast<float>(newest_x)));
            clock.observe(arrival.server_time);
        }
        Pose pose;
        if (!history.sample(clock.time(), pose)) {
            continue;
        }
        if (local_time > 1.0) { // skip the start-up
            steps.push_back(pose.position.x - previous_x);
            naive_steps.push_back(newest_x - previous_naive_x);
        }
        previous_x = pose.position.x;
        previous_naive_x = newest_x;
    }

    const double ideal = speed * frame; // 6.7 cm per frame
    const auto [smallest, largest] = std::minmax_element(steps.begin(), steps.end());
    const auto [naive_smallest, naive_largest] = std::minmax_element(naive_steps.begin(), naive_steps.end());

    // Interpolated: every frame moves forward by close to the ideal step.
    CHECK(*smallest > ideal * 0.7);
    CHECK(*largest < ideal * 1.3);
    // Drawing the newest snapshot instead: frames with no movement at all,
    // and frames that jump several steps at once.
    CHECK(*naive_smallest == 0.0);
    CHECK(*naive_largest > ideal * 3.0);
}
