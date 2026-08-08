#include "cmakegen/parser/parser.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cmakegen {
namespace {

[[nodiscard]] std::optional<SourceLocation> location_of(const YAML::Node& node) {
    const auto mark = node.Mark();
    if (mark.is_null()) {
        return std::nullopt;
    }
    return SourceLocation{static_cast<std::size_t>(mark.line) + 1,
                          static_cast<std::size_t>(mark.column) + 1};
}

[[nodiscard]] bool is_implicit_non_string(std::string_view value) {
    static const std::regex implicit_non_string{
        R"(^(?:null|Null|NULL|~|true|True|TRUE|false|False|FALSE|[-+]?(?:[0-9][0-9]*|0o[0-7]+|0x[0-9a-fA-F]+)|[-+]?(?:(?:[0-9][0-9]*\.[0-9]*|\.[0-9]+)(?:[eE][-+]?[0-9]+)?|[0-9][0-9]*[eE][-+]?[0-9]+|\.(?:inf|Inf|INF))|\.(?:nan|NaN|NAN))$)"};
    return std::regex_match(value.begin(), value.end(), implicit_non_string);
}

[[nodiscard]] bool is_unsigned_integer_literal(std::string_view value) {
    static const std::regex unsigned_integer{
        R"(^\+?(?:[0-9][0-9]*|0o[0-7]+|0x[0-9a-fA-F]+)$)"};
    return std::regex_match(value.begin(), value.end(), unsigned_integer);
}

[[nodiscard]] bool is_boolean_literal(std::string_view value) {
    return value == "true" || value == "True" || value == "TRUE" ||
           value == "false" || value == "False" || value == "FALSE";
}

[[nodiscard]] bool has_string_semantics(const YAML::Node& node) {
    if (!node.IsScalar()) {
        return false;
    }
    const auto& tag = node.Tag();
    if (tag == "!" || tag == "tag:yaml.org,2002:str") {
        return true;
    }
    if (tag != "?") {
        return false;
    }
    return !is_implicit_non_string(node.Scalar());
}

void add_invalid(DiagnosticSet& diagnostics, std::string path, const YAML::Node& node,
                 std::string message) {
    diagnostics.add({std::string{diagnostic_codes::invalid_value}, DiagnosticSeverity::error,
                     std::move(message), std::move(path), location_of(node)});
}

void require_map(const YAML::Node& node, std::string_view path, DiagnosticSet& diagnostics) {
    if (!node.IsMap()) {
        add_invalid(diagnostics, std::string{path}, node, "expected a mapping");
    }
}

[[nodiscard]] bool is_allowed(std::string_view key, std::initializer_list<std::string_view> allowed) {
    return std::ranges::find(allowed, key) != allowed.end();
}

void reject_unknown_keys(const YAML::Node& node, std::string_view path,
                         std::initializer_list<std::string_view> allowed,
                         DiagnosticSet& diagnostics) {
    if (!node.IsMap()) {
        return;
    }
    std::unordered_set<std::string> seen_keys;
    for (const auto& item : node) {
        if (!has_string_semantics(item.first)) {
            add_invalid(diagnostics, std::string{path}, item.first, "mapping keys must be strings");
            continue;
        }
        const auto key = item.first.Scalar();
        const auto key_path = path.empty() ? key : std::string{path} + "." + key;
        if (!seen_keys.insert(key).second) {
            add_invalid(diagnostics, key_path, item.first, "duplicate key '" + key + "'");
            continue;
        }
        if (!is_allowed(key, allowed)) {
            diagnostics.add({std::string{diagnostic_codes::unknown_key}, DiagnosticSeverity::error,
                             "unknown key '" + key + "'", key_path, location_of(item.first)});
        }
    }
}

void reject_duplicate_keys(const YAML::Node& node, std::string_view path,
                           DiagnosticSet& diagnostics) {
    if (!node.IsMap()) {
        return;
    }
    std::unordered_set<std::string> seen_keys;
    for (const auto& item : node) {
        if (!has_string_semantics(item.first)) {
            continue;
        }
        const auto key = item.first.Scalar();
        if (!seen_keys.insert(key).second) {
            const auto key_path = path.empty() ? key : std::string{path} + "." + key;
            add_invalid(diagnostics, key_path, item.first, "duplicate key '" + key + "'");
        }
    }
}

[[nodiscard]] std::optional<RawValue<std::string>> parse_string(const YAML::Node& node,
                                                                  std::string path,
                                                                  DiagnosticSet& diagnostics) {
    if (!has_string_semantics(node)) {
        add_invalid(diagnostics, path, node, "expected a string");
        return std::nullopt;
    }
    return RawValue<std::string>{node.Scalar(), std::move(path), location_of(node)};
}

[[nodiscard]] std::optional<RawValue<std::uint32_t>> parse_uint32(const YAML::Node& node,
                                                                    std::string path,
                                                                    DiagnosticSet& diagnostics) {
    if (!node.IsScalar() || (node.Tag() != "?" && node.Tag() != "tag:yaml.org,2002:int") ||
        !is_unsigned_integer_literal(node.Scalar())) {
        add_invalid(diagnostics, path, node, "expected an unsigned integer");
        return std::nullopt;
    }
    auto value = node.Scalar();
    auto digits = std::string_view{value};
    auto base = 10;
    if (digits.starts_with('+')) {
        digits.remove_prefix(1);
    }
    if (digits.starts_with("0o")) {
        base = 8;
        digits.remove_prefix(2);
    } else if (digits.starts_with("0x")) {
        base = 16;
        digits.remove_prefix(2);
    }
    std::uint32_t parsed_value{};
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), parsed_value, base);
    if (error != std::errc{} || end != digits.data() + digits.size()) {
        add_invalid(diagnostics, path, node, "expected an unsigned integer");
        return std::nullopt;
    }
    return RawValue<std::uint32_t>{parsed_value, std::move(path), location_of(node)};
}

[[nodiscard]] std::optional<RawValue<bool>> parse_bool(const YAML::Node& node, std::string path,
                                                        DiagnosticSet& diagnostics) {
    if (!node.IsScalar() || (node.Tag() != "?" && node.Tag() != "tag:yaml.org,2002:bool") ||
        !is_boolean_literal(node.Scalar())) {
        add_invalid(diagnostics, path, node, "expected a boolean");
        return std::nullopt;
    }
    try {
        return RawValue<bool>{node.as<bool>(), std::move(path), location_of(node)};
    } catch (const YAML::BadConversion&) {
        add_invalid(diagnostics, path, node, "expected a boolean");
        return std::nullopt;
    }
}

void parse_string_sequence(const YAML::Node& node, std::string_view path,
                           std::vector<RawValue<std::string>>& destination,
                           DiagnosticSet& diagnostics) {
    if (!node.IsSequence()) {
        add_invalid(diagnostics, std::string{path}, node, "expected a sequence of strings");
        return;
    }
    for (std::size_t index = 0; index < node.size(); ++index) {
        if (auto value = parse_string(node[index], std::string{path} + "[" + std::to_string(index) + "]", diagnostics)) {
            destination.push_back(std::move(*value));
        }
    }
}

void parse_languages(const YAML::Node& node, std::vector<RawValue<std::string>>& destination,
                     DiagnosticSet& diagnostics) {
    std::vector<RawValue<std::string>> values;
    parse_string_sequence(node, "cmake.languages", values, diagnostics);
    if (node.IsSequence() && values.empty()) {
        add_invalid(diagnostics, "cmake.languages", node, "must not be empty");
    }
    for (auto& value : values) {
        if (value.value != "C" && value.value != "CXX") {
            diagnostics.add({std::string{diagnostic_codes::invalid_value}, DiagnosticSeverity::error,
                             "unsupported language '" + value.value + "'", value.path, value.location});
        } else {
            destination.push_back(std::move(value));
        }
    }
}

void parse_scoped_strings(const YAML::Node& node, std::string_view path,
                          RawScopedValues<std::string>& destination,
                          DiagnosticSet& diagnostics) {
    if (node.IsSequence()) {
        parse_string_sequence(node, path, destination.private_values, diagnostics);
        return;
    }
    if (!node.IsMap()) {
        add_invalid(diagnostics, std::string{path}, node,
                    "expected a sequence of strings or scoped mapping");
        return;
    }
    reject_unknown_keys(node, path, {"private", "public", "interface"}, diagnostics);
    if (const auto values = node["private"]) {
        parse_string_sequence(values, std::string{path} + ".private", destination.private_values,
                              diagnostics);
    }
    if (const auto values = node["public"]) {
        parse_string_sequence(values, std::string{path} + ".public", destination.public_values,
                              diagnostics);
    }
    if (const auto values = node["interface"]) {
        parse_string_sequence(values, std::string{path} + ".interface", destination.interface_values,
                              diagnostics);
    }
}

[[nodiscard]] std::optional<RawSource> parse_source(const YAML::Node& node, std::string path,
                                                     DiagnosticSet& diagnostics) {
    if (node.IsScalar()) {
        auto source_path = parse_string(node, path, diagnostics);
        if (!source_path) {
            return std::nullopt;
        }
        return RawSource{std::move(*source_path), RawValue<bool>{true, path + ".recursive", location_of(node)}, {}};
    }
    if (!node.IsMap()) {
        add_invalid(diagnostics, path, node, "expected a source path string or mapping");
        return std::nullopt;
    }
    reject_unknown_keys(node, path, {"path", "recursive", "exclude"}, diagnostics);
    const auto path_node = node["path"];
    if (!path_node) {
        add_invalid(diagnostics, path + ".path", node, "missing required field 'path'");
        return std::nullopt;
    }
    auto source_path = parse_string(path_node, path + ".path", diagnostics);
    if (!source_path) {
        return std::nullopt;
    }
    RawSource source{std::move(*source_path), RawValue<bool>{true, path + ".recursive", location_of(node)}, {}};
    if (const auto recursive = node["recursive"]) {
        if (auto value = parse_bool(recursive, path + ".recursive", diagnostics)) {
            source.recursive = std::move(*value);
        }
    }
    if (const auto exclude = node["exclude"]) {
        parse_string_sequence(exclude, path + ".exclude", source.exclude, diagnostics);
    }
    return source;
}

[[nodiscard]] std::optional<RawTargetType> parse_target_type(const YAML::Node& node,
                                                              std::string_view path,
                                                              DiagnosticSet& diagnostics) {
    const auto type = parse_string(node, std::string{path}, diagnostics);
    if (!type) {
        return std::nullopt;
    }
    if (type->value == "executable") return RawTargetType::executable;
    if (type->value == "staticLibrary") return RawTargetType::static_library;
    if (type->value == "sharedLibrary") return RawTargetType::shared_library;
    if (type->value == "interfaceLibrary") return RawTargetType::interface_library;
    diagnostics.add({std::string{diagnostic_codes::unknown_target_type}, DiagnosticSeverity::error,
                     "unknown target type '" + type->value + "'", std::string{path}, type->location});
    return std::nullopt;
}

void parse_sources(const YAML::Node& node, std::string_view path,
                   std::vector<RawSource>& destination, DiagnosticSet& diagnostics) {
    if (!node.IsSequence()) {
        add_invalid(diagnostics, std::string{path}, node, "expected a sequence");
        return;
    }
    for (std::size_t index = 0; index < node.size(); ++index) {
        if (auto source = parse_source(node[index], std::string{path} + "[" + std::to_string(index) + "]",
                                       diagnostics)) {
            destination.push_back(std::move(*source));
        }
    }
}

[[nodiscard]] RawTargetOverride parse_target_override(const YAML::Node& node, std::string_view path,
                                                       DiagnosticSet& diagnostics) {
    RawTargetOverride override;
    require_map(node, path, diagnostics);
    if (!node.IsMap()) return override;
    reject_unknown_keys(node, path,
                        {"sources", "includeDirectories", "definitions", "compileOptions", "dependencies"},
                        diagnostics);
    if (const auto values = node["sources"]) parse_sources(values, std::string{path} + ".sources", override.sources, diagnostics);
    if (const auto values = node["includeDirectories"]) parse_scoped_strings(values, std::string{path} + ".includeDirectories", override.include_directories, diagnostics);
    if (const auto values = node["definitions"]) parse_scoped_strings(values, std::string{path} + ".definitions", override.definitions, diagnostics);
    if (const auto values = node["compileOptions"]) parse_scoped_strings(values, std::string{path} + ".compileOptions", override.compile_options, diagnostics);
    if (const auto values = node["dependencies"]) parse_scoped_strings(values, std::string{path} + ".dependencies", override.dependencies, diagnostics);
    return override;
}

[[nodiscard]] bool is_valid_project_name(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    return std::ranges::all_of(name, [](unsigned char character) {
        return std::isalnum(character) || character == '_' || character == '.' ||
               character == '+' || character == '-';
    });
}

[[nodiscard]] bool is_valid_preset_name(std::string_view name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find_first_of("/\\\\$") == std::string_view::npos;
}

} // namespace

Result<RawSpec> parse_raw_spec(std::string_view yaml) {
    DiagnosticSet diagnostics;
    YAML::Node root;
    try {
        auto documents = YAML::LoadAll(std::string{yaml});
        if (documents.size() != 1) {
            const auto location = documents.size() > 1 ? location_of(documents[1]) : std::nullopt;
            diagnostics.add({std::string{diagnostic_codes::invalid_value}, DiagnosticSeverity::error,
                             "expected exactly one YAML document", "", location});
            return std::unexpected(std::move(diagnostics));
        }
        root = std::move(documents.front());
    } catch (const YAML::ParserException& error) {
        std::optional<SourceLocation> location;
        if (!error.mark.is_null()) {
            location = SourceLocation{static_cast<std::size_t>(error.mark.line) + 1,
                                      static_cast<std::size_t>(error.mark.column) + 1};
        }
        diagnostics.add({std::string{diagnostic_codes::invalid_value}, DiagnosticSeverity::error,
                         error.msg, "", location});
        return std::unexpected(std::move(diagnostics));
    }

    require_map(root, "", diagnostics);
    if (!root.IsMap()) {
        return std::unexpected(std::move(diagnostics));
    }
    reject_unknown_keys(root, "", {"schemaVersion", "name", "version", "cmake", "packages", "targets", "presets"}, diagnostics);

    const auto schema_node = root["schemaVersion"];
    const auto name_node = root["name"];
    const auto targets_node = root["targets"];
    if (!schema_node) add_invalid(diagnostics, "schemaVersion", root, "missing required field 'schemaVersion'");
    if (!name_node) add_invalid(diagnostics, "name", root, "missing required field 'name'");
    if (!targets_node) add_invalid(diagnostics, "targets", root, "missing required field 'targets'");

    std::optional<RawValue<std::uint32_t>> schema_version;
    if (schema_node) {
        schema_version = parse_uint32(schema_node, "schemaVersion", diagnostics);
        if (schema_version && schema_version->value != 1) {
            diagnostics.add({std::string{diagnostic_codes::invalid_schema_version}, DiagnosticSeverity::error,
                             "unsupported schema version " + std::to_string(schema_version->value),
                             "schemaVersion", schema_version->location});
        }
    }
    auto name = name_node ? parse_string(name_node, "name", diagnostics) : std::nullopt;
    if (name && !is_valid_project_name(name->value)) {
        add_invalid(diagnostics, "name", name_node, "must match ^[A-Za-z0-9_.+-]+$");
    }
    std::optional<RawValue<std::string>> version;
    if (const auto node = root["version"]) version = parse_string(node, "version", diagnostics);

    RawCmake cmake;
    cmake.languages.push_back({"CXX", "cmake.languages[0]", std::nullopt});
    if (const auto cmake_node = root["cmake"]) {
        require_map(cmake_node, "cmake", diagnostics);
        if (cmake_node.IsMap()) {
            reject_unknown_keys(cmake_node, "cmake", {"minimumVersion", "languages", "cStandard", "cxxStandard", "extensions", "include"}, diagnostics);
            if (const auto node = cmake_node["minimumVersion"]) cmake.minimum_version = parse_string(node, "cmake.minimumVersion", diagnostics);
            if (const auto node = cmake_node["languages"]) {
                cmake.languages.clear();
                parse_languages(node, cmake.languages, diagnostics);
            }
            if (const auto node = cmake_node["cStandard"]) cmake.c_standard = parse_uint32(node, "cmake.cStandard", diagnostics);
            if (const auto node = cmake_node["cxxStandard"]) cmake.cxx_standard = parse_uint32(node, "cmake.cxxStandard", diagnostics);
            if (const auto node = cmake_node["extensions"]) {
                if (auto value = parse_bool(node, "cmake.extensions", diagnostics)) cmake.extensions = std::move(*value);
            }
            if (const auto node = cmake_node["include"]) parse_string_sequence(node, "cmake.include", cmake.include_files, diagnostics);
        }
    }

    std::vector<RawPackage> packages;
    if (const auto packages_node = root["packages"]) {
        require_map(packages_node, "packages", diagnostics);
        if (packages_node.IsMap()) reject_duplicate_keys(packages_node, "packages", diagnostics);
        if (packages_node.IsMap()) for (const auto& item : packages_node) {
            if (!has_string_semantics(item.first)) {
                add_invalid(diagnostics, "packages", item.first, "package aliases must be strings");
                continue;
            }
            const auto alias = item.first.Scalar();
            const auto package_path = "packages." + alias;
            require_map(item.second, package_path, diagnostics);
            if (!item.second.IsMap()) continue;
            reject_unknown_keys(item.second, package_path, {"name", "version", "required", "components"}, diagnostics);
            const auto name_node = item.second["name"];
            if (!name_node) {
                add_invalid(diagnostics, package_path + ".name", item.second, "missing required field 'name'");
                continue;
            }
            auto package_name = parse_string(name_node, package_path + ".name", diagnostics);
            if (!package_name) continue;
            RawPackage package{alias, package_path, location_of(item.first), std::move(*package_name), std::nullopt,
                               RawValue<bool>{true, package_path + ".required", location_of(item.second)}, {}};
            if (const auto node = item.second["version"]) package.version = parse_string(node, package_path + ".version", diagnostics);
            if (const auto node = item.second["required"]) {
                if (auto value = parse_bool(node, package_path + ".required", diagnostics)) package.required = std::move(*value);
            }
            if (const auto node = item.second["components"]) parse_string_sequence(node, package_path + ".components", package.components, diagnostics);
            packages.push_back(std::move(package));
        }
    }

    std::vector<RawTarget> targets;
    if (targets_node) {
        require_map(targets_node, "targets", diagnostics);
        if (targets_node.IsMap() && targets_node.size() == 0) add_invalid(diagnostics, "targets", targets_node, "must not be empty");
        if (targets_node.IsMap()) {
            reject_duplicate_keys(targets_node, "targets", diagnostics);
        }
        if (targets_node.IsMap()) for (const auto& item : targets_node) {
            if (!has_string_semantics(item.first)) {
                add_invalid(diagnostics, "targets", item.first, "target names must be strings");
                continue;
            }
            const auto target_name = item.first.Scalar();
            const auto target_path = "targets." + target_name;
            require_map(item.second, target_path, diagnostics);
            if (!item.second.IsMap()) {
                continue;
            }
            reject_unknown_keys(item.second, target_path,
                                {"type", "sources", "includeDirectories", "definitions", "compileOptions", "dependencies", "platform"}, diagnostics);
            const auto type_node = item.second["type"];
            if (!type_node) {
                add_invalid(diagnostics, target_path + ".type", item.second, "missing required field 'type'");
                continue;
            }
            const auto type = parse_target_type(type_node, target_path + ".type", diagnostics);
            if (!type) continue;
            RawTarget target{target_name, target_path, location_of(item.first),
                             RawValue<RawTargetType>{*type, target_path + ".type", location_of(type_node)}, {}, {}, {}, {}, {}, std::nullopt, std::nullopt, std::nullopt};
            if (const auto sources = item.second["sources"]) {
                parse_sources(sources, target_path + ".sources", target.sources, diagnostics);
            }
            if (const auto values = item.second["includeDirectories"]) parse_scoped_strings(values, target_path + ".includeDirectories", target.include_directories, diagnostics);
            if (const auto values = item.second["definitions"]) parse_scoped_strings(values, target_path + ".definitions", target.definitions, diagnostics);
            if (const auto values = item.second["compileOptions"]) parse_scoped_strings(values, target_path + ".compileOptions", target.compile_options, diagnostics);
            if (const auto values = item.second["dependencies"]) parse_scoped_strings(values, target_path + ".dependencies", target.dependencies, diagnostics);
            if (const auto platform = item.second["platform"]) {
                const auto platform_path = target_path + ".platform";
                require_map(platform, platform_path, diagnostics);
                if (platform.IsMap()) {
                    reject_unknown_keys(platform, platform_path, {"macos", "linux", "windows"}, diagnostics);
                    if (const auto values = platform["macos"]) target.macos = parse_target_override(values, platform_path + ".macos", diagnostics);
                    if (const auto values = platform["linux"]) target.linux = parse_target_override(values, platform_path + ".linux", diagnostics);
                    if (const auto values = platform["windows"]) target.windows = parse_target_override(values, platform_path + ".windows", diagnostics);
                }
            }
            targets.push_back(std::move(target));
        }
    }

    std::optional<std::vector<RawPreset>> presets;
    if (const auto presets_node = root["presets"]) {
        presets.emplace();
        require_map(presets_node, "presets", diagnostics);
        if (presets_node.IsMap()) reject_duplicate_keys(presets_node, "presets", diagnostics);
        if (presets_node.IsMap()) for (const auto& item : presets_node) {
            if (!has_string_semantics(item.first)) {
                add_invalid(diagnostics, "presets", item.first, "preset names must be strings");
                continue;
            }
            const auto preset_name = item.first.Scalar();
            const auto preset_path = "presets." + preset_name;
            if (!is_valid_preset_name(preset_name)) {
                add_invalid(diagnostics, preset_path, item.first,
                            "must not contain path separators or '$', or be '.' or '..'");
                continue;
            }
            require_map(item.second, preset_path, diagnostics);
            if (!item.second.IsMap()) continue;
            reject_unknown_keys(item.second, preset_path, {"buildType", "generator", "binaryDir", "cacheVariables"}, diagnostics);
            const auto build_type_node = item.second["buildType"];
            if (!build_type_node) {
                add_invalid(diagnostics, preset_path + ".buildType", item.second, "missing required field 'buildType'");
                continue;
            }
            auto build_type = parse_string(build_type_node, preset_path + ".buildType", diagnostics);
            if (!build_type) continue;
            RawPreset preset{preset_name, preset_path, location_of(item.first), std::move(*build_type), std::nullopt, std::nullopt, {}, {}};
            if (const auto node = item.second["generator"]) preset.generator = parse_string(node, preset_path + ".generator", diagnostics);
            if (const auto node = item.second["binaryDir"]) preset.binary_dir = parse_string(node, preset_path + ".binaryDir", diagnostics);
            if (const auto variables = item.second["cacheVariables"]) {
                require_map(variables, preset_path + ".cacheVariables", diagnostics);
                if (variables.IsMap()) {
                    reject_duplicate_keys(variables, preset_path + ".cacheVariables", diagnostics);
                    for (const auto& variable : variables) {
                        if (!has_string_semantics(variable.first)) {
                            add_invalid(diagnostics, preset_path + ".cacheVariables", variable.first, "cache variable names must be strings");
                            continue;
                        }
                        const auto variable_name = variable.first.Scalar();
                        const auto variable_path = preset_path + ".cacheVariables." + variable_name;
                        auto value = parse_string(variable.second, variable_path, diagnostics);
                        if (!value) continue;
                        preset.cache_variable_names.push_back({variable_name, variable_path, location_of(variable.first)});
                        preset.cache_variable_values.push_back(std::move(*value));
                    }
                }
            }
            presets->push_back(std::move(preset));
        }
    }

    if (diagnostics.has_errors() || !schema_version || !name) return std::unexpected(std::move(diagnostics));
    return RawSpec{std::move(*schema_version), std::move(*name), std::move(version), std::move(cmake), std::move(packages), std::move(targets), std::move(presets)};
}

} // namespace cmakegen
