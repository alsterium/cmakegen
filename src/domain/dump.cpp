#include "cmakegen/domain/dump.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <sstream>
#include <string_view>
#include <utility>

namespace cmakegen {
namespace {

using json = nlohmann::ordered_json;

[[nodiscard]] std::string name_of(Language value) {
    return value == Language::c ? "C" : "CXX";
}

[[nodiscard]] std::string name_of(TargetType value) {
    switch (value) {
    case TargetType::executable: return "executable";
    case TargetType::static_library: return "staticLibrary";
    case TargetType::shared_library: return "sharedLibrary";
    case TargetType::interface_library: return "interfaceLibrary";
    }
    std::unreachable();
}

[[nodiscard]] std::string name_of(SourceKind value) {
    switch (value) {
    case SourceKind::automatic: return "auto";
    case SourceKind::source: return "source";
    case SourceKind::header: return "header";
    }
    std::unreachable();
}

[[nodiscard]] json dump_dependency(const LinkDependency& value) {
    return {{"kind", value.kind == LinkDependency::Kind::project_target ? "projectTarget" : "importedTarget"},
            {"name", value.name}};
}

template <typename T, typename Dump>
[[nodiscard]] json dump_scoped(const ScopedValues<T>& values, Dump dump) {
    const auto dump_values = [&dump](const auto& entries) {
        json result = json::array();
        for (const auto& entry : entries) result.push_back(dump(entry));
        return result;
    };
    return {{"private", dump_values(values.private_values)},
            {"public", dump_values(values.public_values)},
            {"interface", dump_values(values.interface_values)}};
}

[[nodiscard]] json dump_source(const SourceEntry& source) {
    return {{"path", source.path}, {"recursive", source.recursive}, {"exclude", source.exclude},
            {"kind", name_of(source.kind)}};
}

[[nodiscard]] json dump_override(const TargetOverride& value) {
    json sources = json::array();
    for (const auto& source : value.sources) sources.push_back(dump_source(source));
    return {{"sources", sources},
            {"includeDirectories", dump_scoped(value.include_directories, [](const auto& item) { return item; })},
            {"definitions", dump_scoped(value.definitions, [](const auto& item) { return item; })},
            {"compileOptions", dump_scoped(value.compile_options, [](const auto& item) { return item; })},
            {"dependencies", dump_scoped(value.dependencies, dump_dependency)}};
}

[[nodiscard]] json dump_project(const Project& project) {
    json cmake{{"minimumVersion", project.cmake.minimum_version},
               {"languages", json::array()},
               {"cStandard", project.cmake.c_standard},
               {"cxxStandard", project.cmake.cxx_standard},
               {"extensions", project.cmake.extensions},
               {"include", project.cmake.include_files}};
    for (const auto language : project.cmake.languages) cmake["languages"].push_back(name_of(language));

    json packages = json::object();
    for (const auto& [alias, package] : project.packages)
        packages[alias] = {{"name", package.name}, {"version", package.version}, {"required", package.required},
                           {"components", package.components}};

    json targets = json::object();
    for (const auto& [name, target] : project.targets) {
        json sources = json::array();
        for (const auto& source : target.sources) sources.push_back(dump_source(source));
        targets[name] = {{"name", target.name},
                         {"type", name_of(target.type)},
                         {"sources", sources},
                         {"includeDirectories", dump_scoped(target.include_directories, [](const auto& item) { return item; })},
                         {"definitions", dump_scoped(target.definitions, [](const auto& item) { return item; })},
                         {"compileOptions", dump_scoped(target.compile_options, [](const auto& item) { return item; })},
                         {"dependencies", dump_scoped(target.dependencies, dump_dependency)},
                         {"platform", {{"macos", dump_override(target.platform.macos)},
                                       {"linux", dump_override(target.platform.linux)},
                                       {"windows", dump_override(target.platform.windows)}}}};
    }

    json presets = json::object();
    for (const auto& [name, preset] : project.presets)
        presets[name] = {{"name", preset.name}, {"buildType", preset.build_type}, {"generator", preset.generator},
                         {"binaryDir", preset.binary_dir}, {"cacheVariables", preset.cache_variables}};
    return {{"schemaVersion", project.schema_version}, {"name", project.name}, {"version", project.version},
            {"cmake", cmake}, {"targets", targets}, {"packages", packages}, {"presets", presets}};
}

[[nodiscard]] std::string quote_yaml(std::string_view value) {
    constexpr std::array hex_digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                    '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    std::string quoted{"\""};
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto character = value[index];
        if (static_cast<unsigned char>(character) == 0xC2 && index + 1 < value.size() &&
            static_cast<unsigned char>(value[index + 1]) == 0x85) {
            quoted += "\\N";
            ++index;
            continue;
        }
        switch (character) {
        case '\0': quoted += "\\0"; break;
        case '\a': quoted += "\\a"; break;
        case '\b': quoted += "\\b"; break;
        case '\t': quoted += "\\t"; break;
        case '\n': quoted += "\\n"; break;
        case '\v': quoted += "\\v"; break;
        case '\f': quoted += "\\f"; break;
        case '\r': quoted += "\\r"; break;
        case '\x1B': quoted += "\\e"; break;
        case '\\': quoted += "\\\\"; break;
        case '\"': quoted += "\\\""; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20 ||
                static_cast<unsigned char>(character) == 0x7F) {
                const auto byte = static_cast<unsigned char>(character);
                quoted += "\\x";
                quoted += hex_digits[byte >> 4];
                quoted += hex_digits[byte & 0x0F];
            } else {
                quoted += character;
            }
        }
    }
    return quoted + '\"';
}

void write_yaml(const json& value, std::ostringstream& output, int indent) {
    const std::string padding(static_cast<std::size_t>(indent), ' ');
    if (value.is_object()) {
        for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
            output << padding << quote_yaml(iterator.key()) << ':';
            if (iterator.value().is_object() && iterator.value().empty()) {
                output << " {}\n";
            } else if (iterator.value().is_array() && iterator.value().empty()) {
                output << " []\n";
            } else if (iterator.value().is_object() || iterator.value().is_array()) {
                output << '\n';
                write_yaml(iterator.value(), output, indent + 2);
            } else if (iterator.value().is_null()) {
                output << " null\n";
            } else if (iterator.value().is_boolean()) {
                output << (iterator.value().get<bool>() ? " true\n" : " false\n");
            } else if (iterator.value().is_number()) {
                output << ' ' << iterator.value() << '\n';
            } else {
                output << ' ' << quote_yaml(iterator.value().get<std::string>()) << '\n';
            }
        }
    } else if (value.is_array()) {
        if (value.empty()) { output << padding << "[]\n"; return; }
        for (const auto& item : value) {
            output << padding << '-';
            if (item.is_object() && item.empty()) {
                output << " {}\n";
            } else if (item.is_array() && item.empty()) {
                output << " []\n";
            } else if (item.is_object() || item.is_array()) {
                output << '\n';
                write_yaml(item, output, indent + 2);
            } else if (item.is_null()) {
                output << " null\n";
            } else if (item.is_boolean()) {
                output << (item.get<bool>() ? " true\n" : " false\n");
            } else if (item.is_number()) {
                output << ' ' << item << '\n';
            } else {
                output << ' ' << quote_yaml(item.get<std::string>()) << '\n';
            }
        }
    }
}

} // namespace

std::string dump_project_json(const Project& project) {
    return dump_project(project).dump(2) + '\n';
}

std::string dump_project_yaml(const Project& project) {
    std::ostringstream output;
    write_yaml(dump_project(project), output, 0);
    return output.str();
}

} // namespace cmakegen
