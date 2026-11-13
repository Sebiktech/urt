// urt_server: the dedicated server. It owns the authoritative game state and
// has no window, so it runs on a plain Linux box.
//
// Players connect, walk around and see each other. What happens in the world
// beyond that comes from resources: folders of Luau scripts in resources/.

#include "urt/app/application.hpp"
#include "urt/core/args.hpp"
#include "urt/core/log.hpp"
#include "urt/ecs/components.hpp"
#include "urt/ecs/movement_system.hpp"
#include "urt/net/server.hpp"
#include "urt/script/script_system.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace {

void print_usage() {
    std::puts("urt_server " URT_VERSION "\n"
              "\n"
              "Options:\n"
              "  --port <n>            UDP port to listen on, 0 = any free port (default 30777)\n"
              "  --max-players <n>     player limit (default 64)\n"
              "  --resources <dir>     folder with script resources (default: the nearest 'resources' folder)\n"
              "  --script-timeout <s>  stop a script call that runs longer; 0 = never (default 0.25)\n"
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

// Looks for a folder named "resources" in the current folder and its
// parents. Running the server from the build folder (as an IDE does) then
// still finds the resources in the source tree.
std::filesystem::path find_resources_directory() {
    std::error_code ignored;
    std::filesystem::path folder = std::filesystem::current_path(ignored);
    for (int level = 0; level < 8 && !folder.empty(); ++level) {
        const std::filesystem::path candidate = folder / "resources";
        if (std::filesystem::is_directory(candidate, ignored)) {
            return candidate;
        }
        if (folder == folder.parent_path()) {
            break; // reached the root
        }
        folder = folder.parent_path();
    }
    return {};
}

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

    // A whole number: clients are told the rate and must tick at exactly the
    // same one for their movement prediction to match.
    double tick_rate = 0;
    if (!read_count(args, "tick-rate", 30, 1, 1000, tick_rate)) {
        return 2;
    }

    double max_ticks = 0;
    double threads = 0;
    double port = 0;
    double max_players = 0;
    if (!read_count(args, "ticks", 0, 0, 1e15, max_ticks) ||
        !read_count(args, "threads", 0, 0, 1024, threads) ||
        !read_count(args, "port", urt::net::default_port, 0, 65535, port) ||
        !read_count(args, "max-players", 64, 1, 4000, max_players)) {
        return 2;
    }

    urt::AppConfig config;
    config.name = "urt_server";
    config.role = urt::Role::Server;
    config.tick_rate = tick_rate;
    config.max_ticks = static_cast<std::uint64_t>(max_ticks);
    config.worker_threads = static_cast<unsigned>(threads);

    urt::net::ServerConfig net_config;
    net_config.port = static_cast<std::uint16_t>(port);
    net_config.max_players = static_cast<std::size_t>(max_players);

    urt::script::ScriptConfig script_config;
    if (const auto directory = args.value("resources")) {
        script_config.resources_directory = std::filesystem::path(std::string(*directory));
    } else if (args.has("resources")) {
        urt::log::error("server", "--resources needs a folder");
        return 2;
    } else {
        script_config.resources_directory = find_resources_directory();
    }
    const std::optional<double> script_timeout = args.number("script-timeout", script_config.max_call_seconds);
    if (!script_timeout || *script_timeout < 0.0 || *script_timeout > 3600.0) {
        urt::log::error("server", "--script-timeout needs a number of seconds between 0 and 3600");
        return 2;
    }
    script_config.max_call_seconds = *script_timeout;

    urt::Application app(config);

    // Order matters: read the network, let scripts react, move, then send.
    auto& server = app.add_system<urt::net::Server>(net_config);
    app.add_system<urt::script::ScriptSystem>(server, script_config);
    app.add_system<urt::MovementSystem>();
    app.add_system<urt::net::ServerSnapshotSystem>(server);

    if (!server.start()) {
        return 1;
    }

    if (script_config.resources_directory.empty()) {
        urt::log::warn("server", "no 'resources' folder found; the world will be empty");
    } else {
        urt::log::info("server", "resources from '{}'", script_config.resources_directory.string());
    }
    urt::log::info("server", "URT server {} started", URT_VERSION);
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
