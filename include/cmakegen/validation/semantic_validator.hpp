#pragma once

#include "cmakegen/diagnostics/diagnostic.hpp"
#include "cmakegen/domain/model.hpp"

#include <map>
#include <vector>

namespace cmakegen {

// Resolved sources are optional because logical validation is useful before a
// source tree is available. When supplied, they allow interface-library source
// rules to be checked against the actual expanded file set.
using ResolvedSourcesByTarget = std::map<std::string, std::vector<ResolvedSource>>;

struct TargetGraph {
    // Both maps are sorted by target name. Dependency vectors are sorted and
    // de-duplicated, so rendering clients receive deterministic graph data.
    std::map<std::string, std::vector<std::string>> internal_adjacency;
    std::map<std::string, std::vector<LinkDependency>> dependencies;
};

// Validates semantic constraints and returns the dependency graph when the
// project is renderable. Dependencies not naming a project target remain
// imported targets and are retained in TargetGraph::dependencies.
[[nodiscard]] Result<TargetGraph> validate_project(
    const Project& project,
    const ResolvedSourcesByTarget& resolved_sources = {});

} // namespace cmakegen
