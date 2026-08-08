#include "cmakegen/domain/normalizer.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>

namespace cmakegen {
namespace {

[[nodiscard]] std::string normalize_path(const std::string& path) {
    auto slash_path = path;
    std::ranges::replace(slash_path, '\\', '/');

    std::string prefix;
    std::size_t start = 0;
    std::size_t protected_components = 0;
    bool drive_relative = false;
    const auto unc_server_end = slash_path.find('/', 2);
    const auto is_unc_root = slash_path.starts_with("//") && slash_path.size() > 2 &&
                             slash_path[2] != '/' && unc_server_end != std::string::npos &&
                             unc_server_end + 1 < slash_path.size() &&
                             slash_path[unc_server_end + 1] != '/';
    const auto is_extended_unc_root =
        slash_path.size() >= 8 && slash_path.starts_with("//?/") && slash_path[7] == '/' &&
        std::tolower(static_cast<unsigned char>(slash_path[4])) == 'u' &&
        std::tolower(static_cast<unsigned char>(slash_path[5])) == 'n' &&
        std::tolower(static_cast<unsigned char>(slash_path[6])) == 'c';
    const auto is_extended_drive_root =
        slash_path.size() >= 7 && slash_path.starts_with("//?/") &&
        std::isalpha(static_cast<unsigned char>(slash_path[4])) && slash_path[5] == ':' &&
        slash_path[6] == '/';
    const auto is_drive_root = slash_path.size() >= 3 &&
                               std::isalpha(static_cast<unsigned char>(slash_path[0])) &&
                               slash_path[1] == ':' && slash_path[2] == '/';
    if (is_extended_drive_root) {
        prefix = slash_path.substr(0, 7);
        start = 7;
    } else if (is_drive_root) {
        prefix = slash_path.substr(0, 3);
        start = 3;
    } else if (slash_path.size() >= 2 && std::isalpha(static_cast<unsigned char>(slash_path[0])) &&
               slash_path[1] == ':') {
        prefix = slash_path.substr(0, 2);
        start = 2;
        drive_relative = true;
    } else if (is_unc_root) {
        prefix = "//";
        start = 2;
        protected_components = is_extended_unc_root ? 4 : 2;
    } else if (slash_path.starts_with('/')) {
        prefix = "/";
        start = 1;
    }

    std::vector<std::string_view> components;
    while (start <= slash_path.size()) {
        const auto end = slash_path.find('/', start);
        const auto component = std::string_view{slash_path}.substr(start, end - start);
        if (!component.empty() && component != ".") {
            if (component == "..") {
                const auto is_macro = !components.empty() && components.back().find('$') != std::string_view::npos;
                if (components.size() > protected_components && components.back() != ".." && !is_macro) {
                    components.pop_back();
                } else if (prefix.empty() || drive_relative || is_macro ||
                           (components.size() > protected_components && components.back() == "..")) {
                    components.push_back(component);
                }
            } else {
                components.push_back(component);
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }

    std::string normalized = prefix;
    for (const auto component : components) {
        if (!normalized.empty() && !normalized.ends_with('/') &&
            !(drive_relative && normalized == prefix)) {
            normalized += '/';
        }
        normalized += component;
    }
    if (normalized.empty() && !slash_path.empty()) return ".";
    return normalized;
}

[[nodiscard]] std::string normalize_glob(std::string value) {
    std::ranges::replace(value, '\\', '/');
    return value;
}

[[nodiscard]] Language normalize_language(std::string_view language) {
    return language == "C" ? Language::c : Language::cxx;
}

[[nodiscard]] TargetType normalize_target_type(RawTargetType type) {
    switch (type) {
    case RawTargetType::executable: return TargetType::executable;
    case RawTargetType::static_library: return TargetType::static_library;
    case RawTargetType::shared_library: return TargetType::shared_library;
    case RawTargetType::interface_library: return TargetType::interface_library;
    }
    std::unreachable();
}

[[nodiscard]] SourceEntry normalize_source(const RawSource& raw) {
    SourceEntry source{normalize_path(raw.path.value), raw.recursive.value, {}, SourceKind::automatic};
    source.exclude.reserve(raw.exclude.size());
    for (const auto& exclude : raw.exclude) source.exclude.push_back(normalize_glob(exclude.value));
    return source;
}

template <typename T, typename Transform>
[[nodiscard]] ScopedValues<T> normalize_scoped(const RawScopedValues<std::string>& raw,
                                                Transform transform) {
    auto normalize_values = [&transform](const auto& values) {
        std::vector<T> normalized;
        normalized.reserve(values.size());
        for (const auto& value : values) normalized.push_back(transform(value.value));
        return normalized;
    };
    return {normalize_values(raw.private_values), normalize_values(raw.public_values),
            normalize_values(raw.interface_values)};
}

[[nodiscard]] LinkDependency normalize_dependency(const std::string& value,
                                                  const RawSpec& raw) {
    const auto is_project_target = std::ranges::any_of(
        raw.targets, [&value](const RawTarget& target) { return target.name == value; });
    return {is_project_target ? LinkDependency::Kind::project_target : LinkDependency::Kind::imported_target,
            value};
}

[[nodiscard]] TargetOverride normalize_override(const RawTargetOverride& raw, const RawSpec& spec) {
    TargetOverride result;
    result.sources.reserve(raw.sources.size());
    for (const auto& source : raw.sources) result.sources.push_back(normalize_source(source));
    result.include_directories = normalize_scoped<std::string>(
        raw.include_directories, [](const std::string& value) { return normalize_path(value); });
    result.definitions = normalize_scoped<std::string>(raw.definitions, [](const std::string& value) { return value; });
    result.compile_options = normalize_scoped<std::string>(raw.compile_options, [](const std::string& value) { return value; });
    result.dependencies = normalize_scoped<LinkDependency>(
        raw.dependencies, [&spec](const std::string& value) { return normalize_dependency(value, spec); });
    return result;
}

[[nodiscard]] Target normalize_target(const RawTarget& raw, const RawSpec& spec) {
    Target result;
    result.name = raw.name;
    result.type = normalize_target_type(raw.type.value);
    result.sources.reserve(raw.sources.size());
    for (const auto& source : raw.sources) result.sources.push_back(normalize_source(source));
    result.include_directories = normalize_scoped<std::string>(
        raw.include_directories, [](const std::string& value) { return normalize_path(value); });
    result.definitions = normalize_scoped<std::string>(raw.definitions, [](const std::string& value) { return value; });
    result.compile_options = normalize_scoped<std::string>(raw.compile_options, [](const std::string& value) { return value; });
    result.dependencies = normalize_scoped<LinkDependency>(
        raw.dependencies, [&spec](const std::string& value) { return normalize_dependency(value, spec); });
    if (raw.macos) result.platform.macos = normalize_override(*raw.macos, spec);
    if (raw.linux) result.platform.linux = normalize_override(*raw.linux, spec);
    if (raw.windows) result.platform.windows = normalize_override(*raw.windows, spec);
    return result;
}

} // namespace

Project normalize_project(const RawSpec& raw) {
    Project project;
    project.schema_version = raw.schema_version.value;
    project.name = raw.name.value;
    if (raw.version) project.version = raw.version->value;

    if (raw.cmake.minimum_version) project.cmake.minimum_version = raw.cmake.minimum_version->value;
    if (!raw.cmake.languages.empty()) {
        project.cmake.languages.clear();
        for (const auto& language : raw.cmake.languages) project.cmake.languages.push_back(normalize_language(language.value));
        std::ranges::sort(project.cmake.languages, {}, [](Language language) { return static_cast<int>(language); });
        project.cmake.languages.erase(std::unique(project.cmake.languages.begin(), project.cmake.languages.end()),
                                      project.cmake.languages.end());
    }
    if (raw.cmake.c_standard) project.cmake.c_standard = std::to_string(raw.cmake.c_standard->value);
    if (raw.cmake.cxx_standard) project.cmake.cxx_standard = std::to_string(raw.cmake.cxx_standard->value);
    project.cmake.extensions = raw.cmake.extensions.value;
    for (const auto& include : raw.cmake.include_files)
        project.cmake.include_files.push_back(normalize_path(include.value));

    for (const auto& raw_package : raw.packages) {
        Package package{raw_package.name.value, std::nullopt, raw_package.required.value, {}};
        if (raw_package.version) package.version = raw_package.version->value;
        for (const auto& component : raw_package.components) package.components.push_back(component.value);
        project.packages.emplace(raw_package.alias, std::move(package));
    }
    for (const auto& raw_target : raw.targets)
        project.targets.emplace(raw_target.name, normalize_target(raw_target, raw));

    const auto add_preset = [&project](std::string name, std::string build_type) {
        Preset preset{std::move(name), std::move(build_type), std::nullopt, {}, {}};
        preset.binary_dir = "build/" + preset.name;
        project.presets.emplace(preset.name, std::move(preset));
    };
    if (!raw.presets) {
        add_preset("debug", "Debug");
        add_preset("release", "Release");
    } else {
        for (const auto& raw_preset : *raw.presets) {
            Preset preset{raw_preset.name, raw_preset.build_type.value, std::nullopt, {}, {}};
            if (raw_preset.generator) preset.generator = raw_preset.generator->value;
            preset.binary_dir = raw_preset.binary_dir ? normalize_path(raw_preset.binary_dir->value)
                                                       : "build/" + preset.name;
            for (std::size_t index = 0; index < raw_preset.cache_variable_names.size(); ++index)
                preset.cache_variables.emplace(raw_preset.cache_variable_names[index].value,
                                               raw_preset.cache_variable_values[index].value);
            project.presets.emplace(preset.name, std::move(preset));
        }
    }
    return project;
}

} // namespace cmakegen
