#pragma once

// Turns pressed movement keys into what the client sends to the server.
// Kept free of any window or keyboard library so it can be tested.

#include <cmath>

namespace urt {

struct MoveKeys {
    bool forward = false; // W: away from the camera, towards -Z
    bool back = false;    // S: towards +Z
    bool left = false;    // A: towards -X
    bool right = false;   // D: towards +X
};

struct MoveIntent {
    float x = 0.0f; // world-space direction, length 0 or 1
    float z = 0.0f;
    float yaw = 0.0f; // facing: forward is (cos yaw, sin yaw) on the XZ plane
};

// previous_yaw is kept while standing still, so the character does not snap
// back to a default direction when the keys are released.
inline MoveIntent to_intent(const MoveKeys& keys, float previous_yaw) {
    MoveIntent intent;
    intent.x = (keys.right ? 1.0f : 0.0f) - (keys.left ? 1.0f : 0.0f);
    intent.z = (keys.back ? 1.0f : 0.0f) - (keys.forward ? 1.0f : 0.0f);
    intent.yaw = previous_yaw;

    const float length = std::sqrt(intent.x * intent.x + intent.z * intent.z);
    if (length > 0.0f) {
        // Without this, walking diagonally would be 41 % faster.
        intent.x /= length;
        intent.z /= length;
        intent.yaw = std::atan2(intent.z, intent.x);
    }
    return intent;
}

} // namespace urt
