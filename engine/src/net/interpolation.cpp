#include "urt/net/interpolation.hpp"

#include <cmath>
#include <numbers>

namespace urt::net {

const PoseHistory::Entry& PoseHistory::at(std::size_t age) const {
    // next_ points one past the newest entry; step back and wrap around.
    return entries_[(next_ + capacity - 1 - age) % capacity];
}

void PoseHistory::push(double server_time, const Pose& pose) {
    if (count_ > 0 && server_time <= at(0).time) {
        return;
    }
    entries_[next_] = Entry{server_time, pose};
    next_ = (next_ + 1) % capacity; // when full, this overwrites the oldest
    if (count_ < capacity) {
        ++count_;
    }
}

bool PoseHistory::sample(double server_time, Pose& out) const {
    if (count_ == 0) {
        return false;
    }
    if (server_time >= at(0).time) {
        out = at(0).pose;
        return true;
    }
    // Walk from newest to oldest until the wanted time lies between two entries.
    for (std::size_t age = 1; age < count_; ++age) {
        const Entry& older = at(age);
        if (server_time >= older.time) {
            const Entry& newer = at(age - 1);
            const auto t = static_cast<float>((server_time - older.time) / (newer.time - older.time));
            out.position = older.pose.position + (newer.pose.position - older.pose.position) * t;
            out.yaw = lerp_angle(older.pose.yaw, newer.pose.yaw, t);
            return true;
        }
    }
    out = at(count_ - 1).pose;
    return true;
}

float lerp_angle(float from, float to, float t) {
    constexpr float two_pi = 2.0f * std::numbers::pi_v<float>;
    // remainder() folds the difference into -pi..pi: the short way round.
    const float difference = std::remainder(to - from, two_pi);
    return from + difference * t;
}

void PlaybackClock::advance(double seconds) {
    if (started_ && seconds > 0.0) {
        time_ += seconds;
    }
}

void PlaybackClock::observe(double newest_server_time) {
    const double target = newest_server_time - delay_;
    const double error = target - time_;
    if (!started_ || std::abs(error) > 0.5) {
        // First snapshot, or we fell badly out of step (a long stall): jump.
        time_ = target;
        started_ = true;
        return;
    }
    // Otherwise close a tenth of the gap per snapshot. Small enough that
    // jitter is not visible, large enough to follow real clock drift.
    time_ += error * 0.1;
}

} // namespace urt::net
