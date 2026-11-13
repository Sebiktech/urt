#include <doctest/doctest.h>

#include "urt/core/args.hpp"

using urt::Args;

TEST_CASE("Args reads flags and values") {
    const Args args({"--tick-rate", "60", "--verbose", "--name", "test server"});

    CHECK(args.has("verbose"));
    CHECK(args.has("tick-rate"));
    CHECK_FALSE(args.has("missing"));
    CHECK_FALSE(args.has("tick")); // no prefix matching

    CHECK(args.value("tick-rate") == "60");
    CHECK(args.value("name") == "test server");
    CHECK_FALSE(args.value("verbose").has_value()); // followed by another option
    CHECK_FALSE(args.value("missing").has_value());
}

TEST_CASE("Args skips the program name") {
    const char* const argv[] = {"urt_server", "--ticks", "10"};
    const Args args(3, argv);
    CHECK_FALSE(args.has("urt_server"));
    CHECK(args.value("ticks") == "10");
}

TEST_CASE("Args converts numbers and reports bad ones") {
    const Args args({"--rate", "62.5", "--bad", "12abc", "--empty"});

    CHECK(args.number("rate", 30.0) == 62.5);
    CHECK(args.number("missing", 30.0) == 30.0);
    CHECK_FALSE(args.number("bad", 1.0).has_value());
    CHECK_FALSE(args.number("empty", 1.0).has_value());
}
