#include <doctest/doctest.h>

#include "urt/core/log.hpp"

#include <string>
#include <vector>

// No `namespace log = urt::log;` alias here: the C library already declares a
// global function log() (the logarithm from <math.h>), and a name cannot be
// both a function and a namespace in the same scope.

namespace {

// Captures log output for the lifetime of the object, then restores defaults.
struct Capture {
    std::vector<std::string> lines;
    urt::log::Level previous = urt::log::level();

    Capture() {
        urt::log::set_sink([this](urt::log::Level, std::string_view channel, std::string_view message) {
            lines.push_back(std::string(channel) + ": " + std::string(message));
        });
    }
    ~Capture() {
        urt::log::set_sink({});
        urt::log::set_level(previous);
    }
};

} // namespace

TEST_CASE("log formats messages and filters by level") {
    Capture capture;
    urt::log::set_level(urt::log::Level::Info);

    urt::log::debug("net", "hidden {}", 1);
    urt::log::info("net", "port {}", 30120);
    urt::log::error("app", "{} + {} = {}", 1, 2, 3);

    REQUIRE(capture.lines.size() == 2);
    CHECK(capture.lines[0] == "net: port 30120");
    CHECK(capture.lines[1] == "app: 1 + 2 = 3");

    urt::log::set_level(urt::log::Level::Off);
    urt::log::error("app", "also hidden");
    CHECK(capture.lines.size() == 2);
}

TEST_CASE("log parses level names") {
    urt::log::Level level = urt::log::Level::Info;
    CHECK(urt::log::parse_level("debug", level));
    CHECK(level == urt::log::Level::Debug);
    CHECK(urt::log::parse_level("off", level));
    CHECK(level == urt::log::Level::Off);

    CHECK_FALSE(urt::log::parse_level("loud", level));
    CHECK(level == urt::log::Level::Off); // unchanged
}
