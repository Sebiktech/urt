#pragma once

// Minimal command line parsing: "--name value" options and "--name" flags.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace urt {

class Args {
public:
    Args(int argc, const char* const* argv);
    explicit Args(std::vector<std::string> arguments);

    // True if "--name" is present.
    bool has(std::string_view name) const;

    // The argument after "--name", if there is one and it is not another option.
    std::optional<std::string_view> value(std::string_view name) const;

    // value() converted to a number. Returns `fallback` when the option is
    // missing. Returns std::nullopt when it is present but not a valid number.
    std::optional<double> number(std::string_view name, double fallback) const;

private:
    std::vector<std::string> arguments_;
};

} // namespace urt
