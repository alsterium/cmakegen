#pragma once

#include "cmakegen/domain/model.hpp"

#include <string>

namespace cmakegen {

/// Renders a deterministic CMakePresets.json document for a normalized project.
/// The returned document uses two-space JSON indentation and ends with a newline.
///
/// @throws std::invalid_argument when the project's CMake minimum is not
///         supported by the MVP Presets compatibility mapping.
[[nodiscard]] std::string render_presets(const Project& project);

} // namespace cmakegen
