#pragma once

// Helpers that turn game things into boxes for the renderer.

#include "urt/render/renderer.hpp"

#include <cmath>
#include <vector>

namespace urt::render {

// A character: a tall body plus a small white "nose" at head height that
// shows which way it faces. `position` is where its feet are.
inline void add_character(std::vector<Box>& boxes, const glm::vec3& position, float yaw,
                          const glm::vec3& color) {
    boxes.push_back(Box{.position = position, .yaw = yaw, .size = {0.6f, 1.8f, 0.6f}, .color = color});

    const glm::vec3 forward{std::cos(yaw), 0.0f, std::sin(yaw)};
    boxes.push_back(Box{.position = position + forward * 0.38f + glm::vec3{0.0f, 1.3f, 0.0f},
                        .yaw = yaw,
                        .size = {0.2f, 0.2f, 0.2f},
                        .color = {0.95f, 0.95f, 0.95f}});
}

inline constexpr glm::vec3 local_player_color{1.0f, 0.6f, 0.2f}; // orange
inline constexpr glm::vec3 other_entity_color{0.3f, 0.6f, 1.0f}; // blue

} // namespace urt::render
