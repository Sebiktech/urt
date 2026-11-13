#pragma once

// Components used by networking.

#include "urt/net/protocol.hpp"

#include <cstdint>

#include <glm/vec2.hpp>

namespace urt::net {

// Marks an entity that exists on both the server and the clients, and gives
// it the id both sides use to talk about it. Local ECS ids differ per machine,
// so they can never be sent over the network.
struct NetworkId {
    NetId value = 0;
};

// Server only: the newest input received from the player who controls the
// entity.
struct PlayerInput {
    glm::vec2 move{0.0f};           // x and z, length at most 1
    float yaw = 0.0f;
    std::uint64_t received_tick = 0; // server tick when it arrived
};

// Client only: tags the entity this client controls.
struct LocalPlayer {};

} // namespace urt::net
