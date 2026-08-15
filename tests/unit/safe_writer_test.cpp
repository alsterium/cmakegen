#include "cmakegen/fs/safe_writer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string_view>

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

[[nodiscard]] std::filesystem::path utf8_path(std::string_view value) {
#if defined(_WIN32)
    return std::filesystem::u8path(value);
#else
    return std::filesystem::path{std::string{value}};
#endif
}

#if defined(__APPLE__) || defined(_WIN32)
[[nodiscard]] bool filesystem_has_alias(const std::filesystem::path& root, std::string_view first,
                                        std::string_view second) {
    const auto first_path = root / utf8_path(first);
    const auto second_path = root / utf8_path(second);
    write_file(first_path, "probe\n");
    const bool aliases = std::filesystem::exists(second_path) && read_file(second_path) == "probe\n";
    std::error_code error;
    std::filesystem::remove(first_path, error);
    error.clear();
    std::filesystem::remove(second_path, error);
    return aliases;
}
#endif

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

TEST_CASE("safe writer verifies a manifest-owned file before replacing it") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"generated.txt", "first\n"}})));
    write_file(fixture.path() / "generated.txt", "user edit\n");

    CHECK_FALSE(writer.write(file_set({{"generated.txt", "second\n"}})));
    CHECK(read_file(fixture.path() / "generated.txt") == "user edit\n");
    REQUIRE(writer.write(file_set({{"generated.txt", "second\n"}}), {.force = true}));
    CHECK(read_file(fixture.path() / "generated.txt") == "second\n");
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

TEST_CASE("disabling stale cleanup retains ownership of files omitted from this run") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"old.txt", "old\n"}, {"keep.txt", "keep\n"}})));

    const auto updated = writer.write(file_set({{"keep.txt", "keep\n"}}), {.cleanup_stale = false});
    REQUIRE(updated);
    CHECK(std::filesystem::exists(fixture.path() / "old.txt"));
    const auto manifest = read_file(fixture.path() / ".cmakegen/manifest.json");
    CHECK(manifest.find("\"path\": \"old.txt\"") != std::string::npos);
    REQUIRE(writer.clean());
    CHECK_FALSE(std::filesystem::exists(fixture.path() / "old.txt"));
    CHECK_FALSE(std::filesystem::exists(fixture.path() / "keep.txt"));
}

TEST_CASE("disabling stale cleanup updates hashes for regenerated files") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"old.txt", "old\n"}, {"keep.txt", "first\n"}})));

    REQUIRE(writer.write(file_set({{"keep.txt", "second\n"}}), {.cleanup_stale = false}));
    REQUIRE(writer.clean());
    CHECK_FALSE(std::filesystem::exists(fixture.path() / "old.txt"));
    CHECK_FALSE(std::filesystem::exists(fixture.path() / "keep.txt"));
}

TEST_CASE("stale cleanup and clean preserve manifest-owned files modified by users") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"stale.txt", "generated\n"}, {"keep.txt", "keep\n"}})));
    write_file(fixture.path() / "stale.txt", "user edit\n");

    CHECK_FALSE(writer.write(file_set({{"keep.txt", "keep\n"}})));
    CHECK(std::filesystem::exists(fixture.path() / "stale.txt"));
    REQUIRE(writer.write(file_set({{"keep.txt", "keep\n"}}), {.force = true}));
    REQUIRE(writer.write(file_set({{"clean.txt", "generated\n"}})));
    write_file(fixture.path() / "clean.txt", "user edit\n");
    CHECK_FALSE(writer.clean());
    CHECK(std::filesystem::exists(fixture.path() / "clean.txt"));
    REQUIRE(writer.clean(false, true));
    CHECK_FALSE(std::filesystem::exists(fixture.path() / "clean.txt"));
}

TEST_CASE("stale owned files can become output directories after verified cleanup") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"generated", "first\n"}})));

    const auto transitioned = writer.write(file_set({{"generated/child.txt", "child\n"}}));
    REQUIRE(transitioned);
    CHECK(transitioned->files_removed == 1);
    CHECK(read_file(fixture.path() / "generated/child.txt") == "child\n");

    TemporaryDirectory modified_fixture;
    cmakegen::SafeWriter modified_writer{modified_fixture.path()};
    REQUIRE(modified_writer.write(file_set({{"generated", "first\n"}})));
    write_file(modified_fixture.path() / "generated", "user edit\n");
    CHECK_FALSE(modified_writer.write(file_set({{"generated/child.txt", "child\n"}})));
    REQUIRE(modified_writer.write(file_set({{"generated/child.txt", "child\n"}}), {.force = true}));
    CHECK(read_file(modified_fixture.path() / "generated/child.txt") == "child\n");
}

TEST_CASE("safe writer rejects portable aliases of its manifest path") {
    TemporaryDirectory fixture;
    const auto alias = fixture.path() / ".CmakeGen/Manifest.JSON";
    write_file(alias, "{\"version\": 1, \"files\": []}\n");

    bool distinct_alias = false;
    for (const auto& entry : std::filesystem::directory_iterator(fixture.path())) {
        distinct_alias = entry.path().filename() == ".CmakeGen";
    }
    if (!distinct_alias) SKIP("filesystem does not support distinct portable metadata aliases");

    cmakegen::SafeWriter writer{fixture.path()};
    CHECK_FALSE(writer.write(file_set({{"generated.txt", "contents\n"}})));
    CHECK_FALSE(writer.clean());
    std::size_t manifest_directory_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(fixture.path()))
        if (entry.is_directory() &&
            (entry.path().filename() == ".CmakeGen" || entry.path().filename() == ".cmakegen"))
            ++manifest_directory_count;
    // Do not query the canonical spelling directly: on a case-insensitive
    // filesystem it is the same directory as the intentional alias.
    CHECK(manifest_directory_count == 1);
}

TEST_CASE("safe writer rejects portable metadata aliases and output collisions during preflight") {
    cmakegen::GeneratedFileSet metadata_alias;
    CHECK_FALSE(metadata_alias.add({".CMakeGen/manifest.json", "bad\n"}));
    CHECK_FALSE(metadata_alias.add({".CMAKEGEN", "bad\n"}));
    CHECK_FALSE(metadata_alias.add({".cmakegen./manifest.json", "bad\n"}));
    CHECK_FALSE(metadata_alias.add({".cmakegen /manifest.json", "bad\n"}));

    cmakegen::GeneratedFileSet duplicates;
    REQUIRE(duplicates.add({"CMakeLists.txt", "one\n"}));
    CHECK_FALSE(duplicates.add({"cmakelists.TXT", "two\n"}));

    cmakegen::GeneratedFileSet ancestors;
    REQUIRE(ancestors.add({"directory", "file\n"}));
    CHECK_FALSE(ancestors.add({"directory/child.txt", "child\n"}));

    cmakegen::GeneratedFileSet case_folded_ancestors;
    REQUIRE(case_folded_ancestors.add({"Directory", "file\n"}));
    CHECK_FALSE(case_folded_ancestors.add({"directory/child.txt", "child\n"}));

    cmakegen::GeneratedFileSet win32_aliases;
    REQUIRE(win32_aliases.add({"output", "one\n"}));
    CHECK_FALSE(win32_aliases.add({"output. ", "two\n"}));

    cmakegen::GeneratedFileSet directory_destinations;
    CHECK_FALSE(directory_destinations.add({"directory/file/", "bad\n"}));
    CHECK_FALSE(directory_destinations.add({"directory/file/.", "bad\n"}));
}

TEST_CASE("safe writer keeps portable ASCII aliases host independent") {
    cmakegen::GeneratedFileSet files;
    REQUIRE(files.add({"CaseSensitive.txt", "one\n"}));
    CHECK_FALSE(files.add({"casesensitive.txt", "two\n"}));

    cmakegen::GeneratedFileSet win32_aliases;
    REQUIRE(win32_aliases.add({"output", "one\n"}));
    CHECK_FALSE(win32_aliases.add({"output. ", "two\n"}));
}

TEST_CASE("safe writer applies Win32 trailing-dot-space aliases during ownership lookup") {
    TemporaryDirectory fixture;
    write_file(fixture.path() / "output", "user\n");
    cmakegen::SafeWriter writer{fixture.path()};

    CHECK_FALSE(writer.write(file_set({{"output. ", "generated\n"}})));
    CHECK(read_file(fixture.path() / "output") == "user\n");
    CHECK_FALSE(std::filesystem::exists(fixture.path() / ".cmakegen/manifest.json"));
}

TEST_CASE("safe writer rejects portable aliases when a case-sensitive host stores both spellings") {
    TemporaryDirectory fixture;
    write_file(fixture.path() / "Foo.txt", "generated\n");
    write_file(fixture.path() / "foo.txt", "user\n");

    if (!std::filesystem::exists(fixture.path() / "Foo.txt") ||
        !std::filesystem::exists(fixture.path() / "foo.txt") ||
        read_file(fixture.path() / "Foo.txt") == read_file(fixture.path() / "foo.txt")) {
        SKIP("filesystem does not support distinct case-only aliases");
    }

    cmakegen::SafeWriter writer{fixture.path()};
    CHECK_FALSE(writer.write(file_set({{"FOO.TXT", "replacement\n"}})));
    CHECK(read_file(fixture.path() / "Foo.txt") == "generated\n");
    CHECK(read_file(fixture.path() / "foo.txt") == "user\n");
}

#if defined(__APPLE__)
TEST_CASE("safe writer follows macOS Unicode normalization equivalence") {
    TemporaryDirectory fixture;
    const std::string composed = "caf\xc3\xa9.txt";
    const std::string decomposed = "cafe\xcc\x81.txt";
    const bool aliases = filesystem_has_alias(fixture.path(), composed, decomposed);

    cmakegen::GeneratedFileSet files;
    REQUIRE(files.add({utf8_path(composed), "composed\n"}));
    REQUIRE(files.add({utf8_path(decomposed), "decomposed\n"}));
    cmakegen::SafeWriter writer{fixture.path()};
    const auto result = writer.write(files);
    if (aliases) {
        CHECK_FALSE(result);
        CHECK_FALSE(std::filesystem::exists(fixture.path() / ".cmakegen/manifest.json"));
    } else {
        REQUIRE(result);
        CHECK(std::filesystem::exists(fixture.path() / utf8_path(composed)));
        CHECK(std::filesystem::exists(fixture.path() / utf8_path(decomposed)));
        const auto manifest = read_file(fixture.path() / ".cmakegen/manifest.json");
        const auto rerun = writer.write(files);
        REQUIRE(rerun);
        CHECK_FALSE(rerun->would_change);
        CHECK(read_file(fixture.path() / ".cmakegen/manifest.json") == manifest);
    }
}

TEST_CASE("safe writer uses macOS Unicode case aliases for ownership and cleanup") {
    TemporaryDirectory fixture;
    const std::string upper = "\xc3\x85.txt";
    const std::string lower = "\xc3\xa5.txt";
    if (!filesystem_has_alias(fixture.path(), upper, lower)) {
        SKIP("filesystem is not case-insensitive for this Unicode alias");
    }

    cmakegen::SafeWriter writer{fixture.path()};
    const auto first = writer.write(file_set({{utf8_path(upper), "first\n"}}));
    REQUIRE(first);
    const auto updated = writer.write(file_set({{utf8_path(lower), "second\n"}}));
    REQUIRE(updated);
    CHECK(updated->files_written == 1);
    CHECK(read_file(fixture.path() / utf8_path(upper)) == "second\n");
    REQUIRE(writer.clean());
    CHECK_FALSE(std::filesystem::exists(fixture.path() / utf8_path(upper)));
}

TEST_CASE("safe writer preserves distinct macOS Unicode names and manifest order") {
    TemporaryDirectory fixture;
    const std::string accented = "caf\xc3\xa9.txt";
    const std::string plain = "cafe.txt";
    CHECK_FALSE(filesystem_has_alias(fixture.path(), accented, plain));

    cmakegen::GeneratedFileSet files;
    REQUIRE(files.add({utf8_path(accented), "accented\n"}));
    REQUIRE(files.add({utf8_path(plain), "plain\n"}));
    cmakegen::SafeWriter writer{fixture.path()};
    const auto result = writer.write(files);
    REQUIRE(result);
    const auto manifest = read_file(fixture.path() / ".cmakegen/manifest.json");
    const auto rerun = writer.write(files);
    REQUIRE(rerun);
    CHECK_FALSE(rerun->would_change);
    CHECK(read_file(fixture.path() / ".cmakegen/manifest.json") == manifest);
}
#elif defined(_WIN32)
TEST_CASE("safe writer preserves native Windows paths as UTF-8 during preflight") {
    TemporaryDirectory fixture;
    const std::string filename = "\xc3\x85.txt";
    cmakegen::GeneratedFileSet files;
    REQUIRE(files.add({utf8_path(filename), "contents\n"}));
    REQUIRE(files.files().size() == 1);
    CHECK(files.files().front().path == utf8_path(filename));

    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(files));
    CHECK(std::filesystem::exists(fixture.path() / utf8_path(filename)));
    CHECK(read_file(fixture.path() / ".cmakegen/manifest.json").find(filename) != std::string::npos);
    REQUIRE(writer.write(file_set({{"replacement.txt", "replacement\n"}})));
    CHECK_FALSE(std::filesystem::exists(fixture.path() / utf8_path(filename)));

    REQUIRE(writer.write(files));
    REQUIRE(writer.clean());
    CHECK_FALSE(std::filesystem::exists(fixture.path() / utf8_path(filename)));
}

TEST_CASE("safe writer follows Windows Unicode case capability") {
    TemporaryDirectory fixture;
    const std::string upper = "\xc3\x85.txt";
    const std::string lower = "\xc3\xa5.txt";
    const bool aliases = filesystem_has_alias(fixture.path(), upper, lower);

    cmakegen::GeneratedFileSet files;
    REQUIRE(files.add({utf8_path(upper), "upper\n"}));
    REQUIRE(files.add({utf8_path(lower), "lower\n"}));
    cmakegen::SafeWriter writer{fixture.path()};
    const auto result = writer.write(files);
    if (aliases) {
        CHECK_FALSE(result);
    } else {
        REQUIRE(result);
        CHECK(std::filesystem::exists(fixture.path() / utf8_path(upper)));
        CHECK(std::filesystem::exists(fixture.path() / utf8_path(lower)));
    }
}
#else
TEST_CASE("safe writer fails safely when Unicode filesystem capability is unsupported") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    const auto result = writer.write(file_set({{"caf\xc3\xa9.txt", "content\n"}}));
    CHECK_FALSE(result);
    CHECK_FALSE(std::filesystem::exists(fixture.path() / ".cmakegen/manifest.json"));
}
#endif

TEST_CASE("safe writer recognizes manifest ownership through portable path aliases") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"Foo.txt", "first\n"}})));

    const auto updated = writer.write(file_set({{"foo.txt", "second\n"}}));
    REQUIRE(updated);
    CHECK(read_file(fixture.path() / "Foo.txt") == "second\n");
    REQUIRE(writer.clean());
    CHECK_FALSE(std::filesystem::exists(fixture.path() / "Foo.txt"));
}

TEST_CASE("safe writer rejects ambiguous portable aliases before ownership lookup") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"Foo.txt", "generated\n"}})));
    write_file(fixture.path() / "foo.txt", "user file\n");

    // Case-insensitive filesystems cannot represent both names.  The safety
    // condition only applies where the second physical alias exists.
    if (!std::filesystem::exists(fixture.path() / "Foo.txt") ||
        !std::filesystem::exists(fixture.path() / "foo.txt") ||
        read_file(fixture.path() / "Foo.txt") == read_file(fixture.path() / "foo.txt")) {
        SKIP("filesystem does not support distinct case-only aliases");
    }

    CHECK_FALSE(writer.write(file_set({{"Foo.txt", "updated\n"}})));
    CHECK(read_file(fixture.path() / "Foo.txt") == "generated\n");
    CHECK(read_file(fixture.path() / "foo.txt") == "user file\n");
}

TEST_CASE("safe writer rejects an existing file ancestor before writing") {
    TemporaryDirectory fixture;
    write_file(fixture.path() / "directory", "user file\n");
    cmakegen::SafeWriter writer{fixture.path()};
    CHECK_FALSE(writer.write(file_set({{"directory/child.txt", "generated\n"}})));
    CHECK(read_file(fixture.path() / "directory") == "user file\n");
    CHECK_FALSE(std::filesystem::exists(fixture.path() / ".cmakegen/manifest.json"));
}

TEST_CASE("safe writer rejects case-folded existing file ancestors before writing") {
    TemporaryDirectory fixture;
    write_file(fixture.path() / "Directory", "user file\n");
    cmakegen::SafeWriter writer{fixture.path()};
    CHECK_FALSE(writer.write(file_set({{"directory/child.txt", "generated\n"}})));
    CHECK(read_file(fixture.path() / "Directory") == "user file\n");
    CHECK_FALSE(std::filesystem::exists(fixture.path() / ".cmakegen/manifest.json"));
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
