#pragma once

// Decides what the client draws each frame: turns the networked world into
// boxes and places the camera. Separate from the window so that it can be
// tested without one.

#include "urt/ecs/world.hpp"
#include "urt/net/client.hpp"
#include "urt/net/interpolation.hpp"
#include "urt/render/renderer.hpp"

#include <cstdint>
#include <span>
#include <vector>

class WorldView {
public:
    // interpolation_delay: how far in the past the world is shown, in seconds.
    // 0.1 s is three snapshots at 30 per second, so one lost packet still
    // leaves two to blend between.
    explicit WorldView(double interpolation_delay = 0.1) : playback_(interpolation_delay) {}

    // Call once per frame, after the client's ticks for that frame have run.
    // tick_fraction (0..1) is how far this frame is between the last tick and
    // the next one.
    void update(double frame_seconds, float tick_fraction, const urt::net::Client& client, urt::World& world);

    std::span<const urt::render::Box> boxes() const { return boxes_; }
    const urt::render::Camera& camera() const { return camera_; }

private:
    urt::net::PlaybackClock playback_;
    std::uint64_t seen_snapshots_ = 0;
    std::vector<urt::render::Box> boxes_;
    urt::render::Camera camera_;
};
