#include "cmakegen/cli/application.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace cmakegen;

namespace {

class FixtureDirectory {
public:
    FixtureDirectory() : path_(std::filesystem::temp_directory_path() / ("cmakegen-cli-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) { std::filesystem::create_directories(path_); }
    ~FixtureDirectory() { std::filesystem::remove_all(path_); }
    void file(const std::filesystem::path& path, std::string_view content) const { std::filesystem::create_directories((path_ / path).parent_path()); std::ofstream{path_ / path} << content; }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

[[nodiscard]] int run(std::initializer_list<std::string> arguments, std::string& output, std::string& error) {
    std::ostringstream actual_output, actual_error;
    const auto status = run_cli({arguments}, actual_output, actual_error);
    output = actual_output.str(); error = actual_error.str(); return status;
}

constexpr std::string_view valid_project = R"(schemaVersion: 1
name: Example
targets:
  App:
    type: executable
    sources: [src/main.cpp]
    dependencies:
      private: [Core, fmt::fmt]
  Core:
    type: staticLibrary
    sources: [src/core.cpp]
presets:
  debug:
    buildType: Debug
    binaryDir: build/debug
)";

} // namespace

TEST_CASE("CLI provides help and version without a project") {
    std::string output, error;
    CHECK(run({"--version"}, output, error) == 0);
    CHECK(output.starts_with("cmakegen "));
    CHECK(error.empty());
    CHECK(run({"--help"}, output, error) == 0);
    CHECK(output.find("generate, validate, dump, graph, clean") != std::string::npos);
}

TEST_CASE("CLI validates, dumps, and prints project-only or external dependency graphs") {
    FixtureDirectory fixture;
    fixture.file("project.yml", valid_project);
    fixture.file("src/main.cpp", "int main() {}\n");
    fixture.file("src/core.cpp", "void core() {}\n");
    std::string output, error;
    const auto project = (fixture.path() / "project.yml").string();
    CHECK(run({"--project", project, "validate"}, output, error) == 0);
    CHECK(output == "Valid.\n");
    CHECK(run({"--project", project, "dump", "--format", "json"}, output, error) == 0);
    CHECK(output.find("\"name\": \"Example\"") != std::string::npos);
    CHECK(run({"--project", project, "graph"}, output, error) == 0);
    CHECK(output.find("fmt::fmt") == std::string::npos);
    CHECK(run({"--project", project, "graph", "--include-external", "--format", "dot"}, output, error) == 0);
    CHECK(output.find("\"App\" -> \"fmt::fmt\"") != std::string::npos);
}

TEST_CASE("CLI generate honors check, force, root, and clean dry run safety behavior") {
    FixtureDirectory fixture;
    fixture.file("config/spec.yml", valid_project);
    fixture.file("src/main.cpp", "int main() {}\n");
    fixture.file("src/core.cpp", "void core() {}\n");
    std::string output, error;
    const auto spec = (fixture.path() / "config/spec.yml").string();
    const auto root = fixture.path().string();
    CHECK(run({"--project", spec, "--root", root, "generate"}, output, error) == 0);
    CHECK(output == "Generated 3 files.\n");
    CHECK(std::filesystem::exists(fixture.path() / ".cmakegen/manifest.json"));
    CHECK(run({"--project", spec, "--root", root, "generate", "--check"}, output, error) == 0);
    CHECK(output == "No changes.\n");
    fixture.file("CMakeLists.txt", "user-owned\n");
    CHECK(run({"--project", spec, "--root", root, "generate", "--check"}, output, error) == 1);
    CHECK(run({"--root", root, "clean", "--dry-run"}, output, error) == 0);
    CHECK(std::filesystem::exists(fixture.path() / "CMakeLists.txt"));
    CHECK(run({"--root", root, "clean"}, output, error) == 0);
    CHECK_FALSE(std::filesystem::exists(fixture.path() / "CMakeLists.txt"));
}

TEST_CASE("CLI maps invalid specifications and filesystem failures to documented statuses") {
    FixtureDirectory fixture;
    fixture.file("invalid.yml", "schemaVersion: 1\nname: broken\ntargets: []\n");
    std::string output, error;
    CHECK(run({"--project", (fixture.path() / "invalid.yml").string(), "validate", "--no-color"}, output, error) == 1);
    CHECK(error.find("error[CMG") != std::string::npos);
    CHECK(run({"--project", (fixture.path() / "missing.yml").string(), "validate"}, output, error) == 2);
    CHECK(error.find("CMG2001") != std::string::npos);
}
