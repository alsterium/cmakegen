#include "cmakegen/domain/normalizer.hpp"
#include "cmakegen/generator/presets_compatibility.hpp"
#include "cmakegen/generator/presets_renderer.hpp"
#include "cmakegen/parser/parser.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <stdexcept>

using namespace cmakegen;

namespace {

[[nodiscard]] Project normalize(std::string_view yaml) {
    const auto raw = parse_raw_spec(yaml);
    REQUIRE(raw);
    return normalize_project(*raw);
}

} // namespace

TEST_CASE("presets renderer produces the documented default presets") {
    const auto project = normalize(R"(
schemaVersion: 1
name: example
targets:
  App:
    type: executable
)");

    const auto rendered = render_presets(project);
    CHECK(rendered == R"({
  "version": 8,
  "cmakeMinimumRequired": {
    "major": 3,
    "minor": 28,
    "patch": 0
  },
  "configurePresets": [
    {
      "name": "debug",
      "binaryDir": "${sourceDir}/build/debug",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug"
      }
    },
    {
      "name": "release",
      "binaryDir": "${sourceDir}/build/release",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Release"
      }
    }
  ],
  "buildPresets": [
    {
      "name": "debug",
      "configurePreset": "debug",
      "configuration": "Debug"
    },
    {
      "name": "release",
      "configurePreset": "release",
      "configuration": "Release"
    }
  ],
  "testPresets": [
    {
      "name": "debug",
      "configurePreset": "debug",
      "configuration": "Debug"
    },
    {
      "name": "release",
      "configurePreset": "release",
      "configuration": "Release"
    }
  ]
}
)");
}

TEST_CASE("presets renderer orders explicit presets and cache variables deterministically") {
    const auto project = normalize(R"(
schemaVersion: 1
name: example
presets:
  zeta:
    buildType: RelWithDebInfo
    generator: Ninja
    binaryDir: out/zeta
    cacheVariables:
      Z_VALUE: last
      A_VALUE: first
      CMAKE_BUILD_TYPE: ignored
  alpha:
    buildType: Debug
    binaryDir: ${sourceDir}/custom/alpha
targets:
  App:
    type: executable
)");

    const auto first = render_presets(project);
    const auto second = render_presets(project);
    CHECK(first == second);

    const auto json = nlohmann::json::parse(first);
    REQUIRE(json["configurePresets"].size() == 2);
    CHECK(json["configurePresets"][0]["name"] == "alpha");
    CHECK(json["configurePresets"][0]["binaryDir"] == "${sourceDir}/custom/alpha");
    CHECK(json["configurePresets"][1]["name"] == "zeta");
    CHECK(json["configurePresets"][1]["generator"] == "Ninja");
    CHECK(json["configurePresets"][1]["binaryDir"] == "${sourceDir}/out/zeta");
    CHECK(json["configurePresets"][1]["cacheVariables"]["CMAKE_BUILD_TYPE"] == "RelWithDebInfo");
    CHECK(json["configurePresets"][1]["cacheVariables"]["A_VALUE"] == "first");
    CHECK(json["configurePresets"][1]["cacheVariables"]["Z_VALUE"] == "last");
}

TEST_CASE("presets renderer sets build and test configurations for multi-config generators") {
    const auto project = normalize(R"(
schemaVersion: 1
name: example
presets:
  multi:
    buildType: RelWithDebInfo
    generator: Ninja Multi-Config
    binaryDir: $env{CMAKEGEN_BUILD_ROOT}/multi
  inherited:
    buildType: Debug
    generator: Visual Studio 17 2022
    binaryDir: $penv{CMAKEGEN_BUILD_ROOT}/inherited
targets:
  App:
    type: executable
)");

    const auto json = nlohmann::json::parse(render_presets(project));
    REQUIRE(json["configurePresets"].size() == 2);
    CHECK(json["configurePresets"][0]["binaryDir"] == "$penv{CMAKEGEN_BUILD_ROOT}/inherited");
    CHECK(json["configurePresets"][1]["binaryDir"] == "$env{CMAKEGEN_BUILD_ROOT}/multi");
    CHECK(json["buildPresets"][0]["configuration"] == "Debug");
    CHECK(json["buildPresets"][1]["configuration"] == "RelWithDebInfo");
    CHECK(json["testPresets"][0]["configuration"] == "Debug");
    CHECK(json["testPresets"][1]["configuration"] == "RelWithDebInfo");
}

TEST_CASE("presets renderer supports an explicitly empty preset set") {
    const auto project = normalize(R"(
schemaVersion: 1
name: example
presets: {}
targets:
  App:
    type: executable
)");

    const auto json = nlohmann::json::parse(render_presets(project));
    CHECK(json["configurePresets"].empty());
    CHECK(json["buildPresets"].empty());
    CHECK(json["testPresets"].empty());
}

TEST_CASE("presets compatibility mapping requires the documented CMake baseline") {
    CHECK(preset_schema_version_for("3.28") == 8);
    CHECK(preset_schema_version_for("4.0.1") == 8);
    CHECK(cmake_version_components("4.0.1") == std::array<std::uint32_t, 3>{4, 0, 1});
    CHECK(cmake_version_components("3.28.0.1") == std::array<std::uint32_t, 3>{3, 28, 1});
    CHECK(cmake_version_components("3.28.4.7") == std::array<std::uint32_t, 3>{3, 28, 5});
    CHECK(cmake_version_components("00000000003.00000000028.00000000000.00000000001") ==
          std::array<std::uint32_t, 3>{3, 28, 1});
    CHECK(cmake_version_components("3.28...3.31") == std::array<std::uint32_t, 3>{3, 28, 0});
    CHECK_THROWS_AS(preset_schema_version_for("3.27"), std::invalid_argument);
    CHECK_THROWS_AS(preset_schema_version_for("3"), std::invalid_argument);
    CHECK_THROWS_AS(preset_schema_version_for("not-a-version"), std::invalid_argument);
}
