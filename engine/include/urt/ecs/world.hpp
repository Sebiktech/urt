#pragma once

// The world: every entity and component of one running game.
//
// An entity is just an id. Data lives in components (plain structs) attached
// to entities, and behaviour lives in systems that loop over components.
// EnTT stores each component type in its own tightly packed array, which is
// what makes those loops fast.

#include <cstddef>

#include <entt/entity/registry.hpp>

namespace urt {

using Entity = entt::entity;

struct World {
    entt::registry registry;

    // Number of living entities. Walks all of them, so use it for stats and
    // tests, not every tick.
    std::size_t entity_count() const;
};

} // namespace urt
