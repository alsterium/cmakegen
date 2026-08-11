#pragma once

#include "cmakegen/diagnostics/diagnostic.hpp"

#include <concepts>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cmakegen {

enum class Language { c, cxx };
enum class TargetType { executable, static_library, shared_library, interface_library };
enum class SourceKind { automatic, source, header };

template <typename T>
struct ScopedValues {
    std::vector<T> private_values;
    std::vector<T> public_values;
    std::vector<T> interface_values;
};

// A normalized scoped string retains the YAML entry that produced its
// effective value.  The renderer consumes only value, while semantic
// validation can use the provenance to point at the exact offending entry.
template <typename T>
struct ScopedValue {
    T value;
    std::string diagnostic_path;
    std::optional<SourceLocation> diagnostic_location;

    ScopedValue() = default;
    ScopedValue(T value) : value(std::move(value)) {}
    template <typename U>
        requires std::constructible_from<T, U>
    ScopedValue(U&& value) : value(std::forward<U>(value)) {}
    ScopedValue(T value, std::string diagnostic_path,
                std::optional<SourceLocation> diagnostic_location)
        : value(std::move(value)), diagnostic_path(std::move(diagnostic_path)),
          diagnostic_location(diagnostic_location) {}

    ScopedValue& operator=(T new_value) {
        value = std::move(new_value);
        return *this;
    }
};

using ScopedStringValue = ScopedValue<std::string>;
using ScopedStringValues = ScopedValues<ScopedStringValue>;

struct SourceEntry {
    std::string path;
    bool recursive = true;
    std::vector<std::string> exclude;
    SourceKind kind = SourceKind::automatic;
    std::string diagnostic_path;
    std::optional<SourceLocation> diagnostic_location;
};

struct ResolvedSource {
    std::string path;
    SourceKind kind;
    // Keep the declaration provenance while expanding a source entry. Semantic
    // checks run after expansion, so this lets their diagnostics point back to
    // a platform override (rather than the target's common sources list).
    std::string diagnostic_path;
    std::optional<SourceLocation> diagnostic_location;
};

struct LinkDependency {
    enum class Kind { project_target, imported_target };

    Kind kind;
    std::string name;
    // Retain the parsed declaration so diagnostics produced from the effective
    // dependency graph identify the exact dependency that closes a cycle.
    std::string diagnostic_path;
    std::optional<SourceLocation> diagnostic_location;
};

struct TargetOverride {
    std::vector<SourceEntry> sources;
    ScopedStringValues include_directories;
    ScopedStringValues definitions;
    ScopedStringValues compile_options;
    ScopedValues<LinkDependency> dependencies;
};

struct PlatformOverrides {
    TargetOverride macos;
    TargetOverride linux;
    TargetOverride windows;
};

struct Target {
    std::string name;
    TargetType type;
    std::string diagnostic_path;
    std::optional<SourceLocation> diagnostic_location;
    std::vector<SourceEntry> sources;
    ScopedStringValues include_directories;
    ScopedStringValues definitions;
    ScopedStringValues compile_options;
    ScopedValues<LinkDependency> dependencies;
    PlatformOverrides platform;
};

struct CmakeConfig {
    std::string minimum_version = "3.28";
    std::vector<Language> languages{Language::cxx};
    std::optional<std::string> c_standard;
    std::optional<std::string> cxx_standard{"20"};
    bool extensions = false;
    std::vector<std::string> include_files;
};

struct Package {
    std::string name;
    std::optional<std::string> version;
    bool required = true;
    std::vector<std::string> components;
};

struct Preset {
    std::string name;
    std::string build_type;
    std::optional<std::string> generator;
    std::string binary_dir;
    std::map<std::string, std::string> cache_variables;
};

struct Project {
    std::uint32_t schema_version;
    std::string name;
    std::optional<std::string> version;
    CmakeConfig cmake;
    std::map<std::string, Target> targets;
    std::map<std::string, Package> packages;
    std::map<std::string, Preset> presets;
};

} // namespace cmakegen
