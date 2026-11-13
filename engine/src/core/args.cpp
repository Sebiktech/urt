#include "urt/core/args.hpp"

#include <charconv>
#include <cstddef>

namespace urt {
namespace {

bool is_option(std::string_view text) { return text.starts_with("--"); }

} // namespace

Args::Args(int argc, const char* const* argv) {
    // argv[0] is the program name, not an argument.
    for (int i = 1; i < argc; ++i) {
        arguments_.emplace_back(argv[i]);
    }
}

Args::Args(std::vector<std::string> arguments) : arguments_(std::move(arguments)) {}

bool Args::has(std::string_view name) const {
    for (const std::string& argument : arguments_) {
        if (is_option(argument) && std::string_view(argument).substr(2) == name) {
            return true;
        }
    }
    return false;
}

std::optional<std::string_view> Args::value(std::string_view name) const {
    for (std::size_t i = 0; i + 1 < arguments_.size(); ++i) {
        const std::string_view argument = arguments_[i];
        if (is_option(argument) && argument.substr(2) == name) {
            const std::string_view next = arguments_[i + 1];
            if (is_option(next)) {
                return std::nullopt;
            }
            return next;
        }
    }
    return std::nullopt;
}

std::optional<double> Args::number(std::string_view name, double fallback) const {
    if (!has(name)) {
        return fallback;
    }
    const std::optional<std::string_view> text = value(name);
    if (!text) {
        return std::nullopt;
    }
    double result = 0.0;
    const char* const end = text->data() + text->size();
    const auto [parsed_end, error] = std::from_chars(text->data(), end, result);
    if (error != std::errc{} || parsed_end != end) {
        return std::nullopt;
    }
    return result;
}

} // namespace urt
