#pragma once

#include "cmakegen/diagnostics/diagnostic.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cmakegen {

template <typename T>
struct RawValue {
    T value;
    std::string path;
    std::optional<SourceLocation> location;
};

enum class RawTargetType {
    executable,
    static_library,
    shared_library,
    interface_library,
};

enum class RawScope {
    private_,
    public_,
    interface_,
};

template <typename T>
struct RawScopedValues {
    std::vector<RawValue<T>> private_values;
    std::vector<RawValue<T>> public_values;
    std::vector<RawValue<T>> interface_values;
};

struct RawSource {
    RawValue<std::string> path;
    RawValue<bool> recursive{true, {}, std::nullopt};
    std::vector<RawValue<std::string>> exclude;
};

struct RawCmake {
    std::optional<RawValue<std::string>> minimum_version;
    std::vector<RawValue<std::string>> languages;
    std::optional<RawValue<std::uint32_t>> c_standard;
    std::optional<RawValue<std::uint32_t>> cxx_standard;
    RawValue<bool> extensions{false, {}, std::nullopt};
    std::vector<RawValue<std::string>> include_files;
};

struct RawPackage {
    std::string alias;
    std::string path;
    std::optional<SourceLocation> location;
    RawValue<std::string> name;
    std::optional<RawValue<std::string>> version;
    RawValue<bool> required{true, {}, std::nullopt};
    std::vector<RawValue<std::string>> components;
};

struct RawPreset {
    std::string name;
    std::string path;
    std::optional<SourceLocation> location;
    RawValue<std::string> build_type;
    std::optional<RawValue<std::string>> generator;
    std::optional<RawValue<std::string>> binary_dir;
    std::vector<RawValue<std::string>> cache_variable_names;
    std::vector<RawValue<std::string>> cache_variable_values;
};

struct RawTargetOverride {
    std::vector<RawSource> sources;
    RawScopedValues<std::string> include_directories;
    RawScopedValues<std::string> definitions;
    RawScopedValues<std::string> compile_options;
    RawScopedValues<std::string> dependencies;
};

struct RawTarget {
    std::string name;
    std::string path;
    std::optional<SourceLocation> location;
    RawValue<RawTargetType> type;
    std::vector<RawSource> sources;
    RawScopedValues<std::string> include_directories;
    RawScopedValues<std::string> definitions;
    RawScopedValues<std::string> compile_options;
    RawScopedValues<std::string> dependencies;
    std::optional<RawTargetOverride> macos;
    std::optional<RawTargetOverride> linux;
    std::optional<RawTargetOverride> windows;
};

struct RawSpec {
    RawValue<std::uint32_t> schema_version;
    RawValue<std::string> name;
    std::optional<RawValue<std::string>> version;
    RawCmake cmake;
    std::vector<RawPackage> packages;
    std::vector<RawTarget> targets;
    std::vector<RawPreset> presets;
};

} // namespace cmakegen
