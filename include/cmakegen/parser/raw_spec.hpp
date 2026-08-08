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
};

struct RawTarget {
    std::string name;
    std::string path;
    std::optional<SourceLocation> location;
    RawValue<RawTargetType> type;
    std::vector<RawSource> sources;
};

struct RawSpec {
    RawValue<std::uint32_t> schema_version;
    RawValue<std::string> name;
    std::optional<RawValue<std::string>> version;
    RawCmake cmake;
    std::vector<RawTarget> targets;
};

} // namespace cmakegen
