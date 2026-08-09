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
    CHECK(run({"--project", project, "dump"}, output, error) == 0);
    CHECK(output.find("\"name\": \"Example\"") != std::string::npos);
    CHECK(run({"--project", project, "graph"}, output, error) == 0);
    CHECK(output.starts_with("└── App\n"));
    CHECK(output.find("    └── Core\n") != std::string::npos);
    CHECK(output.find("fmt::fmt") == std::string::npos);
    CHECK(run({"--project", project, "graph", "--include-external", "--format", "dot"}, output, error) == 0);
    CHECK(output.find("\"App\" -> \"fmt::fmt\"") != std::string::npos);
}

TEST_CASE("CLI rejects command-only options on commands that do not support them") {
    std::string output, error;
    CHECK(run({"generate", "--dry-run"}, output, error) == 1);
    CHECK(error == "error: --dry-run is only valid with clean\n");
    CHECK(run({"validate", "--force"}, output, error) == 1);
    CHECK(error == "error: --force is only valid with generate\n");
    CHECK(run({"clean", "--format", "json"}, output, error) == 1);
    CHECK(error == "error: --format is only valid with dump or graph\n");
    CHECK(run({"dump", "--include-external"}, output, error) == 1);
    CHECK(error == "error: --include-external is only valid with graph\n");
    CHECK(run({"graph", "--resolved"}, output, error) == 1);
    CHECK(error == "error: --resolved is only valid with validate\n");
    CHECK(run({"--version", "--check"}, output, error) == 1);
    CHECK(error == "error: --check is only valid with generate\n");
}

TEST_CASE("CLI verbose mode reports command context and validation progress") {
    FixtureDirectory fixture;
    fixture.file("project.yml", valid_project);
    std::string output, error;
    const auto project = (fixture.path() / "project.yml").string();
    CHECK(run({"--project", project, "validate", "--verbose"}, output, error) == 0);
    CHECK(output == "Valid.\n");
    CHECK(error.find("verbose: command=validate") != std::string::npos);
    CHECK(error.find("verbose: validated Example (2 targets)") != std::string::npos);
}

TEST_CASE("CLI generate honors check, force, root, and clean dry run safety behavior") {
    FixtureDirectory fixture;
    fixture.file("config/spec.yml", valid_project);
    fixture.file("config/src/main.cpp", "int main() {}\n");
    fixture.file("config/src/core.cpp", "void core() {}\n");
    std::string output, error;
    const auto spec = (fixture.path() / "config/spec.yml").string();
    const auto root = fixture.path().string();
    CHECK(run({"--project", spec, "--root", root, "generate"}, output, error) == 0);
    CHECK(output == "Generated 3 files.\n");
    CHECK(std::filesystem::exists(fixture.path() / ".cmakegen/manifest.json"));
    CHECK(run({"--project", spec, "--root", root, "generate", "--check"}, output, error) == 0);
    CHECK(output == "No changes.\n");
    std::filesystem::remove(fixture.path() / "CMakeLists.txt");
    CHECK(run({"--project", spec, "--root", root, "generate", "--check"}, output, error) == 1);
    CHECK(run({"--project", spec, "--root", root, "generate"}, output, error) == 0);
    CHECK(run({"--root", root, "clean", "--dry-run"}, output, error) == 0);
    CHECK(std::filesystem::exists(fixture.path() / "CMakeLists.txt"));
    CHECK(run({"--root", root, "clean"}, output, error) == 0);
    CHECK_FALSE(std::filesystem::exists(fixture.path() / "CMakeLists.txt"));
}

TEST_CASE("CLI resolves sources relative to the specification, not the output root") {
    FixtureDirectory fixture;
    fixture.file("spec/project.yml", valid_project);
    fixture.file("spec/src/main.cpp", "int main() {}\n");
    fixture.file("spec/src/core.cpp", "void core() {}\n");
    const auto output_root = fixture.path() / "generated";
    std::filesystem::create_directories(output_root);
    std::string output, error;
    CHECK(run({"--project", (fixture.path() / "spec/project.yml").string(), "--root", output_root.string(),
               "generate"}, output, error) == 0);
    CHECK(std::filesystem::exists(output_root / "CMakeLists.txt"));
    CHECK_FALSE(std::filesystem::exists(fixture.path() / "spec/CMakeLists.txt"));
}

TEST_CASE("CLI graph escapes DOT strings and prints platform-union cycle components") {
    FixtureDirectory fixture;
    fixture.file("project.yml", R"(schemaVersion: 1
name: Example
targets:
  App:
    type: executable
    platform:
      macos:
        dependencies:
          private: [Core]
  Core:
    type: staticLibrary
    platform:
      windows:
        dependencies:
          private: [App]
  Tool:
    type: executable
)");
    std::string output, error;
    const auto project = (fixture.path() / "project.yml").string();
    CHECK(run({"--project", project, "graph", "--format", "text"}, output, error) == 0);
    CHECK(output.find("App") != std::string::npos);
    CHECK(output.find("Core") != std::string::npos);
    CHECK(output.find("Tool") != std::string::npos);

    fixture.file("escaped.yml", R"(schemaVersion: 1
name: Example
targets:
  App:
    type: executable
    dependencies:
      private: ['Pkg"Quote\\Name']
)");
    CHECK(run({"--project", (fixture.path() / "escaped.yml").string(), "graph", "--format", "dot",
               "--include-external"}, output, error) == 0);
    CHECK(output.find("Pkg\\\"Quote") != std::string::npos);
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
