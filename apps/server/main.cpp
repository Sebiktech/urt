// urt_server: the dedicated server. It owns the authoritative game state and
// has no window, so it runs on a plain Linux box.
//
// Step 1 only proves the core works: it spawns test entities and simulates
// them at a fixed tick. Networking arrives in step 2.

#include "urt/app/application.hpp"
#include "urt/core/args.hpp"
#include "urt/core/log.hpp"
#include "urt/ecs/components.hpp"
#include "urt/ecs/movement_system.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>

namespace {

void print_usage() {
    std::puts("urt_server " URT_VERSION "\n"
              "\n"
              "Options:\n"
              "  --tick-rate <hz>      simulation ticks per second (default 30)\n"
              "  --ticks <n>           stop after n ticks (default: run until Ctrl+C)\n"
              "  --entities <n>        number of test entities to spawn (default 100)\n"
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

// Logs where the first test entity is, so movement is visible in the output.
class ProbeSystem final : public urt::System {
public:
    ProbeSystem(urt::Entity entity, std::uint64_t every_ticks)
        : entity_(entity), every_ticks_(every_ticks) {}

    std::string_view name() const override { return "probe"; }

    void tick(urt::World& world, const urt::TickContext& context) override {
        if (context.tick % every_ticks_ != 0 || !world.registry.valid(entity_)) {
            return;
        }
        const urt::Transform& transform = world.registry.get<urt::Transform>(entity_);
        urt::log::debug("probe", "t={:.2f}s entity 0 at ({:.2f}, {:.2f}, {:.2f})", context.time,
                        transform.position.x, transform.position.y, transform.position.z);
    }

private:
    urt::Entity entity_;
    std::uint64_t every_ticks_;
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
    double entity_count = 0;
    double threads = 0;
    if (!read_count(args, "ticks", 0, 0, 1e15, max_ticks) ||
        !read_count(args, "entities", 100, 0, 10'000'000, entity_count) ||
        !read_count(args, "threads", 0, 0, 1024, threads)) {
        return 2;
    }

    urt::AppConfig config;
    config.name = "urt_server";
    config.role = urt::Role::Server;
    config.tick_rate = *tick_rate;
    config.max_ticks = static_cast<std::uint64_t>(max_ticks);
    config.worker_threads = static_cast<unsigned>(threads);

    urt::Application app(config);
    app.add_system<urt::MovementSystem>();

    // Test entities walking outwards in a fan. They stand in for players
    // until real ones can connect.
    auto& registry = app.world().registry;
    urt::Entity first = entt::null;
    const auto count = static_cast<std::size_t>(entity_count);
    for (std::size_t i = 0; i < count; ++i) {
        const float angle = static_cast<float>(i) * 0.1f;
        const urt::Entity entity = registry.create();
        registry.emplace<urt::Transform>(entity);
        registry.emplace<urt::Velocity>(
            entity, urt::Velocity{.linear = {std::cos(angle) * 1.4f, 0.0f, std::sin(angle) * 1.4f},
                                  .yaw_rate = 0.5f});
        if (i == 0) {
            first = entity;
        }
    }

    if (first != entt::null) {
        // Once per simulated second.
        const auto every = static_cast<std::uint64_t>(std::llround(*tick_rate));
        app.add_system<ProbeSystem>(first, every);
    }

    urt::log::info("server", "URT server {} starting with {} test entities", URT_VERSION, count);
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
