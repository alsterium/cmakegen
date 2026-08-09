#include "cmakegen/fs/safe_writer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() : path_(std::filesystem::temp_directory_path() /
                               ("cmakegen-safe-writer-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path_, error); }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << content;
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] cmakegen::GeneratedFileSet file_set(std::initializer_list<cmakegen::GeneratedFile> files) {
    cmakegen::GeneratedFileSet result;
    for (auto file : files) REQUIRE(result.add(std::move(file)));
    return result;
}

TEST_CASE("safe writer writes deterministic manifests and skips no-op writes") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    const auto files = file_set({{"z/CMakeLists.txt", "z\n"}, {"CMakeLists.txt", "root\n"}});

    const auto first = writer.write(files);
    REQUIRE(first);
    CHECK(first->files_written == 2);
    const auto manifest = fixture.path() / ".cmakegen/manifest.json";
    const auto first_manifest = read_file(manifest);
    CHECK(first_manifest.find("\"path\": \"CMakeLists.txt\"") < first_manifest.find("\"path\": \"z/CMakeLists.txt\""));
    CHECK(cmakegen::SafeWriter::sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    const auto second = writer.write(files);
    REQUIRE(second);
    CHECK_FALSE(second->would_change);
    CHECK(second->files_written == 0);
    CHECK(read_file(manifest) == first_manifest);
}

TEST_CASE("manifest safely serializes valid paths that require JSON escaping") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    const auto files = file_set({{"quoted\"name.txt", "contents\n"}});
    REQUIRE(writer.write(files));
    const auto rerun = writer.write(files);
    REQUIRE(rerun);
    CHECK_FALSE(rerun->would_change);
}

TEST_CASE("safe writer protects unowned files unless force is requested") {
    TemporaryDirectory fixture;
    write_file(fixture.path() / "CMakeLists.txt", "user\n");
    cmakegen::SafeWriter writer{fixture.path()};
    const auto files = file_set({{"CMakeLists.txt", "generated\n"}});

    CHECK_FALSE(writer.write(files));
    CHECK(read_file(fixture.path() / "CMakeLists.txt") == "user\n");
    const auto forced = writer.write(files, {.force = true});
    REQUIRE(forced);
    CHECK(read_file(fixture.path() / "CMakeLists.txt") == "generated\n");
}

TEST_CASE("check and dry-run perform no writes while reporting pending changes") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    const auto files = file_set({{"generated.txt", "contents\n"}});

    const auto checked = writer.write(files, {.check = true});
    REQUIRE(checked);
    CHECK(checked->would_change);
    CHECK_FALSE(std::filesystem::exists(fixture.path() / "generated.txt"));
    CHECK_FALSE(std::filesystem::exists(fixture.path() / ".cmakegen/manifest.json"));
    const auto dry = writer.write(files, {.dry_run = true});
    REQUIRE(dry);
    CHECK(dry->would_change);
    CHECK_FALSE(std::filesystem::exists(fixture.path() / "generated.txt"));
}

TEST_CASE("stale cleanup and clean affect only manifest-owned files") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"old.txt", "old\n"}, {"keep.txt", "keep\n"}})));
    write_file(fixture.path() / "user.txt", "user\n");

    const auto updated = writer.write(file_set({{"keep.txt", "keep\n"}}));
    REQUIRE(updated);
    CHECK(updated->files_removed == 1);
    CHECK_FALSE(std::filesystem::exists(fixture.path() / "old.txt"));
    CHECK(std::filesystem::exists(fixture.path() / "user.txt"));
    const auto dry_clean = writer.clean(true);
    REQUIRE(dry_clean);
    CHECK(std::filesystem::exists(fixture.path() / "keep.txt"));
    REQUIRE(writer.clean());
    CHECK_FALSE(std::filesystem::exists(fixture.path() / "keep.txt"));
    CHECK(std::filesystem::exists(fixture.path() / "user.txt"));
}

TEST_CASE("unsafe paths and failed preflight cannot alter output outside the root") {
    TemporaryDirectory fixture;
    TemporaryDirectory outside;
    const auto outside_file = outside.path() / "sentinel.txt";
    write_file(outside_file, "sentinel\n");
    cmakegen::SafeWriter writer{fixture.path()};
    cmakegen::GeneratedFileSet unsafe;
    CHECK_FALSE(unsafe.add({"../sentinel.txt", "changed\n"}));
    const auto valid = file_set({{"valid.txt", "valid\n"}});
    write_file(fixture.path() / "valid.txt", "user\n");
    CHECK_FALSE(writer.write(valid));
    CHECK(read_file(outside_file) == "sentinel\n");
    CHECK_FALSE(std::filesystem::exists(fixture.path() / ".cmakegen/manifest.json"));
}

} // namespace
