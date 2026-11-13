// The whole chain without a window: a real server, a real client connected
// over loopback, the client's WorldView and the Vulkan renderer drawing to a
// headless surface. Checks that the player ends up in the middle of the
// picture with the NPCs around. Skipped (exit code 77) on drivers without
// VK_EXT_headless_surface.
//
//   urt_client_render_headless [output.ppm]

#include "picture_check.hpp"
#include "world_view.hpp"

#include "urt/app/application.hpp"
#include "urt/core/log.hpp"
#include "urt/ecs/components.hpp"
#include "urt/ecs/movement_system.hpp"
#include "urt/net/client.hpp"
#include "urt/net/server.hpp"
#include "urt/render/renderer.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

bool check(bool condition, const char* what) {
    if (!condition) {
        urt::log::error("test", "FAILED: {}", what);
    }
    return condition;
}

} // namespace

int main(int argc, char** argv) {
    const std::string output = argc > 1 ? argv[1] : "urt_client_render_headless.ppm";

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

    constexpr double tick_rate = 30.0;
    urt::AppConfig config;
    config.tick_rate = tick_rate;
    config.stats_interval_seconds = 0.0;
    config.worker_threads = 1;

    // Server with six NPCs on a ring; every second one walks.
    urt::Application server_app(config);
    auto& server = server_app.add_system<urt::net::Server>(urt::net::ServerConfig{.port = 0});
    server_app.add_system<urt::MovementSystem>();
    server_app.add_system<urt::net::ServerSnapshotSystem>(server);
    if (!server.start()) {
        return 1;
    }
    for (int i = 0; i < 6; ++i) {
        const float angle = static_cast<float>(i) * 1.047f;
        auto& registry = server_app.world().registry;
        const urt::Entity npc = registry.create();
        registry.emplace<urt::Transform>(
            npc, urt::Transform{.position = {std::cos(angle) * 6.0f, 0.0f, std::sin(angle) * 6.0f}, .yaw = angle});
        registry.emplace<urt::Velocity>(npc, urt::Velocity{.linear = {i % 2 == 0 ? 0.5f : 0.0f, 0.0f, 0.0f}});
        server.make_networked(server_app.world(), npc);
    }

    urt::Application client_app(config);
    auto& client = client_app.add_system<urt::net::Client>(
        urt::net::ClientConfig{.host = "127.0.0.1", .port = server.port(), .name = "Tester"});
    if (!client.connect()) {
        return 1;
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

    // The game loop: one tick and one frame per round, the player walking
    // to the right. Runs until the client has been in the world for 3 s.
    WorldView view;
    bool ok = true;
    int frames_in_world = 0;
    for (int round = 0; round < 3000 && frames_in_world < 90; ++round) {
        client.set_input(1.0f, 0.0f, 0.0f);
        server_app.step();
        client_app.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        view.update(1.0 / tick_rate, 1.0f, client, client_app.world());
        ok = check(renderer.draw_frame(view.camera(), view.boxes()), "draw_frame") && ok;
        if (client.player_entity() != entt::null) {
            ++frames_in_world;
        }
    }
    ok = check(frames_in_world == 90, "the client joined the world") && ok;
    ok = check(client.visible_count() == 7, "the client sees itself and six NPCs") && ok;
    // Two boxes (body and nose) for each of the seven characters.
    ok = check(view.boxes().size() == 14, "the view produced two boxes per character") && ok;

    // The player walked right at 4 m/s for close to 3 s, and the camera followed.
    const float player_x = view.camera().target.x;
    urt::log::info("test", "player drawn at x = {:.2f} m", player_x);
    ok = check(player_x > 8.0f && player_x < 14.0f, "the drawn player moved with the server") && ok;

    renderer.request_screenshot(output);
    ok = check(renderer.draw_frame(view.camera(), view.boxes()), "draw_frame with screenshot") && ok;

    Image image;
    if (check(read_ppm(output, image), "the screenshot can be read back")) {
        const Counts counts = classify(image);
        urt::log::info("test", "orange {} px centred at ({:.0f}, {:.0f}), blue {} px, grid {} px", counts.orange,
                       counts.orange_x, counts.orange_y, counts.blue, counts.grid);
        ok = check(counts.orange > 300, "the player is visible") && ok;
        // The camera looks at the player's feet, so the body is at the
        // centre horizontally and a little above it vertically.
        ok = check(std::abs(counts.orange_x - image.width / 2.0) < image.width * 0.03,
                   "the player is horizontally centred") && ok;
        ok = check(counts.orange_y < image.height / 2.0 && counts.orange_y > image.height * 0.35,
                   "the player stands just above the centre") && ok;
        ok = check(counts.blue > 1000, "NPCs are visible") && ok;
        ok = check(counts.grid > 1000, "the grid is visible") && ok;
    } else {
        ok = false;
    }

    if (renderer.validation_active()) {
        ok = check(renderer.validation_error_count() == 0, "the validation layer reported no errors") && ok;
    }

    std::puts(ok ? "client render test passed" : "client render test FAILED");
    return ok ? 0 : 1;
}
