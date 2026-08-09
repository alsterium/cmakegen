#include "cmakegen/generator/presets_compatibility.hpp"

#include <charconv>
#include <stdexcept>
#include <string>

namespace cmakegen {
namespace {

struct Version {
    std::uint32_t major;
    std::uint32_t minor;
    std::uint32_t patch;
};

[[nodiscard]] std::uint32_t parse_component(std::string_view value) {
    std::uint32_t component = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), component);
    if (error != std::errc{} || end != value.data() + value.size())
        throw std::invalid_argument("CMake minimum version must contain numeric components");
    return component;
}

[[nodiscard]] Version parse_version(std::string_view value) {
    const auto first_dot = value.find('.');
    if (first_dot == std::string_view::npos)
        throw std::invalid_argument("CMake minimum version must use major.minor[.patch] form");

    const auto second_dot = value.find('.', first_dot + 1);
    if (second_dot != std::string_view::npos && value.find('.', second_dot + 1) != std::string_view::npos)
        throw std::invalid_argument("CMake minimum version has too many components");

    const auto major = parse_component(value.substr(0, first_dot));
    const auto minor = parse_component(value.substr(first_dot + 1, second_dot - first_dot - 1));
    const auto patch = second_dot == std::string_view::npos ? 0 : parse_component(value.substr(second_dot + 1));
    return {major, minor, patch};
}

} // namespace

std::uint32_t preset_schema_version_for(const std::string_view cmake_minimum_version) {
    const auto version = parse_version(cmake_minimum_version);
    if (version.major < 3 || (version.major == 3 && version.minor < 28))
        throw std::invalid_argument("CMake Presets schema 8 requires CMake 3.28 or later");
    return 8;
}

} // namespace cmakegen
