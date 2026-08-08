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
    REQUIRE(diagnostics.size() >= 2);
    CHECK(diagnostics[0].code == diagnostic_codes::invalid_schema_version);
    CHECK(diagnostics[0].path == "schemaVersion");
    CHECK(diagnostics[1].path == "targets");
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
