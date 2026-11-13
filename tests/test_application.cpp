#include <doctest/doctest.h>

#include "urt/app/application.hpp"
#include "urt/core/log.hpp"
#include "urt/core/platform.hpp"
#include "urt/ecs/components.hpp"
#include "urt/ecs/movement_system.hpp"

#include <cstdint>
#include <vector>

namespace {

urt::AppConfig quiet_config() {
    urt::AppConfig config;
    config.tick_rate = 50.0; // dt = 0.02 s
    config.stats_interval_seconds = 0.0;
    config.worker_threads = 3;
    return config;
}

// Records the ticks it sees, to check ordering and the tick context.
class RecordingSystem final : public urt::System {
public:
    explicit RecordingSystem(std::vector<int>& order, int id) : order_(order), id_(id) {}

    std::string_view name() const override { return "recording"; }

    void tick(urt::World&, const urt::TickContext& context) override {
        order_.push_back(id_);
        last_tick = context.tick;
        last_time = context.time;
        last_dt = context.dt;
    }

    std::uint64_t last_tick = 0;
    double last_time = 0.0;
    double last_dt = 0.0;

private:
    std::vector<int>& order_;
    int id_;
};

} // namespace

TEST_CASE("Application runs systems in order with a correct tick context") {
    urt::Application app(quiet_config());
    std::vector<int> order;
    auto& first = app.add_system<RecordingSystem>(order, 1);
    app.add_system<RecordingSystem>(order, 2);

    app.step(3);

    CHECK(app.tick() == 3);
    CHECK(order == std::vector<int>{1, 2, 1, 2, 1, 2});
    CHECK(first.last_tick == 2);
    CHECK(first.last_dt == doctest::Approx(0.02));
    CHECK(first.last_time == doctest::Approx(0.04));
}

TEST_CASE("Application::set_tick_rate changes the length of the following ticks") {
    urt::Application app(quiet_config()); // 50 ticks/s
    std::vector<int> order;
    auto& recorder = app.add_system<RecordingSystem>(order, 1);

    app.step();
    CHECK(recorder.last_dt == doctest::Approx(0.02));
    app.set_tick_rate(100.0);
    app.step();
    CHECK(recorder.last_dt == doctest::Approx(0.01));
    CHECK(app.config().tick_rate == 100.0);
}

TEST_CASE("MovementSystem integrates velocity over ticks") {
    urt::Application app(quiet_config());
    app.add_system<urt::MovementSystem>();
    auto& registry = app.world().registry;

    const urt::Entity mover = registry.create();
    registry.emplace<urt::Transform>(mover);
    registry.emplace<urt::Velocity>(mover, urt::Velocity{.linear = {2.0f, 0.0f, -1.0f}, .yaw_rate = 0.5f});

    const urt::Entity still = registry.create(); // no Velocity: must not move
    registry.emplace<urt::Transform>(still, urt::Transform{.position = {5.0f, 0.0f, 0.0f}});

    const urt::Entity ghost = registry.create(); // Velocity only: must be skipped
    registry.emplace<urt::Velocity>(ghost);

    app.step(50); // one simulated second

    const urt::Transform& moved = registry.get<urt::Transform>(mover);
    CHECK(moved.position.x == doctest::Approx(2.0f).epsilon(1e-4));
    CHECK(moved.position.y == doctest::Approx(0.0f));
    CHECK(moved.position.z == doctest::Approx(-1.0f).epsilon(1e-4));
    CHECK(moved.yaw == doctest::Approx(0.5f).epsilon(1e-4));
    CHECK(registry.get<urt::Transform>(still).position.x == doctest::Approx(5.0f));
}

TEST_CASE("MovementSystem gives the same result on one thread and on many") {
    // Enough entities for several parallel chunks.
    constexpr int count = 20'000;

    auto simulate = [](unsigned threads) {
        urt::AppConfig config = quiet_config();
        config.worker_threads = threads;
        urt::Application app(config);
        app.add_system<urt::MovementSystem>();
        auto& registry = app.world().registry;

        std::vector<urt::Entity> entities;
        for (int i = 0; i < count; ++i) {
            const urt::Entity entity = registry.create();
            registry.emplace<urt::Transform>(entity);
            if (i % 3 != 0) { // leave gaps so the two component arrays differ
                registry.emplace<urt::Velocity>(
                    entity, urt::Velocity{.linear = {static_cast<float>(i) * 0.001f, 0.0f, 1.0f}});
            }
            entities.push_back(entity);
        }
        app.step(10);

        std::vector<float> xs;
        for (const urt::Entity entity : entities) {
            xs.push_back(registry.get<urt::Transform>(entity).position.x);
        }
        return xs;
    };

    const std::vector<float> single = simulate(1);
    const std::vector<float> many = simulate(4);
    CHECK(single == many); // bit for bit: each entity is computed independently
    CHECK(single[1] == doctest::Approx(0.001f * 10 * 0.02f));
    CHECK(single[0] == 0.0f);
}

TEST_CASE("World counts living entities") {
    urt::World world;
    CHECK(world.entity_count() == 0);

    const urt::Entity a = world.registry.create();
    const urt::Entity b = world.registry.create();
    const urt::Entity c = world.registry.create();
    CHECK(world.entity_count() == 3);

    world.registry.destroy(a);
    CHECK(world.entity_count() == 2);
    CHECK(world.registry.valid(b));
    CHECK(world.registry.valid(c));
    CHECK_FALSE(world.registry.valid(a));
}

TEST_CASE("Application::run stops at max_ticks and on a shutdown request") {
    const urt::log::Level previous = urt::log::level();
    urt::log::set_level(urt::log::Level::Off);

    SUBCASE("max_ticks") {
        urt::AppConfig config = quiet_config();
        config.tick_rate = 500.0;
        config.max_ticks = 25;
        urt::Application app(config);
        CHECK(app.run() == 0);
        CHECK(app.tick() == 25);
    }

    SUBCASE("shutdown request") {
        urt::AppConfig config = quiet_config();
        config.tick_rate = 500.0;
        urt::Application app(config);

        // A system that asks the process to quit, as Ctrl+C would.
        struct Quitter final : urt::System {
            std::string_view name() const override { return "quitter"; }
            void tick(urt::World&, const urt::TickContext& context) override {
                if (context.tick == 9) {
                    urt::platform::request_shutdown();
                }
            }
        };
        app.add_system<Quitter>();

        CHECK(app.run() == 0);
        CHECK(app.tick() == 10);
        urt::platform::reset_shutdown_request();
    }

    urt::log::set_level(previous);
}
