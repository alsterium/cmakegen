#pragma once

#include "cmakegen/domain/model.hpp"
#include "cmakegen/validation/semantic_validator.hpp"

#include <map>
#include <string>
#include <vector>

namespace cmakegen {

// Sources have already been expanded by SourceResolver. The renderer keeps
// platform-specific sources separate so it can emit native CMake conditions.
using RenderedSourcesByTarget = std::map<std::string, ResolvedTargetSources>;

struct RenderedCmakeFiles {
    std::string root_cmake_lists;
    std::string generated_cmake;
};

// Renders only from normalized, resolved, semantically validated input. The
// TargetGraph argument deliberately makes validation an explicit pipeline
// prerequisite; this function performs no filesystem or process operations.
[[nodiscard]] RenderedCmakeFiles render_cmake(const Project& project, const TargetGraph& graph,
                                               const RenderedSourcesByTarget& sources,
                                               std::string_view source_label = "project.yml");

} // namespace cmakegen
