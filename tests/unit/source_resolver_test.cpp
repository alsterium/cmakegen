#include "cmakegen/resolver/source_resolver.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace cmakegen;

namespace {

class FixtureDirectory {
public:
    FixtureDirectory() {
        const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path() / ("cmakegen-source-resolver-" + stamp);
        std::filesystem::create_directories(path_);
    }

    ~FixtureDirectory() { std::filesystem::remove_all(path_); }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    void file(const std::filesystem::path& relative, std::string_view contents = {}) const {
        std::filesystem::create_directories((path_ / relative).parent_path());
        std::ofstream output{path_ / relative};
        output << contents;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::vector<std::string> paths(const std::vector<ResolvedSource>& sources) {
    std::vector<std::string> result;
    for (const auto& source : sources) result.push_back(source.path);
    return result;
}

} // namespace

TEST_CASE("source resolver expands supported directory files in stable order") {
    FixtureDirectory fixture;
    fixture.file("src/z.cpp");
    fixture.file("src/a.hpp");
    fixture.file("src/nested/main.cxx");
    fixture.file("src/ignored.txt");

    const auto resolved = resolve_sources(fixture.path(), {{"src", true, {}, SourceKind::automatic}});
    REQUIRE(resolved);
    CHECK(paths(*resolved) == std::vector<std::string>{"src/a.hpp", "src/nested/main.cxx", "src/z.cpp"});
    CHECK((*resolved)[0].kind == SourceKind::header);
    CHECK((*resolved)[1].kind == SourceKind::source);
}

TEST_CASE("source resolver supports non-recursive scans and logical glob exclusions") {
    FixtureDirectory fixture;
    fixture.file("src/main.cpp");
    fixture.file("src/main_test.cpp");
    fixture.file("src/nested/child.cpp");

    const auto resolved = resolve_sources(
        fixture.path(), {{"src", false, {"**/*_test.cpp", "src/main.cpp"}, SourceKind::automatic}});
    REQUIRE(resolved);
    CHECK(resolved->empty());
}

TEST_CASE("source resolver preserves explicit files with unknown extensions") {
    FixtureDirectory fixture;
    fixture.file("assets/shader.metal");

    const auto resolved = resolve_sources(fixture.path(), {{"assets/shader.metal", true, {}, SourceKind::automatic}});
    REQUIRE(resolved);
    REQUIRE(resolved->size() == 1);
    CHECK((*resolved)[0].path == "assets/shader.metal");
    CHECK((*resolved)[0].kind == SourceKind::automatic);
}

TEST_CASE("source resolver applies an explicit kind to directory entries") {
    FixtureDirectory fixture;
    fixture.file("include/implementation.cpp");

    const auto resolved = resolve_sources(fixture.path(), {{"include", true, {}, SourceKind::header}});
    REQUIRE(resolved);
    REQUIRE(resolved->size() == 1);
    CHECK((*resolved)[0].kind == SourceKind::header);
}

TEST_CASE("source resolver reports missing paths as structured filesystem diagnostics") {
    FixtureDirectory fixture;
    const auto resolved = resolve_sources(fixture.path(), {{"missing", true, {}, SourceKind::automatic}});
    REQUIRE_FALSE(resolved);
    REQUIRE(resolved.error().diagnostics().size() == 1);
    CHECK(resolved.error().diagnostics()[0].code == diagnostic_codes::filesystem_error);
    CHECK(resolved.error().diagnostics()[0].path == "missing");
}

TEST_CASE("source resolver reports unreadable directories as filesystem diagnostics") {
#if defined(_WIN32)
    SKIP("POSIX directory permissions are unavailable on Windows");
#else
    FixtureDirectory fixture;
    fixture.file("src/blocked/hidden.cpp");
    std::error_code error;
    std::filesystem::permissions(fixture.path() / "src/blocked", std::filesystem::perms::none,
                                 std::filesystem::perm_options::replace, error);
    if (error) SKIP("could not remove directory permissions on this platform");

    const auto resolved = resolve_sources(fixture.path(), {{"src", true, {}, SourceKind::automatic}});
    std::filesystem::permissions(fixture.path() / "src/blocked", std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add, error);
    if (resolved) SKIP("test process can read directories without permissions");

    REQUIRE(resolved.error().diagnostics().size() == 1);
    CHECK(resolved.error().diagnostics()[0].code == diagnostic_codes::filesystem_error);
    CHECK(resolved.error().diagnostics()[0].path == "src");
#endif
}

TEST_CASE("source resolver does not traverse directory symlinks") {
    FixtureDirectory fixture;
    fixture.file("src/main.cpp");
    fixture.file("outside/hidden.cpp");
    std::error_code error;
    std::filesystem::create_directory_symlink(fixture.path() / "outside", fixture.path() / "src/link", error);
    if (error) SKIP("directory symlinks are unavailable on this platform");

    const auto resolved = resolve_sources(fixture.path(), {{"src", true, {}, SourceKind::automatic}});
    REQUIRE(resolved);
    CHECK(paths(*resolved) == std::vector<std::string>{"src/main.cpp"});
}

TEST_CASE("source resolver accepts normalized Windows-style source paths") {
    FixtureDirectory fixture;
    fixture.file("src/main.cpp");
    const auto resolved = resolve_sources(fixture.path(), {{"src\\main.cpp", true, {}, SourceKind::automatic}});
    REQUIRE(resolved);
    CHECK(paths(*resolved) == std::vector<std::string>{"src/main.cpp"});
}
