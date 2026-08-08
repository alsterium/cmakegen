#include "cmakegen/domain/dump.hpp"
#include "cmakegen/domain/normalizer.hpp"
#include "cmakegen/parser/parser.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace cmakegen;

namespace {

[[nodiscard]] Project normalize(std::string_view yaml) {
    const auto raw = parse_raw_spec(yaml);
    REQUIRE(raw);
    return normalize_project(*raw);
}

} // namespace

TEST_CASE("normalization applies defaults and canonical key ordering") {
    const auto project = normalize(R"(
schemaVersion: 1
name: example
packages:
  zlib:
    name: ZLIB
  fmt:
    name: fmt
targets:
  Zebra:
    type: executable
  Alpha:
    type: staticLibrary
)");

    CHECK(project.cmake.minimum_version == "3.28");
    REQUIRE(project.cmake.languages.size() == 1);
    CHECK(project.cmake.languages[0] == Language::cxx);
    CHECK(project.cmake.cxx_standard == "20");
    CHECK_FALSE(project.cmake.extensions);
    REQUIRE(project.targets.size() == 2);
    CHECK(project.targets.begin()->first == "Alpha");
    CHECK(project.packages.begin()->first == "fmt");
    REQUIRE(project.presets.size() == 2);
    CHECK(project.presets.at("debug").binary_dir == "build/debug");
    CHECK(project.presets.at("release").binary_dir == "build/release");
}

TEST_CASE("normalization expands scoped values classifies dependencies and canonicalizes paths") {
    const auto project = normalize(R"(
schemaVersion: 1
name: example
cmake:
  languages: [CXX, C]
  cStandard: 17
  cxxStandard: 23
  include: [cmake/../custom.cmake]
targets:
  App:
    type: executable
    sources:
      - src/../app
    includeDirectories:
      - include/../include/app
    dependencies:
      - Core
      - fmt::fmt
  Core:
    type: staticLibrary
    platform:
      linux:
        definitions: [LINUX]
)");

    CHECK(project.cmake.languages == std::vector{Language::c, Language::cxx});
    CHECK(project.cmake.c_standard == "17");
    CHECK(project.cmake.cxx_standard == "23");
    CHECK(project.cmake.include_files == std::vector<std::string>{"custom.cmake"});
    const auto& app = project.targets.at("App");
    CHECK(app.sources[0].path == "app");
    CHECK(app.include_directories.private_values == std::vector<std::string>{"include/app"});
    REQUIRE(app.dependencies.private_values.size() == 2);
    CHECK(app.dependencies.private_values[0].kind == LinkDependency::Kind::project_target);
    CHECK(app.dependencies.private_values[1].kind == LinkDependency::Kind::imported_target);
    CHECK(project.targets.at("Core").platform.linux.definitions.private_values == std::vector<std::string>{"LINUX"});
}

TEST_CASE("explicit empty presets suppress default presets and paths use slash form") {
    const auto project = normalize(R"(
schemaVersion: 1
name: example
presets: {}
targets:
  App:
    type: executable
    sources: [src]
)");

    CHECK(project.presets.empty());
    CHECK(project.targets.at("App").sources[0].path == "src");
}

TEST_CASE("normalization converts Windows path separators on every host platform") {
    const auto project = normalize(R"(
schemaVersion: 1
name: example
targets:
  App:
    type: executable
    sources: [src\\platform\\..\\main.cpp]
)");

    CHECK(project.targets.at("App").sources[0].path == "src/main.cpp");
}

TEST_CASE("normalization preserves Windows drive roots on every host platform") {
    const auto project = normalize(R"(
schemaVersion: 1
name: example
targets:
  App:
    type: executable
    sources: [C:\\foo\\..\\..\\bar]
)");

    CHECK(project.targets.at("App").sources[0].path == "C:/bar");
}

TEST_CASE("normalization preserves Windows drive-relative prefixes on every host platform") {
    const auto project = normalize(R"(
schemaVersion: 1
name: example
targets:
  App:
    type: executable
    sources: [C:foo\\..\\bar]
)");

    CHECK(project.targets.at("App").sources[0].path == "C:bar");
}

TEST_CASE("normalization preserves UNC server and share roots on every host platform") {
    const auto project = normalize(R"(
schemaVersion: 1
name: example
targets:
  App:
    type: executable
    sources: [\\server\share\..\..\bar]
)");

    CHECK(project.targets.at("App").sources[0].path == "//server/share/bar");
}

TEST_CASE("normalization collapses repeated POSIX root separators") {
    const auto project = normalize(R"(
schemaVersion: 1
name: example
targets:
  App:
    type: executable
    sources: [///foo/../bar]
)");

    CHECK(project.targets.at("App").sources[0].path == "/bar");
}

TEST_CASE("normalized JSON and YAML dumps are deterministic") {
    const auto project = normalize(R"(
schemaVersion: 1
name: example
targets:
  App:
    type: executable
    sources: [src]
)");

    const auto json = dump_project_json(project);
    const auto yaml = dump_project_yaml(project);
    CHECK(json == dump_project_json(project));
    CHECK(yaml == dump_project_yaml(project));
    CHECK(json.find("\"schemaVersion\": 1") != std::string::npos);
    CHECK(json.find("\"src\"") != std::string::npos);
    CHECK(yaml.find("\"schemaVersion\": 1") != std::string::npos);
    CHECK(yaml.find("\"path\": \"src\"") != std::string::npos);
}

TEST_CASE("YAML dump quotes user-controlled mapping keys") {
    const auto project = normalize(R"(
schemaVersion: 1
name: example
targets:
  "foo: bar":
    type: executable
)");

    CHECK(dump_project_yaml(project).find("\"foo: bar\":") != std::string::npos);
}

TEST_CASE("YAML dump escapes control characters in user-controlled values") {
    const auto project = normalize(R"(
schemaVersion: 1
name: example
targets:
  App:
    type: executable
    definitions: ["NUL\0CR\rTAB\t"]
)");

    CHECK(dump_project_yaml(project).find("NUL\\0CR\\rTAB\\t") != std::string::npos);
}

TEST_CASE("YAML dump escapes the NEL line-break character") {
    auto project = normalize(R"(
schemaVersion: 1
name: example
targets:
  App:
    type: executable
)");
    project.targets.at("App").definitions.private_values.push_back(std::string{"NEL\xC2\x85"});

    CHECK(dump_project_yaml(project).find("NEL\\N") != std::string::npos);
}

TEST_CASE("YAML dump escapes C1 control characters") {
    auto project = normalize(R"(
schemaVersion: 1
name: example
targets:
  App:
    type: executable
)");
    project.targets.at("App").definitions.private_values.push_back(std::string{"C1\xC2\x80"});

    CHECK(dump_project_yaml(project).find("C1\\x80") != std::string::npos);
}
