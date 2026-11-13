// Renders a test scene without a window and checks the picture.
//
// It uses the VK_EXT_headless_surface extension: a drawing surface that is
// not connected to any screen. Software drivers (Mesa lavapipe, SwiftShader)
// have it; most real graphics drivers do not, in which case the test reports
// "skipped" (exit code 77).
//
//   urt_render_headless [output.ppm]

#include "picture_check.hpp"

#include "urt/core/log.hpp"
#include "urt/render/renderer.hpp"
#include "urt/render/shapes.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using urt::render::Box;

bool check(bool condition, const char* what) {
    if (!condition) {
        urt::log::error("test", "FAILED: {}", what);
    }
    return condition;
}

} // namespace

int main(int argc, char** argv) {
    const std::string output = argc > 1 ? argv[1] : "urt_render_headless.ppm";

    // Is a headless surface available at all?
    std::uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
    bool has_headless = false;
    for (const VkExtensionProperties& extension : extensions) {
        has_headless = has_headless || std::strcmp(extension.extensionName, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME) == 0;
    }
    if (!has_headless) {
        std::puts("skipped: this Vulkan driver has no headless surface");
        return 77;
    }

    int width = 960;
    int height = 540;

    urt::render::SurfaceProvider provider;
    provider.instance_extensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
    provider.create_surface = [](VkInstance instance) -> VkSurfaceKHR {
        const auto create = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateHeadlessSurfaceEXT"));
        VkHeadlessSurfaceCreateInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (create == nullptr || create(instance, &info, nullptr, &surface) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
        return surface;
    };
    provider.framebuffer_size = [&](int& w, int& h) {
        w = width;
        h = height;
    };

    urt::render::Renderer renderer;
    if (!renderer.init(provider, {.validation = true})) {
        return 1;
    }

    // The scene the client shows: us in the middle, others around.
    std::vector<Box> boxes;
    urt::render::add_character(boxes, {0.0f, 0.0f, 0.0f}, 0.0f, urt::render::local_player_color);
    urt::render::add_character(boxes, {4.0f, 0.0f, -3.0f}, 1.0f, urt::render::other_entity_color);
    urt::render::add_character(boxes, {-5.0f, 0.0f, 1.0f}, 2.5f, urt::render::other_entity_color);
    urt::render::add_character(boxes, {2.0f, 0.0f, 5.0f}, -1.5f, urt::render::other_entity_color);
    urt::render::add_character(boxes, {-3.0f, 0.0f, -6.0f}, 4.0f, urt::render::other_entity_color);
    const urt::render::Camera camera;

    bool ok = true;

    // A few ordinary frames, then one that is saved.
    for (int i = 0; i < 3; ++i) {
        ok = check(renderer.draw_frame(camera, boxes), "draw_frame") && ok;
    }
    renderer.request_screenshot(output);
    ok = check(renderer.draw_frame(camera, boxes), "draw_frame with screenshot") && ok;

    Image image;
    if (check(read_ppm(output, image), "the screenshot can be read back")) {
        const Counts counts = classify(image);
        const int total = image.width * image.height;
        urt::log::info("test", "{}x{}: orange {}, blue {}, ground {}, grid {} of {} pixels", image.width,
                       image.height, counts.orange, counts.blue, counts.ground, counts.grid, total);
        ok = check(image.width == 960 && image.height == 540, "the screenshot has the surface's size") && ok;
        ok = check(counts.orange > 300, "the local player is visible") && ok;
        ok = check(counts.blue > 3 * 300, "the other entities are visible") && ok;
        ok = check(counts.blue > counts.orange * 2, "there is more blue than orange (4 others, 1 player)") && ok;
        ok = check(counts.ground > total / 2, "most of the picture is ground") && ok;
        ok = check(counts.grid > 1000, "the grid lines are visible") && ok;
    } else {
        ok = false;
    }

    // Resize, as when the user drags the window border.
    width = 640;
    height = 360;
    const std::string resized = output + ".resized.ppm";
    ok = check(renderer.draw_frame(camera, boxes), "draw_frame after resize") && ok;
    renderer.request_screenshot(resized);
    ok = check(renderer.draw_frame(camera, boxes), "draw_frame with screenshot after resize") && ok;
    Image small;
    ok = check(read_ppm(resized, small) && small.width == 640 && small.height == 360,
               "the swapchain follows the new size") && ok;

    // Minimise and restore.
    width = 0;
    height = 0;
    ok = check(renderer.draw_frame(camera, boxes), "draw_frame while minimised") && ok;
    width = 800;
    height = 600;
    for (int i = 0; i < 3; ++i) {
        ok = check(renderer.draw_frame(camera, boxes), "draw_frame after restore") && ok;
    }

    // No boxes at all is a valid scene too.
    ok = check(renderer.draw_frame(camera, {}), "draw_frame with an empty scene") && ok;

    if (renderer.validation_active()) {
        ok = check(renderer.validation_error_count() == 0, "the validation layer reported no errors") && ok;
    } else {
        urt::log::info("test", "validation layer not installed; Vulkan usage was not checked");
    }

    std::puts(ok ? "render test passed" : "render test FAILED");
    return ok ? 0 : 1;
}
