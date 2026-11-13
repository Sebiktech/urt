#include "game_window.hpp"

#include "urt/core/log.hpp"

#include <cstdint>

// Make GLFW's header declare its Vulkan functions and skip the OpenGL header.
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

namespace {

void on_glfw_error(int code, const char* description) {
    urt::log::error("window", "GLFW error {}: {}", code, description);
}

} // namespace

GameWindow::~GameWindow() {
    if (window_ != nullptr) {
        glfwDestroyWindow(window_);
        glfwTerminate();
    }
}

bool GameWindow::open(int width, int height, const char* title) {
    glfwSetErrorCallback(on_glfw_error);
    if (glfwInit() == GLFW_FALSE) {
        return false;
    }
    if (glfwVulkanSupported() == GLFW_FALSE) {
        urt::log::error("window", "no Vulkan driver found on this machine");
        glfwTerminate();
        return false;
    }
    // GLFW creates an OpenGL context by default; we draw with Vulkan.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window_ = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (window_ == nullptr) {
        glfwTerminate();
        return false;
    }
    return true;
}

bool GameWindow::should_close() const {
    return window_ == nullptr || glfwWindowShouldClose(window_) == GLFW_TRUE;
}

bool GameWindow::is_minimised() const {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window_, &width, &height);
    return width <= 0 || height <= 0;
}

urt::MoveKeys GameWindow::poll_keys() {
    glfwPollEvents();
    const auto down = [this](int key) { return glfwGetKey(window_, key) == GLFW_PRESS; };
    if (down(GLFW_KEY_ESCAPE)) {
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }
    urt::MoveKeys keys;
    keys.forward = down(GLFW_KEY_W) || down(GLFW_KEY_UP);
    keys.back = down(GLFW_KEY_S) || down(GLFW_KEY_DOWN);
    keys.left = down(GLFW_KEY_A) || down(GLFW_KEY_LEFT);
    keys.right = down(GLFW_KEY_D) || down(GLFW_KEY_RIGHT);
    return keys;
}

bool GameWindow::screenshot_pressed() {
    const bool down = glfwGetKey(window_, GLFW_KEY_F12) == GLFW_PRESS;
    const bool pressed = down && !f12_was_down_;
    f12_was_down_ = down;
    return pressed;
}

void GameWindow::set_title(const std::string& title) { glfwSetWindowTitle(window_, title.c_str()); }

urt::render::SurfaceProvider GameWindow::surface_provider() const {
    urt::render::SurfaceProvider provider;

    std::uint32_t count = 0;
    const char** extensions = glfwGetRequiredInstanceExtensions(&count);
    if (extensions != nullptr) {
        provider.instance_extensions.assign(extensions, extensions + count);
    }

    GLFWwindow* const window = window_;
    provider.create_surface = [window](VkInstance instance) -> VkSurfaceKHR {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
        return surface;
    };
    provider.framebuffer_size = [window](int& width, int& height) {
        glfwGetFramebufferSize(window, &width, &height);
    };
    return provider;
}
