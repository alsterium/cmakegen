#include "cmakegen/generator/presets_renderer.hpp"

#include "cmakegen/generator/presets_compatibility.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <string_view>

namespace cmakegen {
namespace {

using Json = nlohmann::ordered_json;

[[nodiscard]] bool is_absolute_or_macro_path(const std::string_view path) {
    if (path.starts_with("${") || path.starts_with("$env{") || path.starts_with("$penv{") ||
        path.starts_with('/') || path.starts_with('\\')) return true;
    return path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':' && path[2] == '/';
}

[[nodiscard]] std::string preset_binary_directory(const Preset& preset) {
    if (is_absolute_or_macro_path(preset.binary_dir)) return preset.binary_dir;
    return "${sourceDir}/" + preset.binary_dir;
}

[[nodiscard]] Json render_configure_preset(const Preset& preset) {
    Json result = {
        {"name", preset.name},
    };
    if (preset.generator) result["generator"] = *preset.generator;
    result["binaryDir"] = preset_binary_directory(preset);

    Json cache_variables = Json::object();
    cache_variables["CMAKE_BUILD_TYPE"] = preset.build_type;
    for (const auto& [name, value] : preset.cache_variables) {
        if (name != "CMAKE_BUILD_TYPE") cache_variables[name] = value;
    }
    result["cacheVariables"] = std::move(cache_variables);
    return result;
}

[[nodiscard]] Json render_named_preset(const Preset& preset) {
    Json result = {{"name", preset.name}, {"configurePreset", preset.name}};
    // An omitted generator uses regular discovery, which may select Visual
    // Studio, Xcode, or Ninja Multi-Config.  configuration is harmless for
    // single-config generators and required for those multi-config cases.
    result["configuration"] = preset.build_type;
    return result;
}

} // namespace

std::string render_presets(const Project& project) {
    const auto [major, minor, patch] = cmake_version_components(project.cmake.minimum_version);
    Json document = {
        {"version", preset_schema_version_for(project.cmake.minimum_version)},
        {"cmakeMinimumRequired", {{"major", major}, {"minor", minor}, {"patch", patch}}},
    };

    Json configure_presets = Json::array();
    Json build_presets = Json::array();
    Json test_presets = Json::array();
    for (const auto& entry : project.presets) {
        const auto& preset = entry.second;
        configure_presets.push_back(render_configure_preset(preset));
        build_presets.push_back(render_named_preset(preset));
        test_presets.push_back(render_named_preset(preset));
    }
    document["configurePresets"] = std::move(configure_presets);
    document["buildPresets"] = std::move(build_presets);
    document["testPresets"] = std::move(test_presets);
    return document.dump(2) + '\n';
}

} // namespace cmakegen
