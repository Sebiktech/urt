#pragma once

// Components shared by the server and the client.
// Units: metres, seconds, radians. Y is up.

#include <glm/vec3.hpp>

namespace urt {

struct Transform {
    glm::vec3 position{0.0f};
    float yaw = 0.0f; // rotation around the Y axis
};

struct Velocity {
    glm::vec3 linear{0.0f}; // metres per second
    float yaw_rate = 0.0f;  // radians per second
};

} // namespace urt
