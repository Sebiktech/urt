// urt_server: the dedicated server. It owns the authoritative game state and
// has no window, so it runs on a plain Linux box.
//
// Step 2: players can connect, walk around and see each other. A few NPCs
// walk in circles so a single client has something to look at.

#include "urt/app/application.hpp"
#include "urt/core/args.hpp"
#include "urt/core/log.hpp"
#include "urt/ecs/components.hpp"
#include "urt/ecs/movement_system.hpp"
#include "urt/net/server.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <string_view>

namespace {

void print_usage() {
    std::puts("urt_server " URT_VERSION "\n"
              "\n"
              "Options:\n"
              "  --port <n>            UDP port to listen on, 0 = any free port (default 30777)\n"
              "  --max-players <n>     player limit (default 64)\n"
              "  --npcs <n>            walking test characters to spawn (default 8)\n"
              "  --tick-rate <hz>      simulation ticks per second (default 30)\n"
              "  --ticks <n>           stop after n ticks (default: run until Ctrl+C)\n"
              "  --threads <n>         worker threads (default: from hardware)\n"
              "  --log-level <level>   trace, debug, info, warn, error, off (default info)\n"
              "  --help                show this text");
}

// Reads a whole number option. Returns false (after logging) if it is invalid.
bool read_count(const urt::Args& args, const char* name, double fallback, double min, double max,
                double& out) {
    const std::optional<double> value = args.number(name, fallback);
    if (!value || *value < min || *value > max || *value != std::floor(*value)) {
        urt::log::error("server", "--{} needs a whole number between {} and {}", name, min, max);
        return false;
    }
    out = *value;
    return true;
}

// Walks in a circle: forward along its own facing direction while turning.
// Placeholder behaviour until NPCs are scripted in Lua.
struct Patrol {
    float speed = 1.4f; // metres per second
};

class PatrolSystem final : public urt::System {
public:
    std::string_view name() const override { return "patrol"; }

    void tick(urt::World& world, const urt::TickContext&) override {
        auto view = world.registry.view<const Patrol, const urt::Transform, urt::Velocity>();
        for (auto [entity, patrol, transform, velocity] : view.each()) {
            velocity.linear = {std::cos(transform.yaw) * patrol.speed, 0.0f,
                               std::sin(transform.yaw) * patrol.speed};
        }
    }
};

int run_server(const urt::Args& args) {
    if (args.has("help")) {
        print_usage();
        return 0;
    }

    if (const auto text = args.value("log-level")) {
        urt::log::Level level{};
        if (!urt::log::parse_level(*text, level)) {
            urt::log::error("server", "unknown log level '{}'", *text);
            return 2;
        }
        urt::log::set_level(level);
    } else if (args.has("log-level")) {
        urt::log::error("server", "--log-level needs a value");
        return 2;
    }

    const std::optional<double> tick_rate = args.number("tick-rate", 30.0);
    if (!tick_rate || *tick_rate < 1.0 || *tick_rate > 1000.0) {
        urt::log::error("server", "--tick-rate needs a number between 1 and 1000");
        return 2;
    }

    double max_ticks = 0;
    double npc_count = 0;
    double threads = 0;
    double port = 0;
    double max_players = 0;
    if (!read_count(args, "ticks", 0, 0, 1e15, max_ticks) ||
        !read_count(args, "npcs", 8, 0, 100'000, npc_count) ||
        !read_count(args, "threads", 0, 0, 1024, threads) ||
        !read_count(args, "port", urt::net::default_port, 0, 65535, port) ||
        !read_count(args, "max-players", 64, 1, 4000, max_players)) {
        return 2;
    }

    urt::AppConfig config;
    config.name = "urt_server";
    config.role = urt::Role::Server;
    config.tick_rate = *tick_rate;
    config.max_ticks = static_cast<std::uint64_t>(max_ticks);
    config.worker_threads = static_cast<unsigned>(threads);

    urt::net::ServerConfig net_config;
    net_config.port = static_cast<std::uint16_t>(port);
    net_config.max_players = static_cast<std::size_t>(max_players);

    urt::Application app(config);

    // Order matters: read the network, decide, move, then send the result.
    auto& server = app.add_system<urt::net::Server>(net_config);
    app.add_system<PatrolSystem>();
    app.add_system<urt::MovementSystem>();
    app.add_system<urt::net::ServerSnapshotSystem>(server);

    if (!server.start()) {
        return 1;
    }

    // NPCs on circles of different sizes around the origin.
    auto& registry = app.world().registry;
    const auto npcs = static_cast<std::size_t>(npc_count);
    for (std::size_t i = 0; i < npcs; ++i) {
        const float angle = static_cast<float>(i) * 2.4f;
        const float distance = 6.0f + 2.0f * static_cast<float>(i % 16);
        const urt::Entity entity = registry.create();
        registry.emplace<urt::Transform>(
            entity, urt::Transform{.position = {std::cos(angle) * distance, 0.0f, std::sin(angle) * distance},
                                   .yaw = angle});
        registry.emplace<urt::Velocity>(entity, urt::Velocity{.yaw_rate = 0.3f});
        registry.emplace<Patrol>(entity);
        server.make_networked(app.world(), entity);
    }

    urt::log::info("server", "URT server {} started with {} NPCs", URT_VERSION, npcs);
    return app.run();
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run_server(urt::Args(argc, argv));
    } catch (const std::exception& e) {
        urt::log::error("server", "fatal: {}", e.what());
        return 1;
    }
}
