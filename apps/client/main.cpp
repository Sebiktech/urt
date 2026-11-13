// urt_client: the game client. By default it opens a debug window and you
// walk with WASD or the arrow keys. With --headless (or when built without
// the window) it plays by itself: it walks in a circle and prints what it sees.

#include "urt/app/application.hpp"
#include "urt/core/args.hpp"
#include "urt/core/log.hpp"
#include "urt/core/platform.hpp"
#include "urt/core/time.hpp"
#include "urt/ecs/components.hpp"
#include "urt/net/client.hpp"
#include "urt/net/components.hpp"

#if defined(URT_CLIENT_WINDOW)
#include "debug_view.hpp"
#endif

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace {

void print_usage() {
    std::puts("urt_client " URT_VERSION "\n"
              "\n"
              "Options:\n"
              "  --connect <host[:port]>  server address (default 127.0.0.1:30777)\n"
              "  --name <text>            player name (default player)\n"
              "  --headless               no window: walk in a circle and print what is seen\n"
              "  --ticks <n>              headless only: stop after n ticks\n"
              "  --log-level <level>      trace, debug, info, warn, error, off (default info)\n"
              "  --help                   show this text");
}

// Splits "host" or "host:port". Returns false if the port is not a number.
bool parse_address(std::string_view text, std::string& host, std::uint16_t& port) {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos) {
        host = std::string(text);
        return !host.empty();
    }
    const std::string_view port_text = text.substr(colon + 1);
    unsigned value = 0;
    const char* const end = port_text.data() + port_text.size();
    const auto [parsed_end, error] = std::from_chars(port_text.data(), end, value);
    if (error != std::errc{} || parsed_end != end || value == 0 || value > 65535) {
        return false;
    }
    host = std::string(text.substr(0, colon));
    port = static_cast<std::uint16_t>(value);
    return !host.empty();
}

// Stands in for a human until there is a window and a keyboard: steers the
// player in a circle and reports once per second.
class BotSystem final : public urt::System {
public:
    BotSystem(urt::Application& app, urt::net::Client& client) : app_(app), client_(client) {}

    std::string_view name() const override { return "bot"; }

    // True if the connection ended before the user asked to quit.
    bool lost_connection() const { return lost_connection_; }

    void tick(urt::World& world, const urt::TickContext& context) override {
        if (client_.state() == urt::net::ClientState::Disconnected) {
            lost_connection_ = true;
            app_.stop();
            return;
        }

        // Walk direction turns slowly, which makes a circle.
        const auto angle = static_cast<float>(context.time * 0.5);
        client_.set_input(std::cos(angle), std::sin(angle), angle);

        const auto ticks_per_second = static_cast<std::uint64_t>(std::llround(1.0 / context.dt));
        if (context.tick % ticks_per_second == 0) {
            report(world);
        }
    }

private:
    void report(urt::World& world) const {
        auto& registry = world.registry;
        const urt::Entity me = client_.player_entity();
        if (me == entt::null) {
            return; // not in the world yet
        }
        const glm::vec3 my_position = registry.get<urt::Transform>(me).position;

        // Find the closest entity that is not us.
        float nearest_distance = std::numeric_limits<float>::max();
        urt::net::NetId nearest_id = 0;
        for (auto [entity, id, transform] :
             registry.view<const urt::net::NetworkId, const urt::Transform>().each()) {
            if (entity == me) {
                continue;
            }
            const glm::vec3 d = transform.position - my_position;
            const float distance = std::sqrt(d.x * d.x + d.z * d.z);
            if (distance < nearest_distance) {
                nearest_distance = distance;
                nearest_id = id.value;
            }
        }

        if (nearest_id != 0) {
            urt::log::info("bot", "player {} at ({:.1f}, {:.1f}), sees {} others, nearest is {} at {:.1f} m",
                           client_.player_id(), my_position.x, my_position.z,
                           client_.visible_count() - 1, nearest_id, nearest_distance);
        } else {
            urt::log::info("bot", "player {} at ({:.1f}, {:.1f}), sees nobody", client_.player_id(),
                           my_position.x, my_position.z);
        }
    }

    urt::Application& app_;
    urt::net::Client& client_;
    bool lost_connection_ = false;
};

#if defined(URT_CLIENT_WINDOW)
// The windowed main loop. Application::run() sleeps between ticks, which is
// right for a server; here every screen refresh draws a frame, and the fixed
// timestep decides how many simulation ticks fit in between.
// Returns false if the window could not be opened.
bool run_windowed(urt::Application& app, urt::net::Client& client, bool& lost_connection) {
    DebugView view;
    if (!view.open(1280, 720, "URT")) {
        return false;
    }
    urt::platform::install_shutdown_handler();
    urt::log::info("client", "walk with WASD or the arrow keys, Esc quits");

    urt::FixedTimestep timestep(app.config().tick_rate);
    urt::Clock::time_point last = urt::Clock::now();
    float yaw = 0.0f;
    std::uint64_t frame = 0;

    while (!view.should_close() && !urt::platform::shutdown_requested()) {
        if (client.state() == urt::net::ClientState::Disconnected) {
            lost_connection = true;
            break;
        }

        const urt::MoveIntent intent = urt::to_intent(view.poll_keys(), yaw);
        yaw = intent.yaw;
        client.set_input(intent.x, intent.z, intent.yaw);

        const urt::Clock::time_point now = urt::Clock::now();
        app.step(static_cast<std::uint64_t>(timestep.advance(now - last)));
        last = now;

        view.draw(app.world(), client.player_entity());

        if (++frame % 30 == 0) {
            view.set_title(std::format("URT - player {} - {} entities visible", client.player_id(),
                                       client.visible_count()));
        }
    }
    return true;
}
#endif

int run_client(const urt::Args& args) {
    if (args.has("help")) {
        print_usage();
        return 0;
    }

    if (const auto text = args.value("log-level")) {
        urt::log::Level level{};
        if (!urt::log::parse_level(*text, level)) {
            urt::log::error("client", "unknown log level '{}'", *text);
            return 2;
        }
        urt::log::set_level(level);
    }

    urt::net::ClientConfig net_config;
    if (const auto address = args.value("connect")) {
        if (!parse_address(*address, net_config.host, net_config.port)) {
            urt::log::error("client", "--connect needs host or host:port, got '{}'", *address);
            return 2;
        }
    } else if (args.has("connect")) {
        urt::log::error("client", "--connect needs a value");
        return 2;
    }
    if (const auto name = args.value("name")) {
        net_config.name = std::string(*name);
    }

    const std::optional<double> ticks = args.number("ticks", 0.0);
    if (!ticks || *ticks < 0.0) {
        urt::log::error("client", "--ticks needs a non-negative number");
        return 2;
    }

    urt::AppConfig config;
    config.name = "urt_client";
    config.role = urt::Role::Client;
    config.tick_rate = 30.0;
    config.max_ticks = static_cast<std::uint64_t>(*ticks);
    config.stats_interval_seconds = 0.0; // the bot's report is enough output

    urt::Application app(config);
    auto& client = app.add_system<urt::net::Client>(net_config);

    if (!client.connect()) {
        return 1;
    }

#if defined(URT_CLIENT_WINDOW)
    if (!args.has("headless")) {
        bool lost_connection = false;
        if (run_windowed(app, client, lost_connection)) {
            return lost_connection ? 1 : 0;
        }
        urt::log::warn("client", "cannot open a window, continuing headless");
    }
#endif

    auto& bot = app.add_system<BotSystem>(app, client);
    app.run();
    return bot.lost_connection() ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run_client(urt::Args(argc, argv));
    } catch (const std::exception& e) {
        urt::log::error("client", "fatal: {}", e.what());
        return 1;
    }
}
