#pragma once

// How a player moves for one input. The server and the client both use
// exactly this code.
//
// That is what makes prediction work: the client applies its own input at
// once, without waiting for the server, and because both sides do the same
// arithmetic in the same order, the server arrives at the same position.

#include <algorithm>
#include <cmath>
#include <numbers>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace urt::net {

// One tick of player intent, cleaned up and safe to simulate.
struct MoveCommand {
    glm::vec2 move{0.0f}; // direction on x and z, length at most 1
    float yaw = 0.0f;     // facing, within -pi..pi
};

// Turns raw input numbers into a MoveCommand. The values must be finite (the
// packet decoder guarantees that); their size is enforced here. The client
// only states a direction: an input of (5, 0) is the same as (1, 0), so a
// modified client cannot walk faster.
inline MoveCommand sanitize_move(float move_x, float move_z, float yaw) {
    MoveCommand command;
    command.move = {std::clamp(move_x, -1.0f, 1.0f), std::clamp(move_z, -1.0f, 1.0f)};
    const float length = std::sqrt(command.move.x * command.move.x + command.move.y * command.move.y);
    if (length > 1.0f) {
        command.move /= length;
    }
    command.yaw = std::remainder(yaw, 2.0f * std::numbers::pi_v<float>);
    return command;
}

// The velocity a command asks for.
inline glm::vec3 move_velocity(const MoveCommand& command, float walk_speed) {
    return {command.move.x * walk_speed, 0.0f, command.move.y * walk_speed};
}

// Advances a position and facing by one tick of a command. On the server the
// same happens in two steps: the command sets the velocity and
// MovementSystem adds velocity * dt.
inline void apply_move(glm::vec3& position, float& yaw, const MoveCommand& command, float walk_speed,
                       float tick_seconds) {
    position += move_velocity(command, walk_speed) * tick_seconds;
    yaw = command.yaw;
}

} // namespace urt::net
