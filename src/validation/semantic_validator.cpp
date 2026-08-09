#include "cmakegen/validation/semantic_validator.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>
#include <string_view>

namespace cmakegen {
namespace {

[[nodiscard]] bool is_valid_logical_name(std::string_view name) {
    return !name.empty() && std::ranges::all_of(name, [](unsigned char character) {
        return std::isalnum(character) || character == '_' || character == '.' ||
               character == '+' || character == '-';
    });
}

[[nodiscard]] std::string target_path(const std::string& name) {
    return "targets." + name;
}

void add_semantic_error(DiagnosticSet& diagnostics, std::string path, std::string message) {
    diagnostics.add({std::string{diagnostic_codes::semantic_error}, DiagnosticSeverity::error,
                     std::move(message), std::move(path), std::nullopt});
}

void validate_interface_scopes(DiagnosticSet& diagnostics, const Target& target,
                               const std::string& path) {
    const auto validate_scopes = [&](const auto& values, const std::string& property_path) {
        if (!values.private_values.empty()) {
            add_semantic_error(diagnostics, property_path + ".private",
                               "interface libraries only allow interface scope");
        }
        if (!values.public_values.empty()) {
            add_semantic_error(diagnostics, property_path + ".public",
                               "interface libraries only allow interface scope");
        }
    };
    validate_scopes(target.include_directories, path + ".includeDirectories");
    validate_scopes(target.definitions, path + ".definitions");
    validate_scopes(target.compile_options, path + ".compileOptions");
    validate_scopes(target.dependencies, path + ".dependencies");

    const auto validate_override = [&](const TargetOverride& override, std::string_view platform) {
        const auto platform_path = path + ".platform." + std::string{platform};
        validate_scopes(override.include_directories, platform_path + ".includeDirectories");
        validate_scopes(override.definitions, platform_path + ".definitions");
        validate_scopes(override.compile_options, platform_path + ".compileOptions");
        validate_scopes(override.dependencies, platform_path + ".dependencies");
    };
    validate_override(target.platform.macos, "macos");
    validate_override(target.platform.linux, "linux");
    validate_override(target.platform.windows, "windows");
}

void validate_source_entries(DiagnosticSet& diagnostics, const std::vector<SourceEntry>& sources,
                             TargetType target_type, const std::string& sources_path) {
    for (std::size_t index = 0; index < sources.size(); ++index) {
        const auto& source = sources[index];
        if (source.path.empty() || source.path == ".") {
            add_semantic_error(diagnostics, sources_path + "[" + std::to_string(index) + "]",
                               "source path must not be empty or current directory");
        }
        if (target_type == TargetType::interface_library && source.kind == SourceKind::source) {
            add_semantic_error(diagnostics, sources_path + "[" + std::to_string(index) + "]",
                               "interface libraries cannot contain implementation sources");
        }
    }
}

void validate_sources(DiagnosticSet& diagnostics, const Target& target, const std::string& path,
                      const ResolvedSourcesByTarget& resolved_sources) {
    validate_source_entries(diagnostics, target.sources, target.type, path + ".sources");
    validate_source_entries(diagnostics, target.platform.macos.sources, target.type,
                            path + ".platform.macos.sources");
    validate_source_entries(diagnostics, target.platform.linux.sources, target.type,
                            path + ".platform.linux.sources");
    validate_source_entries(diagnostics, target.platform.windows.sources, target.type,
                            path + ".platform.windows.sources");
    const auto resolved = resolved_sources.find(target.name);
    if (target.type != TargetType::interface_library || resolved == resolved_sources.end()) return;
    for (const auto& source : resolved->second) {
        if (source.kind == SourceKind::source) {
            add_semantic_error(diagnostics, path + ".sources",
                               "interface libraries cannot contain implementation sources: " + source.path);
        }
    }
}

[[nodiscard]] std::vector<LinkDependency> sorted_dependencies(const Target& target) {
    std::vector<LinkDependency> dependencies;
    const auto append = [&dependencies](const auto& values) {
        dependencies.insert(dependencies.end(), values.begin(), values.end());
    };
    append(target.dependencies.private_values);
    append(target.dependencies.public_values);
    append(target.dependencies.interface_values);
    std::ranges::sort(dependencies, {}, [](const LinkDependency& dependency) {
        return std::pair{dependency.name, dependency.kind};
    });
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end(),
                                   [](const LinkDependency& left, const LinkDependency& right) {
        return left.name == right.name && left.kind == right.kind;
    }), dependencies.end());
    return dependencies;
}

} // namespace

Result<TargetGraph> validate_project(const Project& project,
                                     const ResolvedSourcesByTarget& resolved_sources) {
    DiagnosticSet diagnostics;
    TargetGraph graph;

    if (!is_valid_logical_name(project.name)) {
        add_semantic_error(diagnostics, "name", "project name contains unsupported characters");
    }

    std::set<std::string> logical_names;
    for (const auto& [key, target] : project.targets) {
        const auto path = target_path(key);
        if (!is_valid_logical_name(key)) {
            add_semantic_error(diagnostics, path, "target name contains unsupported characters");
        }
        if (target.name != key) {
            add_semantic_error(diagnostics, path, "target key and logical target name must match");
        }
        if (!logical_names.insert(target.name).second) {
            add_semantic_error(diagnostics, path, "duplicate logical target definition '" + target.name + "'");
        }
        validate_sources(diagnostics, target, path, resolved_sources);
        if (target.type == TargetType::interface_library) {
            validate_interface_scopes(diagnostics, target, path);
        }

        auto dependencies = sorted_dependencies(target);
        for (auto& dependency : dependencies) {
            dependency.kind = project.targets.contains(dependency.name)
                                  ? LinkDependency::Kind::project_target
                                  : LinkDependency::Kind::imported_target;
        }
        graph.dependencies.emplace(key, dependencies);
        auto& internal = graph.internal_adjacency[key];
        for (const auto& dependency : dependencies) {
            if (dependency.kind == LinkDependency::Kind::project_target) internal.push_back(dependency.name);
        }
    }

    std::map<std::string, int> state;
    std::vector<std::string> stack;
    std::function<void(const std::string&)> visit = [&](const std::string& node) {
        state[node] = 1;
        stack.push_back(node);
        for (const auto& dependency : graph.internal_adjacency.at(node)) {
            if (state[dependency] == 0) {
                visit(dependency);
            } else if (state[dependency] == 1) {
                const auto first = std::find(stack.begin(), stack.end(), dependency);
                std::string cycle;
                for (auto iterator = first; iterator != stack.end(); ++iterator) {
                    if (!cycle.empty()) cycle += " -> ";
                    cycle += *iterator;
                }
                cycle += " -> " + dependency;
                diagnostics.add({std::string{diagnostic_codes::dependency_cycle}, DiagnosticSeverity::error,
                                 "internal dependency cycle: " + cycle, target_path(node) + ".dependencies",
                                 std::nullopt});
            }
        }
        stack.pop_back();
        state[node] = 2;
    };
    for (const auto& [name, _] : graph.internal_adjacency) {
        if (state[name] == 0) visit(name);
    }

    if (diagnostics.has_errors()) return std::unexpected{std::move(diagnostics)};
    return graph;
}

} // namespace cmakegen
