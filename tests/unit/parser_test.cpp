#include "cmakegen/parser/parser.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace cmakegen;

TEST_CASE("the minimal project specification parses into raw schema values") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 1
name: hello
targets:
  hello:
    type: executable
    sources:
      - src
)");

    REQUIRE(result);
    CHECK(result->schema_version.value == 1);
    CHECK(result->name.value == "hello");
    REQUIRE(result->targets.size() == 1);
    CHECK(result->targets[0].type.value == RawTargetType::executable);
    REQUIRE(result->targets[0].sources.size() == 1);
    CHECK(result->targets[0].sources[0].path.value == "src");
    CHECK(result->targets[0].sources[0].path.path == "targets.hello.sources[0]");
    CHECK(result->targets[0].sources[0].path.location == SourceLocation{8, 9});
}

TEST_CASE("unsupported CMake languages are rejected before normalization") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 1
name: hello
cmake:
  languages: [CXX, CUDA]
targets:
  hello:
    type: executable
)");

    REQUIRE_FALSE(result);
    const auto& diagnostic = result.error().diagnostics().front();
    CHECK(diagnostic.code == diagnostic_codes::invalid_value);
    CHECK(diagnostic.path == "cmake.languages[1]");
    CHECK(diagnostic.message == "unsupported language 'CUDA'");
}

TEST_CASE("an explicitly empty CMake language list is rejected") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 1
name: hello
cmake:
  languages: []
targets:
  hello:
    type: executable
)");

    REQUIRE_FALSE(result);
    const auto& diagnostic = result.error().diagnostics().front();
    CHECK(diagnostic.path == "cmake.languages");
    CHECK(diagnostic.message == "must not be empty");
}

TEST_CASE("preset names cannot be used as path traversal components") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 1
name: hello
presets:
  "../../outside":
    buildType: Debug
targets:
  hello:
    type: executable
)");

    REQUIRE_FALSE(result);
    const auto& diagnostic = result.error().diagnostics().front();
    CHECK(diagnostic.path == "presets.../../outside");
    CHECK(diagnostic.message == "must not contain path separators or '$', or be '.' or '..'");
}

TEST_CASE("preset names cannot inject CMake macros into default binary directories") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 1
name: hello
presets:
  "${sourceDir}":
    buildType: Debug
targets:
  hello:
    type: executable
)");

    REQUIRE_FALSE(result);
    const auto& diagnostic = result.error().diagnostics().front();
    CHECK(diagnostic.path == "presets.${sourceDir}");
    CHECK(diagnostic.message == "must not contain path separators or '$', or be '.' or '..'");
}

TEST_CASE("source shorthand and expanded entries have equivalent raw semantics") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 1
name: hello
targets:
  hello:
    type: executable
    sources:
      - src
      - path: include/foo.hpp
        recursive: true
)");

    REQUIRE(result);
    REQUIRE(result->targets[0].sources.size() == 2);
    CHECK(result->targets[0].sources[0].path.value == "src");
    CHECK(result->targets[0].sources[0].recursive.value);
    CHECK(result->targets[0].sources[1].path.value == "include/foo.hpp");
    CHECK(result->targets[0].sources[1].recursive.value);
}

TEST_CASE("expanded sources retain exclusions and locations") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 1
name: hello
targets:
  hello:
    type: staticLibrary
    sources:
      - path: src
        recursive: false
        exclude: ["**/*_test.cpp"]
)");

    REQUIRE(result);
    const auto& source = result->targets[0].sources[0];
    CHECK(source.recursive.value == false);
    REQUIRE(source.exclude.size() == 1);
    CHECK(source.exclude[0].value == "**/*_test.cpp");
    CHECK(source.exclude[0].path == "targets.hello.sources[0].exclude[0]");
}

TEST_CASE("invalid root and schema values produce precise diagnostics") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 2
name: 42
targets: []
)");

    REQUIRE_FALSE(result);
    const auto& diagnostics = result.error().diagnostics();
    REQUIRE(diagnostics.size() == 3);
    CHECK(diagnostics[0].path == "name");
    CHECK(diagnostics[1].code == diagnostic_codes::invalid_schema_version);
    CHECK(diagnostics[1].path == "schemaVersion");
    CHECK(diagnostics[2].path == "targets");
}

TEST_CASE("project names must use the documented character set") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 1
name: hello world
targets:
  hello:
    type: executable
)");

    REQUIRE_FALSE(result);
    const auto& diagnostic = result.error().diagnostics().front();
    CHECK(diagnostic.path == "name");
    CHECK(diagnostic.message == "must match ^[A-Za-z0-9_.+-]+$");
}

TEST_CASE("unknown nested keys and malformed source entries are rejected") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 1
name: hello
targets:
  hello:
    type: executable
    unexpected: true
    sources:
      - recursive: false
)");

    REQUIRE_FALSE(result);
    const auto& diagnostics = result.error().diagnostics();
    REQUIRE(diagnostics.size() == 2);
    CHECK(diagnostics[0].path == "targets.hello.sources[0].path");
    CHECK(diagnostics[1].code == diagnostic_codes::unknown_key);
    CHECK(diagnostics[1].path == "targets.hello.unexpected");
}

TEST_CASE("multiple YAML documents are rejected instead of silently ignoring trailing input") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 1
name: hello
targets:
  hello:
    type: executable
---
schemaVersion: 1
name: ignored
targets:
  ignored:
    type: executable
)");

    REQUIRE_FALSE(result);
    const auto& diagnostic = result.error().diagnostics().front();
    CHECK(diagnostic.message == "expected exactly one YAML document");
}

TEST_CASE("duplicate mapping keys are rejected at root and nested levels") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 1
name: hello
name: ignored
targets:
  hello:
    type: executable
    type: staticLibrary
)");

    REQUIRE_FALSE(result);
    const auto& diagnostics = result.error().diagnostics();
    REQUIRE(diagnostics.size() == 2);
    CHECK(diagnostics[0].path == "name");
    CHECK(diagnostics[0].message == "duplicate key 'name'");
    CHECK(diagnostics[1].path == "targets.hello.type");
    CHECK(diagnostics[1].message == "duplicate key 'type'");
}

TEST_CASE("quoted strings are rejected for integer and boolean fields") {
    const auto result = parse_raw_spec(R"(
schemaVersion: "1"
name: hello
cmake:
  extensions: "false"
targets:
  hello:
    type: executable
)");

    REQUIRE_FALSE(result);
    const auto& diagnostics = result.error().diagnostics();
    REQUIRE(diagnostics.size() == 2);
    CHECK(diagnostics[0].path == "cmake.extensions");
    CHECK(diagnostics[0].message == "expected a boolean");
    CHECK(diagnostics[1].path == "schemaVersion");
    CHECK(diagnostics[1].message == "expected an unsigned integer");
}

TEST_CASE("explicit YAML integer and boolean tags are accepted for typed fields") {
    const auto result = parse_raw_spec(R"(
schemaVersion: !!int 1
name: hello
cmake:
  extensions: !!bool false
targets:
  hello:
    type: executable
)");

    REQUIRE(result);
    CHECK(result->schema_version.value == 1);
    CHECK(result->cmake.extensions.value == false);
}

TEST_CASE("explicit non-string tags are rejected for string fields") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 1
name: !!bool true
targets:
  hello:
    type: executable
)");

    REQUIRE_FALSE(result);
    const auto& diagnostic = result.error().diagnostics().front();
    CHECK(diagnostic.path == "name");
    CHECK(diagnostic.message == "expected a string");
}

TEST_CASE("implicitly typed scalars are rejected for string fields") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 1
name: 42
targets:
  hello:
    type: executable
)");

    REQUIRE_FALSE(result);
    const auto& diagnostic = result.error().diagnostics().front();
    CHECK(diagnostic.path == "name");
    CHECK(diagnostic.message == "expected a string");
}

TEST_CASE("out-of-range implicit numeric scalars are rejected for string fields") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 1
name: 1e9999
targets:
  hello:
    type: executable
)");

    REQUIRE_FALSE(result);
    const auto& diagnostic = result.error().diagnostics().front();
    CHECK(diagnostic.path == "name");
    CHECK(diagnostic.message == "expected a string");
}

TEST_CASE("underscored project names remain string-valued YAML scalars") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 1
name: 1_2
targets:
  hello:
    type: executable
)");

    REQUIRE(result);
    CHECK(result->name.value == "1_2");
}

TEST_CASE("signed NaN-like project names remain string-valued YAML scalars") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 1
name: +.nan
targets:
  hello:
    type: executable
)");

    REQUIRE(result);
    CHECK(result->name.value == "+.nan");
}

TEST_CASE("quoted numeric-looking text remains valid for string fields") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 1
name: "1e9999"
targets:
  hello:
    type: executable
)");

    REQUIRE(result);
    CHECK(result->name.value == "1e9999");
}

TEST_CASE("negative integer values are rejected before unsigned conversion") {
    const auto result = parse_raw_spec(R"(
schemaVersion: -4294967295
name: hello
targets:
  hello:
    type: executable
)");

    REQUIRE_FALSE(result);
    const auto& diagnostic = result.error().diagnostics().front();
    CHECK(diagnostic.path == "schemaVersion");
    CHECK(diagnostic.message == "expected an unsigned integer");
}

TEST_CASE("YAML core octal integers are decoded correctly") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 0o1
name: hello
targets:
  hello:
    type: executable
)");

    REQUIRE(result);
    CHECK(result->schema_version.value == 1);
}

TEST_CASE("legacy YAML boolean spellings are rejected") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 1
name: hello
cmake:
  extensions: yes
targets:
  hello:
    type: executable
)");

    REQUIRE_FALSE(result);
    const auto& diagnostic = result.error().diagnostics().front();
    CHECK(diagnostic.path == "cmake.extensions");
    CHECK(diagnostic.message == "expected a boolean");
}

TEST_CASE("target mapping keys must have string semantics") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 1
name: hello
targets:
  !!bool true:
    type: executable
)");

    REQUIRE_FALSE(result);
    const auto& diagnostic = result.error().diagnostics().front();
    CHECK(diagnostic.path == "targets");
    CHECK(diagnostic.message == "target names must be strings");
}

TEST_CASE("the complete schema-version-1 example parses into extended raw values") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 1
name: MyApp
version: 0.1.0
cmake:
  minimumVersion: "3.28"
  languages: [CXX]
  cxxStandard: 23
  extensions: false
  include: [cmake/custom.cmake]
packages:
  fmt:
    name: fmt
    version: "11"
    required: true
    components: []
targets:
  Core:
    type: staticLibrary
    sources:
      - path: Sources/Core
        exclude: ["**/*_test.cpp"]
    includeDirectories:
      public: [include]
      private: [Sources/Core]
    definitions:
      private: [CORE_INTERNAL=1]
    compileOptions:
      private: [-Wall]
    dependencies:
      public: [fmt::fmt]
    platform:
      macos:
        definitions:
          private: [CORE_MACOS=1]
  App:
    type: executable
    sources: [Sources/App]
    dependencies: [Core]
presets:
  debug:
    buildType: Debug
    generator: Ninja
    binaryDir: build/debug-custom
    cacheVariables:
      CMAKE_EXPORT_COMPILE_COMMANDS: "ON"
  release:
    buildType: Release
)");

    REQUIRE(result);
    REQUIRE(result->cmake.include_files.size() == 1);
    CHECK(result->cmake.include_files[0].value == "cmake/custom.cmake");
    REQUIRE(result->packages.size() == 1);
    CHECK(result->packages[0].alias == "fmt");
    CHECK(result->packages[0].name.value == "fmt");
    REQUIRE(result->packages[0].version);
    CHECK(result->packages[0].version->value == "11");
    CHECK(result->packages[0].required.value);
    REQUIRE(result->targets.size() == 2);
    const auto& core = result->targets[0];
    CHECK(core.include_directories.public_values[0].value == "include");
    CHECK(core.include_directories.private_values[0].path == "targets.Core.includeDirectories.private[0]");
    CHECK(core.include_directories.public_values[0].location == SourceLocation{24, 16});
    CHECK(core.include_directories.private_values[0].location == SourceLocation{25, 17});
    CHECK(core.definitions.private_values[0].value == "CORE_INTERNAL=1");
    CHECK(core.definitions.private_values[0].location == SourceLocation{27, 17});
    CHECK(core.compile_options.private_values[0].value == "-Wall");
    CHECK(core.compile_options.private_values[0].location == SourceLocation{29, 17});
    CHECK(core.dependencies.public_values[0].value == "fmt::fmt");
    REQUIRE(core.macos);
    CHECK(core.macos->definitions.private_values[0].value == "CORE_MACOS=1");
    CHECK(core.macos->definitions.private_values[0].location == SourceLocation{35, 21});
    CHECK(result->targets[1].dependencies.private_values[0].value == "Core");
    REQUIRE(result->presets);
    REQUIRE(result->presets->size() == 2);
    CHECK((*result->presets)[0].build_type.value == "Debug");
    REQUIRE((*result->presets)[0].generator);
    CHECK((*result->presets)[0].generator->value == "Ninja");
    CHECK((*result->presets)[0].cache_variable_names[0].value == "CMAKE_EXPORT_COMPILE_COMMANDS");
    CHECK((*result->presets)[0].cache_variable_values[0].value == "ON");
}

TEST_CASE("omitted and explicitly empty preset mappings retain distinct raw semantics") {
    const auto omitted = parse_raw_spec(R"(
schemaVersion: 1
name: omitted
targets:
  app:
    type: executable
)");
    const auto empty = parse_raw_spec(R"(
schemaVersion: 1
name: empty
targets:
  app:
    type: executable
presets: {}
)");

    REQUIRE(omitted);
    CHECK_FALSE(omitted->presets);
    REQUIRE(empty);
    REQUIRE(empty->presets);
    CHECK(empty->presets->empty());
}

TEST_CASE("scoped values accept every documented scope and platform override") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 1
name: scoped
targets:
  library:
    type: staticLibrary
    includeDirectories: {private: [private], public: [public], interface: [interface]}
    definitions: {private: [PRIVATE], public: [PUBLIC], interface: [INTERFACE]}
    compileOptions: {private: [-private], public: [-public], interface: [-interface]}
    dependencies: {private: [Private], public: [Public], interface: [Interface]}
    platform:
      linux:
        sources: [linux]
        includeDirectories: [linux/include]
        definitions: [LINUX]
        compileOptions: [-pthread]
        dependencies: [Threads::Threads]
      windows: {}
)");

    REQUIRE(result);
    const auto& target = result->targets.front();
    CHECK(target.include_directories.private_values[0].value == "private");
    CHECK(target.include_directories.public_values[0].value == "public");
    CHECK(target.include_directories.interface_values[0].value == "interface");
    REQUIRE(target.linux);
    CHECK(target.linux->sources[0].path.value == "linux");
    CHECK(target.linux->include_directories.private_values[0].value == "linux/include");
    REQUIRE(target.windows);
}

TEST_CASE("packages are required unless explicitly marked optional") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 1
name: packages
packages:
  Qt:
    name: Qt6
    version: "6.8"
    components: [Core, Widgets]
  optional:
    name: OptionalPackage
    required: false
targets:
  app:
    type: executable
)");

    REQUIRE(result);
    REQUIRE(result->packages.size() == 2);
    CHECK(result->packages[0].required.value);
    CHECK(result->packages[1].required.value == false);
}

TEST_CASE("unknown keys are rejected at every extended mapping level") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 1
name: invalid
cmake:
  unknown: value
packages:
  pkg:
    name: pkg
    unknown: value
targets:
  app:
    type: executable
    dependencies:
      protected: [value]
    platform:
      freebsd: {}
      linux:
        unknown: value
presets:
  debug:
    buildType: Debug
    unknown: value
)");

    REQUIRE_FALSE(result);
    const auto& diagnostics = result.error().diagnostics();
    REQUIRE(diagnostics.size() == 6);
    CHECK(diagnostics[0].path == "cmake.unknown");
    CHECK(diagnostics[1].path == "packages.pkg.unknown");
    CHECK(diagnostics[2].path == "presets.debug.unknown");
    CHECK(diagnostics[3].path == "targets.app.dependencies.protected");
    CHECK(diagnostics[4].path == "targets.app.platform.freebsd");
    CHECK(diagnostics[5].path == "targets.app.platform.linux.unknown");
}

TEST_CASE("extended mappings report malformed values and required fields") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 1
name: invalid
packages:
  missingName: {}
targets:
  app:
    type: executable
    includeDirectories: text
    platform:
      macos: text
presets:
  missingBuildType: {}
  badCacheVariables:
    buildType: Debug
    cacheVariables: [not-a-map]
)");

    REQUIRE_FALSE(result);
    const auto& diagnostics = result.error().diagnostics();
    REQUIRE(diagnostics.size() == 5);
    CHECK(diagnostics[0].path == "packages.missingName.name");
    CHECK(diagnostics[1].path == "presets.badCacheVariables.cacheVariables");
    CHECK(diagnostics[2].path == "presets.missingBuildType.buildType");
    CHECK(diagnostics[3].path == "targets.app.includeDirectories");
    CHECK(diagnostics[4].path == "targets.app.platform.macos");
}

TEST_CASE("empty preset cache variable names are rejected") {
    const auto result = parse_raw_spec(R"(
schemaVersion: 1
name: invalid
presets:
  debug:
    buildType: Debug
    cacheVariables:
      "": ON
targets:
  app:
    type: executable
)");

    REQUIRE_FALSE(result);
    const auto& diagnostic = result.error().diagnostics().front();
    CHECK(diagnostic.path == "presets.debug.cacheVariables");
    CHECK(diagnostic.message == "cache variable names must not be empty");
}
