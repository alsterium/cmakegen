#pragma once

#include "cmakegen/diagnostics/diagnostic.hpp"
#include "cmakegen/domain/model.hpp"

#include <filesystem>
#include <vector>

namespace cmakegen {

// Resolves source paths relative to project_directory. Directory entries are
// expanded into supported source and header files; explicitly named files are
// retained regardless of their extension.
[[nodiscard]] Result<std::vector<ResolvedSource>> resolve_sources(
    const std::filesystem::path& project_directory,
    const std::vector<SourceEntry>& entries);

} // namespace cmakegen
