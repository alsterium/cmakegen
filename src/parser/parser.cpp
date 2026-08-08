#include "cmakegen/parser/parser.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
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

template <typename T>
[[nodiscard]] bool can_convert_to(const YAML::Node& node) {
    try {
        static_cast<void>(node.as<T>());
        return true;
    } catch (const YAML::BadConversion&) {
        return false;
    }
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
    return !can_convert_to<bool>(node) && !can_convert_to<long long>(node) &&
           !can_convert_to<double>(node);
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
    if (!node.IsScalar() || (node.Tag() != "?" && node.Tag() != "tag:yaml.org,2002:int")) {
        add_invalid(diagnostics, path, node, "expected an unsigned integer");
        return std::nullopt;
    }
    try {
        return RawValue<std::uint32_t>{node.as<std::uint32_t>(), std::move(path), location_of(node)};
    } catch (const YAML::BadConversion&) {
        add_invalid(diagnostics, path, node, "expected an unsigned integer");
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<RawValue<bool>> parse_bool(const YAML::Node& node, std::string path,
                                                        DiagnosticSet& diagnostics) {
    if (!node.IsScalar() || (node.Tag() != "?" && node.Tag() != "tag:yaml.org,2002:bool")) {
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

[[nodiscard]] bool is_valid_project_name(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    return std::ranges::all_of(name, [](unsigned char character) {
        return std::isalnum(character) || character == '_' || character == '.' ||
               character == '+' || character == '-';
    });
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
    reject_unknown_keys(root, "", {"schemaVersion", "name", "version", "cmake", "targets"}, diagnostics);

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
            reject_unknown_keys(cmake_node, "cmake", {"minimumVersion", "languages", "cStandard", "cxxStandard", "extensions"}, diagnostics);
            if (const auto node = cmake_node["minimumVersion"]) cmake.minimum_version = parse_string(node, "cmake.minimumVersion", diagnostics);
            if (const auto node = cmake_node["languages"]) {
                cmake.languages.clear();
                parse_string_sequence(node, "cmake.languages", cmake.languages, diagnostics);
            }
            if (const auto node = cmake_node["cStandard"]) cmake.c_standard = parse_uint32(node, "cmake.cStandard", diagnostics);
            if (const auto node = cmake_node["cxxStandard"]) cmake.cxx_standard = parse_uint32(node, "cmake.cxxStandard", diagnostics);
            if (const auto node = cmake_node["extensions"]) {
                if (auto value = parse_bool(node, "cmake.extensions", diagnostics)) cmake.extensions = std::move(*value);
            }
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
            reject_unknown_keys(item.second, target_path, {"type", "sources"}, diagnostics);
            const auto type_node = item.second["type"];
            if (!type_node) {
                add_invalid(diagnostics, target_path + ".type", item.second, "missing required field 'type'");
                continue;
            }
            const auto type = parse_target_type(type_node, target_path + ".type", diagnostics);
            if (!type) continue;
            RawTarget target{target_name, target_path, location_of(item.first),
                             RawValue<RawTargetType>{*type, target_path + ".type", location_of(type_node)}, {}};
            if (const auto sources = item.second["sources"]) {
                if (!sources.IsSequence()) add_invalid(diagnostics, target_path + ".sources", sources, "expected a sequence");
                else for (std::size_t index = 0; index < sources.size(); ++index) {
                    if (auto source = parse_source(sources[index], target_path + ".sources[" + std::to_string(index) + "]", diagnostics)) target.sources.push_back(std::move(*source));
                }
            }
            targets.push_back(std::move(target));
        }
    }

    if (diagnostics.has_errors() || !schema_version || !name) return std::unexpected(std::move(diagnostics));
    return RawSpec{std::move(*schema_version), std::move(*name), std::move(version), std::move(cmake), std::move(targets)};
}

} // namespace cmakegen
