#include "cmakegen/cli/application.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

using namespace cmakegen;

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path() / ("cmakegen-integration-" +
              std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path_, error); }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

[[nodiscard]] std::filesystem::path fixture_path(std::string_view name) {
    return std::filesystem::path{CMAKEGEN_SOURCE_DIR} / "tests/fixtures/integration" / name;
}

void copy_fixture(std::string_view name, const std::filesystem::path& destination) {
    const auto fixture = fixture_path(name);
    std::filesystem::copy_file(fixture / "project.yml", destination / "project.yml");
    std::filesystem::copy(fixture / "tree", destination,
                          std::filesystem::copy_options::recursive);
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] int run_cli_at(const std::filesystem::path& root, std::initializer_list<std::string> arguments,
                             std::string& output, std::string& error) {
    std::vector<std::string> actual{"--project", (root / "project.yml").string(), "--root", root.string()};
    actual.insert(actual.end(), arguments.begin(), arguments.end());
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const auto status = run_cli(actual, stdout_stream, stderr_stream);
    output = stdout_stream.str();
    error = stderr_stream.str();
    return status;
}

void require_generated_files_match(std::string_view name, const std::filesystem::path& root) {
    const auto expected = fixture_path(name) / "expected";
    for (const auto& relative : {std::filesystem::path{"CMakeLists.txt"}, std::filesystem::path{"CMakePresets.json"},
                                 std::filesystem::path{"cmake/cmakegen.generated.cmake"}}) {
        INFO("fixture: " << name << ", output: " << relative.string());
        REQUIRE(read_file(root / relative) == read_file(expected / relative));
    }
}

[[nodiscard]] int cmake_command(const std::filesystem::path& source, const std::filesystem::path& build,
                                bool build_project) {
    const auto command = std::string{"\"" CMAKEGEN_CMAKE_COMMAND "\" "} +
        (build_project ? "--build \"" + build.string() + "\"" : "-S \"" + source.string() + "\" -B \"" + build.string() + "\"");
    return std::system(command.c_str());
}

} // namespace

TEST_CASE("full generator pipeline matches offline golden fixtures") {
    for (const auto name : {"basic", "internal-library", "package", "platform", "exclusion"}) {
        TemporaryDirectory output_directory;
        copy_fixture(name, output_directory.path());
        std::string output;
        std::string error;
        INFO("fixture: " << name);
        REQUIRE(run_cli_at(output_directory.path(), {"generate", "--no-color"}, output, error) == 0);
        CHECK(error.empty());
        require_generated_files_match(name, output_directory.path());
    }
}

TEST_CASE("generated fixture projects configure and build without network access") {
    for (const auto name : {"basic", "internal-library", "platform", "exclusion"}) {
        TemporaryDirectory output_directory;
        copy_fixture(name, output_directory.path());
        std::string output;
        std::string error;
        INFO("fixture: " << name);
        REQUIRE(run_cli_at(output_directory.path(), {"generate", "--no-color"}, output, error) == 0);
        REQUIRE(cmake_command(output_directory.path(), output_directory.path() / "build", false) == 0);
        REQUIRE(cmake_command(output_directory.path(), output_directory.path() / "build", true) == 0);
    }
}

TEST_CASE("generation is byte-stable and check mode neither writes nor accepts drift") {
    TemporaryDirectory output_directory;
    copy_fixture("internal-library", output_directory.path());
    std::string output;
    std::string error;
    REQUIRE(run_cli_at(output_directory.path(), {"generate", "--no-color"}, output, error) == 0);
    const auto first_root = read_file(output_directory.path() / "CMakeLists.txt");
    const auto first_generated = read_file(output_directory.path() / "cmake/cmakegen.generated.cmake");
    const auto first_presets = read_file(output_directory.path() / "CMakePresets.json");
    const auto first_manifest = read_file(output_directory.path() / ".cmakegen/manifest.json");

    REQUIRE(run_cli_at(output_directory.path(), {"generate", "--no-color"}, output, error) == 0);
    CHECK(output == "No changes.\n");
    CHECK(read_file(output_directory.path() / "CMakeLists.txt") == first_root);
    CHECK(read_file(output_directory.path() / "cmake/cmakegen.generated.cmake") == first_generated);
    CHECK(read_file(output_directory.path() / "CMakePresets.json") == first_presets);
    CHECK(read_file(output_directory.path() / ".cmakegen/manifest.json") == first_manifest);

    { std::ofstream altered{output_directory.path() / "CMakeLists.txt", std::ios::binary}; altered << "drift\n"; }
    REQUIRE(run_cli_at(output_directory.path(), {"generate", "--check", "--no-color"}, output, error) == 1);
    CHECK(read_file(output_directory.path() / "CMakeLists.txt") == "drift\n");
    CHECK(read_file(output_directory.path() / ".cmakegen/manifest.json") == first_manifest);
}

TEST_CASE("invalid input is rejected before any generated output is written") {
    TemporaryDirectory output_directory;
    std::ofstream{output_directory.path() / "project.yml"} << R"(schemaVersion: 1
name: Broken
targets:
  app:
    type: executable
    sources: [missing]
)";
    std::string output;
    std::string error;
    REQUIRE(run_cli_at(output_directory.path(), {"generate", "--no-color"}, output, error) == 2);
    CHECK_FALSE(std::filesystem::exists(output_directory.path() / "CMakeLists.txt"));
    CHECK_FALSE(std::filesystem::exists(output_directory.path() / ".cmakegen/manifest.json"));
}

TEST_CASE("fixture output uses portable slash-normalized source paths") {
    TemporaryDirectory output_directory;
    copy_fixture("exclusion", output_directory.path());
    std::string output;
    std::string error;
    REQUIRE(run_cli_at(output_directory.path(), {"generate", "--no-color"}, output, error) == 0);
    const auto generated = read_file(output_directory.path() / "cmake/cmakegen.generated.cmake");
    CHECK(generated.find("src/engine.cpp") != std::string::npos);
    CHECK(generated.find("engine_test.cpp") == std::string::npos);
    CHECK(generated.find("bench/benchmark.cpp") == std::string::npos);
    CHECK(generated.find('\\') == std::string::npos);
}
