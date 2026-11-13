#pragma once

// Components used by networking.

#include "urt/net/protocol.hpp"


namespace urt::net {

// Marks an entity that exists on both the server and the clients, and gives
// it the id both sides use to talk about it. Local ECS ids differ per machine,
// so they can never be sent over the network.
struct NetworkId {
    NetId value = 0;
};

// Client only: tags the entity this client controls.
struct LocalPlayer {};

} // namespace urt::net
