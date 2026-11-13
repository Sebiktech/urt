#pragma once

// urt_render: the Vulkan renderer.
//
// It knows nothing about windows, the network or the ECS. Whoever owns the
// window hands over a SurfaceProvider; whoever owns the game hands over a
// camera and a list of things to draw each frame. That keeps the renderer
// testable without a window and keeps graphics out of urt_engine, which the
// server links too.
//
// For now it draws coloured boxes on a ground grid. Meshes, materials and
// lighting come in step 4 and replace DrawList, not this interface's shape.

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <glm/vec3.hpp>
#include <vulkan/vulkan.h>

namespace urt::render {

// How the renderer gets its drawing surface from the window system.
struct SurfaceProvider {
    // Instance extensions the window system needs (GLFW reports them).
    std::vector<const char*> instance_extensions;

    // Creates the surface. Returns VK_NULL_HANDLE on failure.
    std::function<VkSurfaceKHR(VkInstance)> create_surface;

    // Current size of the drawable area in pixels. 0 x 0 while minimised.
    std::function<void(int& width, int& height)> framebuffer_size;
};

struct RendererConfig {
    // Turn on the Vulkan validation layer if it is installed. It checks every
    // call against the specification and reports mistakes through the log.
    bool validation = false;
};

struct Camera {
    glm::vec3 eye{0.0f, 14.0f, 12.0f};
    glm::vec3 target{0.0f};
    float fov_degrees = 50.0f;
};

// A box standing on the ground: `position` is the centre of its base.
struct Box {
    glm::vec3 position{0.0f};
    float yaw = 0.0f;                  // forward is (cos yaw, sin yaw) on XZ
    glm::vec3 size{0.6f, 1.8f, 0.6f};  // width (x), height (y), depth (z)
    glm::vec3 color{1.0f};
};

class Renderer {
public:
    Renderer();
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Sets everything up. Returns false (after logging why) on failure.
    bool init(const SurfaceProvider& surface, const RendererConfig& config = {});

    // Draws one frame and shows it. Returns false only on an unrecoverable
    // error; a minimised or resizing window is not an error.
    bool draw_frame(const Camera& camera, std::span<const Box> boxes);

    // Saves the next drawn frame as a binary PPM image.
    void request_screenshot(std::string path);

    // Problems the validation layer has reported so far. Always 0 without it.
    std::uint32_t validation_error_count() const;

    // True if the validation layer was requested and found.
    bool validation_active() const;

    // Name of the GPU in use, for the log.
    const std::string& device_name() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace urt::render
