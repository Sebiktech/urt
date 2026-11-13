#pragma once

#include "urt/ecs/system.hpp"

namespace urt {

// Moves every entity that has both a Transform and a Velocity.
class MovementSystem final : public System {
public:
    std::string_view name() const override { return "movement"; }
    void tick(World& world, const TickContext& context) override;
};

} // namespace urt
