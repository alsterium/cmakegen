#include "cmakegen/generator/presets_compatibility.hpp"

#include <charconv>
#include <limits>
#include <stdexcept>
#include <string>

namespace cmakegen {
namespace {

struct Version {
    std::uint32_t major;
    std::uint32_t minor;
    std::uint32_t patch;
    std::uint32_t tweak;
};

[[nodiscard]] std::uint32_t parse_component(std::string_view value) {
    // Semantic validation permits numeric components with leading zeroes.  Trim
    // them before converting so an otherwise small component does not overflow
    // solely because its textual representation is wide.
    if (value.empty())
        throw std::invalid_argument("CMake minimum version must contain numeric components");
    const auto first_non_zero = value.find_first_not_of('0');
    value = first_non_zero == std::string_view::npos ? std::string_view{"0"}
                                                      : value.substr(first_non_zero);
    std::uint32_t component = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), component);
    if (error != std::errc{} || end != value.data() + value.size())
        throw std::invalid_argument("CMake minimum version must contain numeric components");
    return component;
}

[[nodiscard]] Version parse_numeric_version(std::string_view value) {
    const auto first_dot = value.find('.');
    if (first_dot == std::string_view::npos)
        throw std::invalid_argument("CMake minimum version must use major.minor[.patch] form");

    const auto second_dot = value.find('.', first_dot + 1);
    const auto third_dot = second_dot == std::string_view::npos ? std::string_view::npos : value.find('.', second_dot + 1);
    if (third_dot != std::string_view::npos && value.find('.', third_dot + 1) != std::string_view::npos)
        throw std::invalid_argument("CMake minimum version has too many components");

    const auto major = parse_component(value.substr(0, first_dot));
    const auto minor = parse_component(value.substr(first_dot + 1, second_dot - first_dot - 1));
    const auto patch = second_dot == std::string_view::npos ? 0 :
        parse_component(value.substr(second_dot + 1, third_dot == std::string_view::npos ? std::string_view::npos : third_dot - second_dot - 1));
    const auto tweak = third_dot == std::string_view::npos ? 0 :
        parse_component(value.substr(third_dot + 1));
    return {major, minor, patch, tweak};
}

[[nodiscard]] Version parse_version(std::string_view value) {
    const auto range = value.find("...");
    if (range == std::string_view::npos) return parse_numeric_version(value);
    if (value.find("...", range + 3) != std::string_view::npos)
        throw std::invalid_argument("CMake policy range must contain one separator");
    const auto lower = value.substr(0, range);
    const auto upper = value.substr(range + 3);
    const auto minimum = parse_numeric_version(lower);
    (void)parse_numeric_version(upper);
    return minimum;
}

} // namespace

std::uint32_t preset_schema_version_for(const std::string_view cmake_minimum_version) {
    const auto version = parse_version(cmake_minimum_version);
    if (version.major < 3 || (version.major == 3 && version.minor < 28))
        throw std::invalid_argument("CMake Presets schema 8 requires CMake 3.28 or later");
    return 8;
}

std::array<std::uint32_t, 3> cmake_version_components(const std::string_view version) {
    const auto parsed = parse_version(version);
    // CMakePresets.json has only major/minor/patch fields.  A non-zero fourth
    // CMake component must therefore round the advertised patch *up*, never
    // down: 3.28.0.1 is not compatible with 3.28.0.
    if (parsed.tweak != 0) {
        if (parsed.patch == std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("CMake minimum version cannot be represented by CMake Presets");
        }
        return {parsed.major, parsed.minor, static_cast<std::uint32_t>(parsed.patch + 1)};
    }
    return {parsed.major, parsed.minor, parsed.patch};
}

} // namespace cmakegen
