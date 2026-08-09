#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace cmakegen {

/// Returns the CMake Presets schema version supported by the requested CMake
/// minimum version. The MVP baseline is CMake 3.28, which supports schema 8.
///
/// @throws std::invalid_argument when the version is malformed or predates the
///         MVP baseline.
[[nodiscard]] std::uint32_t preset_schema_version_for(std::string_view cmake_minimum_version);

/// Parses major, minor, and optional patch components (defaulting patch to 0).
[[nodiscard]] std::array<std::uint32_t, 3> cmake_version_components(std::string_view version);

} // namespace cmakegen
