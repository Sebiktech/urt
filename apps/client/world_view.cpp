#include "world_view.hpp"

#include "urt/render/shapes.hpp"

void WorldView::update(double frame_seconds, float tick_fraction, const urt::net::Client& client,
                       urt::World& world) {
    playback_.advance(frame_seconds);
    if (client.snapshots_received() != seen_snapshots_) {
        seen_snapshots_ = client.snapshots_received();
        playback_.observe(client.latest_server_time());
    }

    // Other entities are drawn where they were at the playback time, a moment
    // in the past, blended between two snapshots. Our own player is drawn
    // where prediction says it is right now, so it reacts at once.
    boxes_.clear();
    const urt::Entity me = client.player_entity();
    for (auto [entity, history] : world.registry.view<const urt::net::PoseHistory>().each()) {
        urt::net::Pose pose;
        if (!history.sample(playback_.time(), pose)) {
            continue;
        }
        const bool is_me = entity == me;
        if (is_me) {
            client.predicted_pose(tick_fraction, pose); // keeps the history pose if not ready
        }
        urt::render::add_character(boxes_, pose.position, pose.yaw,
                                   is_me ? urt::render::local_player_color : urt::render::other_entity_color);
        if (is_me) {
            // The camera sits above and behind the player. "Behind" is +Z,
            // so up on the screen is -Z: that is where W walks.
            camera_.target = pose.position;
            camera_.eye = pose.position + glm::vec3{0.0f, 14.0f, 12.0f};
        }
    }
}
