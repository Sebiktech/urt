// urt_client: the game client. For now it is a placeholder with no window.
//
// Step 2 adds the connection to the server; step 4 adds the Vulkan renderer
// as a separate library that only this executable links.

#include "urt/app/application.hpp"
#include "urt/core/args.hpp"
#include "urt/core/log.hpp"

#include <exception>

int main(int argc, char** argv) {
    try {
        const urt::Args args(argc, argv);

        urt::AppConfig config;
        config.name = "urt_client";
        config.role = urt::Role::Client;
        config.tick_rate = 30.0;
        // The client has nothing to do yet, so exit after one second of ticks
        // unless told otherwise.
        const auto ticks = args.number("ticks", 30.0);
        if (!ticks || *ticks < 0.0) {
            urt::log::error("client", "--ticks needs a non-negative number");
            return 2;
        }
        config.max_ticks = static_cast<std::uint64_t>(*ticks);

        urt::Application app(config);
        urt::log::info("client", "URT client {}: no renderer or networking yet", URT_VERSION);
        return app.run();
    } catch (const std::exception& e) {
        urt::log::error("client", "fatal: {}", e.what());
        return 1;
    }
}
