#pragma once

// The client's window: creates it, reads the keyboard, and tells the renderer
// how to get a drawing surface. The only file that talks to GLFW.

#include "urt/input/move_input.hpp"
#include "urt/render/renderer.hpp"

#include <string>

struct GLFWwindow;

class GameWindow {
public:
    GameWindow() = default;
    ~GameWindow();

    GameWindow(const GameWindow&) = delete;
    GameWindow& operator=(const GameWindow&) = delete;

    bool open(int width, int height, const char* title);
    bool should_close() const;
    bool is_minimised() const;

    // Processes window events and returns which movement keys are held.
    urt::MoveKeys poll_keys();

    // True once per press of F12.
    bool screenshot_pressed();

    void set_title(const std::string& title);

    // What the renderer needs from the window. The window must outlive the
    // renderer that was initialised with it.
    urt::render::SurfaceProvider surface_provider() const;

private:
    GLFWwindow* window_ = nullptr;
    bool f12_was_down_ = false;
};
