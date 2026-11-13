#include <doctest/doctest.h>

#include "urt/net/movement.hpp"

#include <cmath>
#include <numbers>

using urt::net::apply_move;
using urt::net::MoveCommand;
using urt::net::sanitize_move;

TEST_CASE("sanitize_move limits the direction to length one") {
    const MoveCommand plain = sanitize_move(0.5f, -0.25f, 1.0f);
    CHECK(plain.move.x == 0.5f); // inside the limits: untouched
    CHECK(plain.move.y == -0.25f);
    CHECK(plain.yaw == 1.0f);

    const MoveCommand greedy = sanitize_move(5.0f, 0.0f, 0.0f);
    CHECK(greedy.move.x == 1.0f);
    CHECK(greedy.move.y == 0.0f);

    const MoveCommand diagonal = sanitize_move(1.0f, 1.0f, 0.0f);
    CHECK(std::hypot(diagonal.move.x, diagonal.move.y) == doctest::Approx(1.0f));

    const MoveCommand huge = sanitize_move(1.0e30f, -1.0e30f, 0.0f);
    CHECK(std::hypot(huge.move.x, huge.move.y) == doctest::Approx(1.0f));
    CHECK(huge.move.x > 0.0f);
    CHECK(huge.move.y < 0.0f);
}

TEST_CASE("sanitize_move folds the facing angle into -pi..pi and is stable") {
    const float pi = std::numbers::pi_v<float>;
    CHECK(sanitize_move(0.0f, 0.0f, 3.0f * pi).yaw == doctest::Approx(pi).epsilon(1e-4));
    CHECK(std::abs(sanitize_move(0.0f, 0.0f, 1.0e30f).yaw) <= pi + 1e-4f);

    // Cleaning an already clean command changes nothing. The client cleans
    // before predicting and the server cleans again on arrival; both must
    // end up simulating the same numbers.
    const MoveCommand once = sanitize_move(3.0f, -4.0f, 10.0f);
    const MoveCommand twice = sanitize_move(once.move.x, once.move.y, once.yaw);
    CHECK(twice.move.x == once.move.x);
    CHECK(twice.move.y == once.move.y);
    CHECK(twice.yaw == once.yaw);
}

TEST_CASE("apply_move advances by speed times tick length") {
    glm::vec3 position{1.0f, 0.0f, 2.0f};
    float yaw = 0.0f;
    apply_move(position, yaw, sanitize_move(1.0f, 0.0f, 0.7f), 4.0f, 0.5f);
    CHECK(position.x == doctest::Approx(3.0f));
    CHECK(position.y == 0.0f);
    CHECK(position.z == doctest::Approx(2.0f));
    CHECK(yaw == 0.7f);

    apply_move(position, yaw, MoveCommand{}, 4.0f, 0.5f); // no input: no movement
    CHECK(position.x == doctest::Approx(3.0f));
}
