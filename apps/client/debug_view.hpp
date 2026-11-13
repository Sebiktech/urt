#pragma once

// A throwaway window that shows the world as coloured boxes on a grid.
//
// It uses old fixed-function OpenGL because that takes a hundred lines and
// runs everywhere. It is NOT the renderer: the real one (Vulkan, step 4) will
// be a separate library, and this file gets deleted then. It lives in the
// client app, not in the engine, for the same reason.

#include "urt/ecs/world.hpp"
#include "urt/input/move_input.hpp"

#include <string>

struct GLFWwindow;

class DebugView {
public:
    DebugView() = default;
    ~DebugView();

    DebugView(const DebugView&) = delete;
    DebugView& operator=(const DebugView&) = delete;

    bool open(int width, int height, const char* title);
    bool should_close() const;

    // Processes window events and returns which movement keys are held.
    urt::MoveKeys poll_keys();

    // Draws every networked entity, with the camera following local_player.
    void draw(urt::World& world, urt::Entity local_player);

    void set_title(const std::string& title);

private:
    GLFWwindow* window_ = nullptr;
};
