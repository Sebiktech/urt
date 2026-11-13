#include <doctest/doctest.h>

#include "urt/input/move_input.hpp"

#include <cmath>
#include <numbers>

using urt::MoveIntent;
using urt::MoveKeys;
using urt::to_intent;

TEST_CASE("movement keys become a unit direction and a facing angle") {
    const float pi = std::numbers::pi_v<float>;

    const MoveIntent right = to_intent(MoveKeys{.right = true}, 0.0f);
    CHECK(right.x == 1.0f);
    CHECK(right.z == 0.0f);
    CHECK(right.yaw == doctest::Approx(0.0f));

    const MoveIntent forward = to_intent(MoveKeys{.forward = true}, 0.0f);
    CHECK(forward.x == 0.0f);
    CHECK(forward.z == -1.0f); // away from the camera
    CHECK(forward.yaw == doctest::Approx(-pi / 2.0f));

    // Diagonal movement is not faster than straight movement.
    const MoveIntent diagonal = to_intent(MoveKeys{.back = true, .left = true}, 0.0f);
    CHECK(std::hypot(diagonal.x, diagonal.z) == doctest::Approx(1.0f));
    CHECK(diagonal.x < 0.0f);
    CHECK(diagonal.z > 0.0f);
    // The facing matches the server's convention: forward = (cos yaw, sin yaw).
    CHECK(std::cos(diagonal.yaw) == doctest::Approx(diagonal.x));
    CHECK(std::sin(diagonal.yaw) == doctest::Approx(diagonal.z));
}

TEST_CASE("standing still keeps the previous facing") {
    const MoveIntent idle = to_intent(MoveKeys{}, 1.25f);
    CHECK(idle.x == 0.0f);
    CHECK(idle.z == 0.0f);
    CHECK(idle.yaw == 1.25f);

    // Opposite keys cancel out.
    const MoveIntent cancelled = to_intent(MoveKeys{.forward = true, .back = true, .left = true, .right = true}, -2.0f);
    CHECK(cancelled.x == 0.0f);
    CHECK(cancelled.z == 0.0f);
    CHECK(cancelled.yaw == -2.0f);
}
