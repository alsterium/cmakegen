#pragma once

#include "cmakegen/diagnostics/diagnostic.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
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
};

struct LinkDependency {
    enum class Kind { project_target, imported_target };

    Kind kind;
    std::string name;
};

struct TargetOverride {
    std::vector<SourceEntry> sources;
    ScopedValues<std::string> include_directories;
    ScopedValues<std::string> definitions;
    ScopedValues<std::string> compile_options;
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
    std::vector<SourceEntry> sources;
    ScopedValues<std::string> include_directories;
    ScopedValues<std::string> definitions;
    ScopedValues<std::string> compile_options;
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
