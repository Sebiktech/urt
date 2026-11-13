#pragma once

// Smooth motion between snapshots.
//
// Snapshots arrive 30 times a second and not perfectly evenly; the screen
// refreshes 60 times or more. Showing each snapshot as it arrives makes
// everything move in small jerks. Instead the client shows the world a short
// moment in the past (the "interpolation delay"). For that moment it almost
// always knows a snapshot before and one after, and can blend between them.

#include <array>
#include <cstddef>

#include <glm/vec3.hpp>

namespace urt::net {

struct Pose {
    glm::vec3 position{0.0f};
    float yaw = 0.0f;
};

// The last few known poses of one entity, each with the server time it was
// true at. A component: the client attaches one to every replicated entity.
class PoseHistory {
public:
    static constexpr std::size_t capacity = 8;

    // Adds a pose. Times must grow; an older or equal time is ignored.
    void push(double server_time, const Pose& pose);

    // The pose at `server_time`, blended between the two surrounding
    // samples. Before the oldest sample it returns the oldest, after the
    // newest the newest: it never guesses beyond what the server said.
    // Returns false if nothing has been pushed yet.
    bool sample(double server_time, Pose& out) const;

    std::size_t size() const { return count_; }

private:
    struct Entry {
        double time = 0.0;
        Pose pose;
    };

    const Entry& at(std::size_t age) const; // 0 = newest

    std::array<Entry, capacity> entries_{};
    std::size_t next_ = 0;  // where the next entry goes
    std::size_t count_ = 0;
};

// Shortest way from angle `from` to angle `to`, blended by t in 0..1.
// Turning from 350 to 10 degrees goes through 0, not back through 180.
float lerp_angle(float from, float to, float t);

// The moment in server time the client is currently showing.
//
// It runs on the local clock and is steered gently towards "newest snapshot
// minus delay". Following each snapshot's time directly would pass network
// jitter straight into the picture.
class PlaybackClock {
public:
    explicit PlaybackClock(double delay_seconds) : delay_(delay_seconds) {}

    // Call once per frame with the real time since the last frame.
    void advance(double seconds);

    // Call when a snapshot with this server time has arrived.
    void observe(double newest_server_time);

    double time() const { return time_; }
    double delay() const { return delay_; }

private:
    double delay_;
    double time_ = 0.0;
    bool started_ = false;
};

} // namespace urt::net
