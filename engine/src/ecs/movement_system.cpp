#include "urt/ecs/movement_system.hpp"

#include "urt/core/jobs.hpp"
#include "urt/ecs/components.hpp"
#include "urt/ecs/world.hpp"

namespace urt {

void MovementSystem::tick(World& world, const TickContext& context) {
    const auto dt = static_cast<float>(context.dt);
    auto view = world.registry.view<Transform, const Velocity>();

    // The view's "handle" is the smaller of the two component arrays. Its
    // entities sit in one packed array, so the array can be cut into index
    // ranges and each range handed to a different thread.
    const auto* handle = view.handle();
    if (handle == nullptr) {
        return;
    }
    const Entity* entities = handle->data();

    // Each entity is visited by exactly one thread and only its own Transform
    // is written, so no locking is needed. Nothing may create or destroy
    // entities or components while this runs.
    context.jobs.parallel_for(handle->size(), 1024, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            const Entity entity = entities[i];
            if (!view.contains(entity)) {
                continue; // has one of the components but not both
            }
            auto [transform, velocity] = view.get<Transform, const Velocity>(entity);
            transform.position += velocity.linear * dt;
            transform.yaw += velocity.yaw_rate * dt;
        }
    });
}

} // namespace urt
