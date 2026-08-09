#include "cmakegen/validation/semantic_validator.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
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

[[nodiscard]] bool is_valid_cmake_version(std::string_view version) {
    if (version.empty()) return false;

    std::size_t components = 1;
    bool expect_digit = true;
    for (const auto character : version) {
        if (std::isdigit(static_cast<unsigned char>(character))) {
            expect_digit = false;
        } else if (character == '.' && !expect_digit && components < 4) {
            ++components;
            expect_digit = true;
        } else {
            return false;
        }
    }
    return !expect_digit;
}

// Compare after grammar validation without narrowing arbitrary-length numeric
// components. This avoids both lexicographic ordering mistakes (3.10 vs 3.9)
// and integer overflow in valid CMake version text.
[[nodiscard]] bool version_at_least(std::string_view value, std::string_view minimum) {
    const auto next_component = [](std::string_view& remaining) {
        const auto separator = remaining.find('.');
        const auto component = remaining.substr(0, separator);
        remaining = separator == std::string_view::npos ? std::string_view{}
                                                         : remaining.substr(separator + 1);
        const auto significant = component.find_first_not_of('0');
        return significant == std::string_view::npos ? std::string_view{"0"}
                                                      : component.substr(significant);
    };

    for (std::size_t index = 0; index < 4; ++index) {
        const auto left = value.empty() ? std::string_view{"0"} : next_component(value);
        const auto right = minimum.empty() ? std::string_view{"0"} : next_component(minimum);
        if (left.size() != right.size()) return left.size() > right.size();
        if (left != right) return left > right;
    }
    return true;
}

// project(VERSION) accepts a numeric version, while find_package() additionally
// accepts a closed or half-open numeric range.  Keep these grammars separate:
// accepting a range for project(VERSION), or rejecting it for find_package(),
// would make an otherwise validated model fail when CMake configures it.
[[nodiscard]] bool is_valid_package_version(std::string_view version) {
    const auto separator = version.find("...");
    if (separator == std::string_view::npos) return is_valid_cmake_version(version);

    const auto minimum = version.substr(0, separator);
    auto maximum = version.substr(separator + 3);
    const bool exclusive_upper_bound = maximum.starts_with('<');
    if (exclusive_upper_bound) maximum.remove_prefix(1);
    if (!is_valid_cmake_version(minimum) || !is_valid_cmake_version(maximum)) return false;

    // CMake rejects backwards ranges and an exclusive range whose endpoints
    // coincide.  A closed range with equal endpoints is still meaningful.
    return exclusive_upper_bound ? !version_at_least(minimum, maximum)
                                 : version_at_least(maximum, minimum);
}

// cmake_minimum_required() requires at least a major and minor component, and
// optionally permits a policy-version range with the same component grammar.
[[nodiscard]] bool is_valid_cmake_minimum_version(std::string_view version) {
    const auto has_major_and_minor = [](std::string_view value) {
        return is_valid_cmake_version(value) && value.find('.') != std::string_view::npos;
    };

    const auto separator = version.find("...");
    if (separator == std::string_view::npos)
        return has_major_and_minor(version) && version_at_least(version, "3.28");

    const auto minimum = version.substr(0, separator);
    const auto policy_maximum = version.substr(separator + 3);
    return has_major_and_minor(minimum) && has_major_and_minor(policy_maximum) &&
           version_at_least(minimum, "3.28") && version_at_least(policy_maximum, minimum);
}

[[nodiscard]] bool is_supported_c_standard(std::string_view standard) {
    return standard == "90" || standard == "99" || standard == "11" || standard == "17" ||
           standard == "23";
}

[[nodiscard]] bool is_supported_cxx_standard(std::string_view standard) {
    return standard == "98" || standard == "11" || standard == "14" || standard == "17" ||
           standard == "20" || standard == "23";
}

[[nodiscard]] std::string target_path(const std::string& name) {
    return "targets." + name;
}

void add_semantic_error(DiagnosticSet& diagnostics, std::string path, std::string message) {
    diagnostics.add({std::string{diagnostic_codes::semantic_error}, DiagnosticSeverity::error,
                     std::move(message), std::move(path), std::nullopt});
}

void add_source_semantic_error(DiagnosticSet& diagnostics, const SourceEntry& source,
                               std::string fallback_path, std::string message) {
    diagnostics.add({std::string{diagnostic_codes::semantic_error}, DiagnosticSeverity::error,
                     std::move(message), source.diagnostic_path.empty() ? std::move(fallback_path)
                                                                        : source.diagnostic_path,
                     source.diagnostic_location});
}

void add_resolved_source_semantic_error(DiagnosticSet& diagnostics, const ResolvedSource& source,
                                        std::string fallback_path, std::string message) {
    diagnostics.add({std::string{diagnostic_codes::semantic_error}, DiagnosticSeverity::error,
                     std::move(message), source.diagnostic_path.empty() ? std::move(fallback_path)
                                                                        : source.diagnostic_path,
                     source.diagnostic_location});
}

void validate_literal_cmake_argument(DiagnosticSet& diagnostics, const std::string& path,
                                     std::string_view value) {
    // `\$` stops CMake from expanding a variable while it reads its script, but
    // CMake passes the resulting `${name}` through to Ninja. Ninja then expands
    // it as one of its own variables. There is no portable literal encoding for
    // this value across CMake generators, so reject the ambiguous syntax rather
    // than producing a build file with a changed compiler argument.
    if (value.contains("${") || value.contains("$<") || value.contains("$(")) {
        add_semantic_error(diagnostics, path,
                           "literal CMake arguments must not contain evaluated dollar syntax");
    }
    if (value.contains('\n') || value.contains('\r')) {
        add_semantic_error(diagnostics, path,
                           "literal CMake arguments must not contain line breaks");
    }
}

template <typename T, typename Value>
void validate_scoped_literal_cmake_arguments(DiagnosticSet& diagnostics, const ScopedValues<T>& values,
                                             const std::string& path, Value value) {
    const auto validate_scope = [&](const std::vector<T>& scope_values, std::string_view scope) {
        for (std::size_t index = 0; index < scope_values.size(); ++index) {
            const auto item = value(scope_values[index]);
            validate_literal_cmake_argument(diagnostics,
                                            path + "." + std::string{scope} + "[" +
                                                std::to_string(index) + "]",
                                            item);
            if (item == "PUBLIC" || item == "PRIVATE" || item == "INTERFACE") {
                add_semantic_error(diagnostics, path + "." + std::string{scope} + "[" +
                                                   std::to_string(index) + "]",
                                   "literal CMake arguments must not equal a scope keyword");
            }
        }
    };
    validate_scope(values.private_values, "private");
    validate_scope(values.public_values, "public");
    validate_scope(values.interface_values, "interface");
}

void validate_compile_definitions(DiagnosticSet& diagnostics, const ScopedValues<std::string>& values,
                                  const std::string& path) {
    const auto validate = [&](const std::vector<std::string>& entries, std::string_view scope) {
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const auto& entry = entries[index];
            if (entry.contains('#') || entry.find('(') < entry.find('=')) {
                add_semantic_error(diagnostics, path + "." + std::string{scope} + "[" +
                                                   std::to_string(index) + "]",
                                   "compile definitions must not contain '#' or function-style syntax");
            }
        }
    };
    validate(values.private_values, "private");
    validate(values.public_values, "public");
    validate(values.interface_values, "interface");
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
    const auto validate_dependencies = [&](const ScopedValues<LinkDependency>& values,
                                           const std::string& property_path) {
        const auto validate_scope = [&](const std::vector<LinkDependency>& dependencies,
                                        std::string_view scope) {
            for (std::size_t index = 0; index < dependencies.size(); ++index) {
                const auto& dependency = dependencies[index];
                if (scope == "interface") continue;
                diagnostics.add({std::string{diagnostic_codes::semantic_error}, DiagnosticSeverity::error,
                                 "interface libraries only allow interface scope",
                                 dependency.diagnostic_path.empty()
                                     ? property_path + "." + std::string{scope} + "[" + std::to_string(index) + "]"
                                     : dependency.diagnostic_path,
                                 dependency.diagnostic_location});
            }
        };
        validate_scope(values.private_values, "private");
        validate_scope(values.public_values, "public");
        validate_scope(values.interface_values, "interface");
    };
    validate_dependencies(target.dependencies, path + ".dependencies");

    const auto validate_override = [&](const TargetOverride& override, std::string_view platform) {
        const auto platform_path = path + ".platform." + std::string{platform};
        validate_scopes(override.include_directories, platform_path + ".includeDirectories");
        validate_scopes(override.definitions, platform_path + ".definitions");
        validate_scopes(override.compile_options, platform_path + ".compileOptions");
        validate_dependencies(override.dependencies, platform_path + ".dependencies");
    };
    validate_override(target.platform.macos, "macos");
    validate_override(target.platform.linux, "linux");
    validate_override(target.platform.windows, "windows");
}

void validate_source_entries(DiagnosticSet& diagnostics, const std::vector<SourceEntry>& sources,
                             TargetType target_type, const std::string& sources_path) {
    for (std::size_t index = 0; index < sources.size(); ++index) {
        const auto& source = sources[index];
        const auto source_path = sources_path + "[" + std::to_string(index) + "]";
        if (source.path.empty()) {
            add_source_semantic_error(diagnostics, source, source_path, "source path must not be empty");
        }
        if (target_type == TargetType::interface_library && source.kind == SourceKind::source) {
            add_source_semantic_error(diagnostics, source, source_path,
                                      "interface libraries cannot contain implementation sources");
        }
    }
}

[[nodiscard]] std::optional<Language> source_language(std::string_view path) {
    const auto extension = std::filesystem::path{path}.extension().string();
    // CMake determines languages from case-sensitive suffixes: `.c` is C,
    // while `.C` is C++. Do not lowercase these before validating the model.
    if (extension == ".c") return Language::c;
    if (extension == ".C" || extension == ".c++" || extension == ".cc" ||
        extension == ".cpp" || extension == ".cxx") {
        return Language::cxx;
    }
    return std::nullopt;
}

void validate_source_languages(DiagnosticSet& diagnostics, const Project& project,
                               const std::vector<ResolvedSource>& sources,
                               const std::string& fallback_path) {
    for (const auto& source : sources) {
        if (source.kind != SourceKind::source) continue;
        const auto language = source_language(source.path);
        if (!language) {
            add_resolved_source_semantic_error(
                diagnostics, source, fallback_path,
                "implementation source has no supported CMake language: " + source.path);
        } else if (std::ranges::find(project.cmake.languages, *language) == project.cmake.languages.end()) {
            add_resolved_source_semantic_error(
                diagnostics, source, fallback_path,
                "implementation source language is not enabled by cmake.languages: " + source.path);
        }
    }
}

void validate_sources(DiagnosticSet& diagnostics, const Target& target, const std::string& path,
                      const Project& project, const ResolvedSourcesByTarget& resolved_sources) {
    validate_source_entries(diagnostics, target.sources, target.type, path + ".sources");
    validate_source_entries(diagnostics, target.platform.macos.sources, target.type,
                            path + ".platform.macos.sources");
    validate_source_entries(diagnostics, target.platform.linux.sources, target.type,
                            path + ".platform.linux.sources");
    validate_source_entries(diagnostics, target.platform.windows.sources, target.type,
                            path + ".platform.windows.sources");
    const auto resolved = resolved_sources.find(target.name);
    if (resolved == resolved_sources.end()) return;
    const auto& resolved_target = resolved->second;
    const auto all_sources = [&]() {
        std::vector<ResolvedSource> sources = resolved_target.common;
        for (const auto* platform : {&resolved_target.macos, &resolved_target.linux,
                                     &resolved_target.windows}) {
            sources.insert(sources.end(), platform->begin(), platform->end());
        }
        return sources;
    }();
    validate_source_languages(diagnostics, project, all_sources, path + ".sources");
    for (const auto& source : all_sources) {
        if (source.path.contains(';')) {
            add_resolved_source_semantic_error(
                diagnostics, source, path + ".sources",
                "source paths must not contain semicolons because CMake cannot render them portably");
        }
        validate_literal_cmake_argument(diagnostics,
                                        source.diagnostic_path.empty() ? path + ".sources"
                                                                       : source.diagnostic_path,
                                        source.path);
    }
    if (target.type == TargetType::interface_library) {
        for (const auto& source : all_sources) {
            if (source.kind == SourceKind::source) {
                add_resolved_source_semantic_error(diagnostics, source, path + ".sources",
                            "interface libraries cannot contain implementation sources: " + source.path);
            }
        }
        return;
    }
    const auto has_implementation = [](const std::vector<ResolvedSource>& sources) {
        return std::ranges::any_of(sources, [](const ResolvedSource& source) {
            return source.kind == SourceKind::source;
        });
    };
    const auto validate_platform = [&](const std::vector<ResolvedSource>& platform_sources,
                                       std::string_view platform) {
        std::vector<ResolvedSource> effective = resolved_target.common;
        effective.insert(effective.end(), platform_sources.begin(), platform_sources.end());
        if (!has_implementation(effective)) {
            add_semantic_error(diagnostics, path + ".platform." + std::string{platform} + ".sources",
                               "non-interface targets require at least one implementation source");
        }
    };
    validate_platform(resolved_target.macos, "macos");
    validate_platform(resolved_target.linux, "linux");
    validate_platform(resolved_target.windows, "windows");
}

[[nodiscard]] bool is_link_mode_keyword(std::string_view value) {
    return value == "debug" || value == "optimized" || value == "general" ||
           value == "PUBLIC" || value == "PRIVATE" || value == "INTERFACE" ||
           value == "LINK_PUBLIC" || value == "LINK_PRIVATE" ||
           value == "LINK_INTERFACE_LIBRARIES";
}

void validate_dependencies(DiagnosticSet& diagnostics, const ScopedValues<LinkDependency>& dependencies,
                           const std::string& path) {
    const auto validate_scope = [&](const std::vector<LinkDependency>& values, std::string_view scope) {
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (is_link_mode_keyword(values[index].name)) {
                add_semantic_error(diagnostics, path + "." + std::string{scope} + "[" +
                                                   std::to_string(index) + "]",
                                   "dependency name is reserved as a CMake link-mode keyword: " +
                                       values[index].name);
            }
        }
    };
    validate_scope(dependencies.private_values, "private");
    validate_scope(dependencies.public_values, "public");
    validate_scope(dependencies.interface_values, "interface");
}

[[nodiscard]] bool is_find_package_keyword(std::string_view value) {
    static constexpr std::array keywords{"EXACT", "QUIET", "MODULE", "REQUIRED", "COMPONENTS",
                                         "OPTIONAL_COMPONENTS", "CONFIG", "NO_MODULE", "NAMES", "CONFIGS",
                                         "HINTS", "PATHS", "PATH_SUFFIXES", "REGISTRY_VIEW", "GLOBAL",
                                         "BYPASS_PROVIDER", "NO_POLICY_SCOPE",
                                         "NO_DEFAULT_PATH", "NO_PACKAGE_ROOT_PATH",
                                         "NO_CMAKE_PATH", "NO_CMAKE_ENVIRONMENT_PATH",
                                         "NO_SYSTEM_ENVIRONMENT_PATH", "NO_CMAKE_PACKAGE_REGISTRY",
                                         "NO_CMAKE_BUILDS_PATH", "NO_CMAKE_INSTALL_PREFIX",
                                         "NO_CMAKE_SYSTEM_PATH", "NO_CMAKE_SYSTEM_PACKAGE_REGISTRY",
                                         "NO_PACKAGE_REGISTRY", "NO_SYSTEM_PACKAGE_REGISTRY",
                                         "CMAKE_FIND_ROOT_PATH_BOTH",
                                         "ONLY_CMAKE_FIND_ROOT_PATH", "NO_CMAKE_FIND_ROOT_PATH"};
    return std::ranges::find(keywords, value) != keywords.end();
}

void classify_and_deduplicate(std::vector<LinkDependency>& dependencies, const Project& project) {
    // Normalization usually classifies these already, but semantic validation
    // accepts manually assembled Project values too. Classify before removing
    // duplicates so a dependency mentioned with different provisional kinds is
    // still represented exactly once in the effective graph.
    for (auto& dependency : dependencies) {
        dependency.kind = project.targets.contains(dependency.name)
                              ? LinkDependency::Kind::project_target
                              : LinkDependency::Kind::imported_target;
    }
    std::ranges::sort(dependencies, {}, [](const LinkDependency& dependency) {
        return std::pair{dependency.name, dependency.kind};
    });
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end(),
                                   [](const LinkDependency& left, const LinkDependency& right) {
        return left.name == right.name && left.kind == right.kind;
    }), dependencies.end());
}

[[nodiscard]] std::vector<LinkDependency> effective_dependencies(
    const Target& target, const Project& project, const TargetOverride* platform_override = nullptr) {
    std::vector<LinkDependency> dependencies;
    const auto append = [&dependencies](const auto& values) {
        dependencies.insert(dependencies.end(), values.begin(), values.end());
    };
    const auto append_scopes = [&append](const auto& scoped_values) {
        append(scoped_values.private_values);
        append(scoped_values.public_values);
        append(scoped_values.interface_values);
    };

    append_scopes(target.dependencies);
    if (platform_override != nullptr) append_scopes(platform_override->dependencies);
    classify_and_deduplicate(dependencies, project);
    return dependencies;
}

[[nodiscard]] std::vector<LinkDependency> all_effective_dependencies(const Target& target,
                                                                      const Project& project) {
    std::vector<LinkDependency> dependencies = effective_dependencies(target, project);
    for (const auto* platform : {&target.platform.macos, &target.platform.linux,
                                 &target.platform.windows}) {
        const auto platform_dependencies = effective_dependencies(target, project, platform);
        dependencies.insert(dependencies.end(), platform_dependencies.begin(), platform_dependencies.end());
    }
    classify_and_deduplicate(dependencies, project);
    return dependencies;
}

struct InternalDependencyEdge {
    std::string name;
    std::string provenance_path;
    std::optional<SourceLocation> provenance_location;
};

struct ReportedCycle {
    std::string sequence;
    std::optional<std::string> platform;

    auto operator<=>(const ReportedCycle&) const = default;
};

[[nodiscard]] std::vector<InternalDependencyEdge> effective_internal_dependencies(
    const std::string& target_name, const Target& target, const Project& project,
    const TargetOverride* platform_override = nullptr,
    const std::optional<std::string_view> platform = std::nullopt) {
    std::map<std::string, InternalDependencyEdge> edges;
    const auto append = [&](const auto& dependencies, const std::string& provenance_path) {
        for (const auto& dependency : dependencies) {
            if (dependency.kind == LinkDependency::Kind::project_target) {
                edges.try_emplace(dependency.name, InternalDependencyEdge{
                    dependency.name,
                    dependency.diagnostic_path.empty() ? provenance_path : dependency.diagnostic_path,
                    dependency.diagnostic_location});
            }
        }
    };

    append(effective_dependencies(target, project), target_path(target_name) + ".dependencies");
    if (platform_override != nullptr) {
        append(effective_dependencies(target, project, platform_override),
               target_path(target_name) + ".platform." + std::string(*platform) + ".dependencies");
    }

    std::vector<InternalDependencyEdge> result;
    result.reserve(edges.size());
    for (auto& [_, edge] : edges) {
        result.push_back(std::move(edge));
    }
    return result;
}

void detect_dependency_cycles(DiagnosticSet& diagnostics,
                              const std::map<std::string, std::vector<InternalDependencyEdge>>& adjacency,
                              std::set<ReportedCycle>& reported_cycles,
                              const std::optional<std::string_view> platform = std::nullopt) {
    std::map<std::string, int> state;
    std::vector<std::string> stack;
    std::function<void(const std::string&)> visit = [&](const std::string& node) {
        state[node] = 1;
        stack.push_back(node);
        for (const auto& dependency : adjacency.at(node)) {
            if (state[dependency.name] == 0) {
                visit(dependency.name);
            } else if (state[dependency.name] == 1) {
                const auto first = std::find(stack.begin(), stack.end(), dependency.name);
                std::vector<std::string> cycle_nodes(first, stack.end());
                // The same directed cycle can be encountered from a different
                // entry point in a platform graph.  Normalize rotations before
                // deduplicating common and platform-effective passes.
                std::size_t canonical_index = 0;
                for (std::size_t candidate = 1; candidate < cycle_nodes.size(); ++candidate) {
                    for (std::size_t offset = 0; offset < cycle_nodes.size(); ++offset) {
                        const auto& lhs = cycle_nodes[(candidate + offset) % cycle_nodes.size()];
                        const auto& rhs = cycle_nodes[(canonical_index + offset) % cycle_nodes.size()];
                        if (lhs == rhs) continue;
                        if (lhs < rhs) canonical_index = candidate;
                        break;
                    }
                }
                std::string cycle;
                for (std::size_t offset = 0; offset < cycle_nodes.size(); ++offset) {
                    if (!cycle.empty()) cycle += " -> ";
                    cycle += cycle_nodes[(canonical_index + offset) % cycle_nodes.size()];
                }
                cycle += " -> " + cycle_nodes[canonical_index];
                const ReportedCycle reported_cycle{
                    cycle, platform ? std::optional{std::string(*platform)} : std::nullopt};
                // A common cycle is present in every platform graph and has
                // already been reported by the common pass. Platform-only
                // cycles with the same target sequence, however, are distinct
                // errors because they are independently realizable builds.
                if ((!platform || !reported_cycles.contains({cycle, std::nullopt})) &&
                    reported_cycles.insert(reported_cycle).second) {
                    diagnostics.add({std::string{diagnostic_codes::dependency_cycle},
                                     DiagnosticSeverity::error, "internal dependency cycle: " + cycle,
                                     dependency.provenance_path,
                                     dependency.provenance_location});
                }
            }
        }
        stack.pop_back();
        state[node] = 2;
    };
    for (const auto& [name, _] : adjacency) {
        if (state[name] == 0) visit(name);
    }
}

} // namespace

Result<TargetGraph> validate_project(const Project& project,
                                     const ResolvedSourcesByTarget& resolved_sources) {
    DiagnosticSet diagnostics;
    TargetGraph graph;

    if (!is_valid_logical_name(project.name)) {
        add_semantic_error(diagnostics, "name", "project name contains unsupported characters");
    }
    if (project.version && !is_valid_cmake_version(*project.version)) {
        add_semantic_error(diagnostics, "version",
                           "project version must be one to four non-negative integer components");
    }
    if (!is_valid_cmake_minimum_version(project.cmake.minimum_version)) {
        add_semantic_error(diagnostics, "cmake.minimumVersion",
                           "CMake minimum version must be two to four non-negative integer components"
                           " or a range of such versions");
    }
    if (project.cmake.c_standard && !is_supported_c_standard(*project.cmake.c_standard)) {
        add_semantic_error(diagnostics, "cmake.cStandard",
                           "C standard must be one of 90, 99, 11, 17, or 23");
    }
    if (project.cmake.cxx_standard && !is_supported_cxx_standard(*project.cmake.cxx_standard)) {
        add_semantic_error(diagnostics, "cmake.cxxStandard",
                           "C++ standard must be one of 98, 11, 14, 17, 20, or 23");
    }
    for (const auto& [key, package] : project.packages) {
        if (package.version && !is_valid_package_version(*package.version)) {
            add_semantic_error(diagnostics, "packages." + key + ".version",
                               "package version must be a numeric version or numeric version range");
        }
        for (std::size_t index = 0; index < package.components.size(); ++index) {
            if (is_find_package_keyword(package.components[index])) {
                add_semantic_error(diagnostics, "packages." + key + ".components[" +
                                                   std::to_string(index) + "]",
                                   "package component name is reserved by find_package: " +
                                       package.components[index]);
            }
        }
    }
    for (std::size_t index = 0; index < project.cmake.include_files.size(); ++index) {
        validate_literal_cmake_argument(diagnostics,
                                        "cmake.include[" + std::to_string(index) + "]",
                                        project.cmake.include_files[index]);
    }

    std::set<std::string> logical_names;
    for (const auto& [key, target] : project.targets) {
        const auto path = target_path(key);
        if (!is_valid_logical_name(key)) {
            diagnostics.add({std::string{diagnostic_codes::semantic_error}, DiagnosticSeverity::error,
                             "target name contains unsupported characters",
                             target.diagnostic_path.empty() ? path : target.diagnostic_path,
                             target.diagnostic_location});
        }
        if (target.name != key) {
            add_semantic_error(diagnostics, path, "target key and logical target name must match");
        }
        static constexpr std::array reserved_target_names{
            "all", "clean", "help", "install", "package", "test", "edit_cache", "rebuild_cache",
            "preinstall", "ALL_BUILD", "INSTALL", "RUN_TESTS", "ZERO_CHECK"};
        if (std::ranges::find(reserved_target_names, target.name) != reserved_target_names.end()) {
            add_semantic_error(diagnostics, path,
                               "target name is reserved by CMake: " + target.name);
        }
        if (!logical_names.insert(target.name).second) {
            add_semantic_error(diagnostics, path, "duplicate logical target definition '" + target.name + "'");
        }
        validate_sources(diagnostics, target, path, project, resolved_sources);
        validate_scoped_literal_cmake_arguments(
            diagnostics, target.include_directories, path + ".includeDirectories",
            [](const std::string& value) -> std::string_view { return value; });
        validate_scoped_literal_cmake_arguments(
            diagnostics, target.definitions, path + ".definitions",
            [](const std::string& value) -> std::string_view { return value; });
        validate_compile_definitions(diagnostics, target.definitions, path + ".definitions");
        validate_scoped_literal_cmake_arguments(
            diagnostics, target.compile_options, path + ".compileOptions",
            [](const std::string& value) -> std::string_view { return value; });
        validate_scoped_literal_cmake_arguments(
            diagnostics, target.dependencies, path + ".dependencies",
            [](const LinkDependency& value) -> std::string_view { return value.name; });
        const auto validate_platform_literals = [&](const TargetOverride& override,
                                                    std::string_view platform) {
            const auto platform_path = path + ".platform." + std::string{platform};
            validate_scoped_literal_cmake_arguments(
                diagnostics, override.include_directories, platform_path + ".includeDirectories",
                [](const std::string& value) -> std::string_view { return value; });
            validate_scoped_literal_cmake_arguments(
                diagnostics, override.definitions, platform_path + ".definitions",
                [](const std::string& value) -> std::string_view { return value; });
            validate_compile_definitions(diagnostics, override.definitions,
                                         platform_path + ".definitions");
            validate_scoped_literal_cmake_arguments(
                diagnostics, override.compile_options, platform_path + ".compileOptions",
                [](const std::string& value) -> std::string_view { return value; });
            validate_scoped_literal_cmake_arguments(
                diagnostics, override.dependencies, platform_path + ".dependencies",
                [](const LinkDependency& value) -> std::string_view { return value.name; });
        };
        validate_platform_literals(target.platform.macos, "macos");
        validate_platform_literals(target.platform.linux, "linux");
        validate_platform_literals(target.platform.windows, "windows");
        validate_dependencies(diagnostics, target.dependencies, path + ".dependencies");
        validate_dependencies(diagnostics, target.platform.macos.dependencies,
                              path + ".platform.macos.dependencies");
        validate_dependencies(diagnostics, target.platform.linux.dependencies,
                              path + ".platform.linux.dependencies");
        validate_dependencies(diagnostics, target.platform.windows.dependencies,
                              path + ".platform.windows.dependencies");
        if (target.type == TargetType::interface_library) {
            validate_interface_scopes(diagnostics, target, path);
        }

        auto dependencies = all_effective_dependencies(target, project);
        graph.dependencies.emplace(key, dependencies);
        auto& internal = graph.internal_adjacency[key];
        for (const auto& dependency : dependencies) {
            if (dependency.kind == LinkDependency::Kind::project_target) internal.push_back(dependency.name);
        }
    }

    // A target's common dependencies are present on every platform, while its
    // platform overrides are mutually exclusive. Validate each effective
    // platform graph independently so macOS-only and Windows-only edges cannot
    // invent a cycle that no build can realize.
    std::set<ReportedCycle> reported_cycles;
    constexpr std::array<std::string_view, 3> platform_names{"macos", "linux", "windows"};
    for (const auto platform_index : std::array<std::optional<std::size_t>, 4>{
             std::nullopt, 0, 1, 2}) {
        std::map<std::string, std::vector<InternalDependencyEdge>> effective_adjacency;
        for (const auto& [name, target] : project.targets) {
            const TargetOverride* override = nullptr;
            if (platform_index) {
                const auto& overrides = std::array{&target.platform.macos, &target.platform.linux,
                                                    &target.platform.windows};
                override = overrides[*platform_index];
            }
            effective_adjacency[name] = effective_internal_dependencies(
                name, target, project, override,
                platform_index ? std::optional{platform_names[*platform_index]} : std::nullopt);
        }
        detect_dependency_cycles(
            diagnostics, effective_adjacency, reported_cycles,
            platform_index ? std::optional{platform_names[*platform_index]} : std::nullopt);
    }

    if (diagnostics.has_errors()) return std::unexpected{std::move(diagnostics)};
    return graph;
}

} // namespace cmakegen
