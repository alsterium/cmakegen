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
