#include "urt/ecs/world.hpp"

namespace urt {

std::size_t World::entity_count() const {
    std::size_t count = 0;
    for ([[maybe_unused]] const Entity entity : registry.view<Entity>()) {
        ++count;
    }
    return count;
}

} // namespace urt
