#include "cmakegen/fs/safe_writer.hpp"
#include "cmakegen/fs/detail/windows_case_capability.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string_view>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

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

class HeldTransactionLock {
public:
    explicit HeldTransactionLock(const std::filesystem::path& path) {
        std::filesystem::create_directories(path.parent_path());
#ifdef _WIN32
        handle_ = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) return;
        OVERLAPPED overlapped{};
        if (!::LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                          0, MAXDWORD, MAXDWORD, &overlapped)) {
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        descriptor_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0666);
        if (descriptor_ == -1 || ::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            if (descriptor_ != -1) ::close(descriptor_);
            descriptor_ = -1;
        }
#endif
    }

    ~HeldTransactionLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            OVERLAPPED overlapped{};
            (void)::UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped);
            (void)::CloseHandle(handle_);
        }
#else
        if (descriptor_ != -1) {
            (void)::flock(descriptor_, LOCK_UN);
            (void)::close(descriptor_);
        }
#endif
    }

    HeldTransactionLock(const HeldTransactionLock&) = delete;
    HeldTransactionLock& operator=(const HeldTransactionLock&) = delete;

    [[nodiscard]] bool held() const noexcept {
#ifdef _WIN32
        return handle_ != INVALID_HANDLE_VALUE;
#else
        return descriptor_ != -1;
#endif
    }

private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int descriptor_ = -1;
#endif
};

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

class FailureInjectionScope {
public:
    explicit FailureInjectionScope(cmakegen::detail::safe_writer_testing::FailureInjection injection) {
        cmakegen::detail::safe_writer_testing::set_failure_injection(injection);
    }
    ~FailureInjectionScope() { cmakegen::detail::safe_writer_testing::clear_failure_injection(); }
    FailureInjectionScope(const FailureInjectionScope&) = delete;
    FailureInjectionScope& operator=(const FailureInjectionScope&) = delete;
};

using FailurePoint = cmakegen::detail::safe_writer_testing::FailurePoint;

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

TEST_CASE("safe writer rolls back before, during, and after replacement failures") {
    const auto run = [](FailurePoint point, std::size_t operation_index) {
        TemporaryDirectory fixture;
        cmakegen::SafeWriter writer{fixture.path()};
        const auto previous = file_set({{"first.txt", "first-old\n"}, {"second.txt", "second-old\n"}});
        REQUIRE(writer.write(previous));
        const auto previous_manifest = read_file(fixture.path() / ".cmakegen/manifest.json");
        const auto next = file_set({{"first.txt", "first-new\n"}, {"second.txt", "second-new\n"}});

        FailureInjectionScope injection{{point, operation_index}};
        const auto result = writer.write(next);
        REQUIRE_FALSE(result);
        CHECK(read_file(fixture.path() / "first.txt") == "first-old\n");
        CHECK(read_file(fixture.path() / "second.txt") == "second-old\n");
        CHECK(read_file(fixture.path() / ".cmakegen/manifest.json") == previous_manifest);
        CHECK_FALSE(std::filesystem::exists(fixture.path() / ".cmakegen/transaction"));
        CHECK(result.error().diagnostics().front().message.find("rollback completed") != std::string::npos);
    };

    SECTION("before replacement") { run(FailurePoint::before_replacement, 0); }
    SECTION("during the second replacement") { run(FailurePoint::during_replacement, 1); }
    SECTION("after the first replacement") { run(FailurePoint::after_replacement, 0); }
}

TEST_CASE("safe writer rolls back output replacements when manifest update fails") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"first.txt", "first-old\n"}, {"second.txt", "second-old\n"}})));
    const auto previous_manifest = read_file(fixture.path() / ".cmakegen/manifest.json");

    FailureInjectionScope injection{{FailurePoint::manifest_update, 0}};
    const auto result = writer.write(file_set({{"first.txt", "first-new\n"}, {"second.txt", "second-new\n"}}));
    REQUIRE_FALSE(result);
    CHECK(read_file(fixture.path() / "first.txt") == "first-old\n");
    CHECK(read_file(fixture.path() / "second.txt") == "second-old\n");
    CHECK(read_file(fixture.path() / ".cmakegen/manifest.json") == previous_manifest);
    CHECK_FALSE(std::filesystem::exists(fixture.path() / ".cmakegen/transaction"));
}

TEST_CASE("safe writer removes a partial staged output during rollback") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"generated.txt", "old content\n"}})));

    {
        FailureInjectionScope injection{{FailurePoint::during_staging, 0}};
        const auto result = writer.write(file_set({{"generated.txt", "new content\n"}}));
        REQUIRE_FALSE(result);
        CHECK(result.error().diagnostics().front().message.find("rollback completed") != std::string::npos);
    }

    CHECK(read_file(fixture.path() / "generated.txt") == "old content\n");
    CHECK_FALSE(std::filesystem::exists(fixture.path() / ".generated.txt.cmakegen-transaction.tmp"));
    CHECK_FALSE(std::filesystem::exists(fixture.path() / ".cmakegen/transaction"));
}

TEST_CASE("safe writer preserves a staging path it never claimed") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"generated.txt", "old content\n"}})));

    const auto staging = fixture.path() / ".generated.txt.cmakegen-transaction.tmp";
    write_file(staging, "user-owned staging content\n");
    const auto result = writer.write(file_set({{"generated.txt", "new content\n"}}));

    REQUIRE_FALSE(result);
    CHECK(read_file(fixture.path() / "generated.txt") == "old content\n");
    CHECK(read_file(staging) == "user-owned staging content\n");
    CHECK_FALSE(std::filesystem::exists(fixture.path() / ".cmakegen/transaction"));
}

TEST_CASE("safe writer rolls back a file-to-directory transition") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"generated", "old\n"}})));
    const auto previous_manifest = read_file(fixture.path() / ".cmakegen/manifest.json");

    {
        FailureInjectionScope injection{{FailurePoint::during_replacement, 0}};
        const auto result = writer.write(file_set({{"generated/child.txt", "new\n"}}));
        REQUIRE_FALSE(result);
        CHECK(result.error().diagnostics().front().message.find("rollback completed") != std::string::npos);
    }

    CHECK(read_file(fixture.path() / "generated") == "old\n");
    CHECK_FALSE(std::filesystem::exists(fixture.path() / "generated/child.txt"));
    CHECK(read_file(fixture.path() / ".cmakegen/manifest.json") == previous_manifest);
    CHECK_FALSE(std::filesystem::exists(fixture.path() / ".cmakegen/transaction"));
}

TEST_CASE("safe writer removes recreated parents when staging fails") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"generated", "old\n"}})));

    {
        FailureInjectionScope injection{{FailurePoint::during_staging, 0}};
        const auto result = writer.write(file_set({{"generated/child.txt", "new\n"}}));
        REQUIRE_FALSE(result);
        CHECK(result.error().diagnostics().front().message.find("rollback completed") != std::string::npos);
    }

    CHECK(read_file(fixture.path() / "generated") == "old\n");
    CHECK_FALSE(std::filesystem::exists(fixture.path() / "generated/child.txt"));
    CHECK_FALSE(std::filesystem::exists(fixture.path() / ".cmakegen/transaction"));
}

TEST_CASE("safe writer journals output directories only after creating them") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"generated", "old\n"}})));

    {
        FailureInjectionScope injection{{FailurePoint::during_staging, 0}};
        cmakegen::detail::safe_writer_testing::queue_failure_injection({FailurePoint::rollback, 0});
        const auto result = writer.write(file_set({{"generated/child.txt", "new\n"}}));
        REQUIRE_FALSE(result);
        CHECK(result.error().diagnostics().front().message.find("requires operator recovery") != std::string::npos);
    }

    const auto transaction = fixture.path() / ".cmakegen/transaction";
    const auto journal = nlohmann::json::parse(read_file(transaction / "journal.json"));
    REQUIRE(journal["created_directories"].is_array());
    REQUIRE(journal["created_directories"].size() == 1);
    CHECK(journal["created_directories"][0] == "generated");
}

TEST_CASE("safe writer preserves an indistinguishable directory created while ownership was in progress") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"generated", "old\n"}})));

    {
        FailureInjectionScope injection{{FailurePoint::during_staging, 0}};
        cmakegen::detail::safe_writer_testing::queue_failure_injection({FailurePoint::rollback, 0});
        REQUIRE_FALSE(writer.write(file_set({{"generated/child.txt", "new\n"}})));
    }

    const auto transaction = fixture.path() / ".cmakegen/transaction";
    auto journal = nlohmann::json::parse(read_file(transaction / "journal.json"));
    journal["created_directories"] = nlohmann::json::array();
    journal["creating_directory"] = "generated";
    write_file(transaction / "journal.json", journal.dump(2) + "\n");

    const auto recovered = writer.write(file_set({{"generated/child.txt", "new\n"}}));
    CHECK_FALSE(recovered);
    CHECK(recovered.error().diagnostics().front().message.find("requires operator recovery") != std::string::npos);
    CHECK(std::filesystem::is_directory(fixture.path() / "generated"));
    CHECK(std::filesystem::exists(transaction));
}

TEST_CASE("safe writer reports platform-specific locked-file replacement failures") {
#ifndef _WIN32
    SKIP("POSIX permits renaming an open file; Windows share-mode locking is tested on Windows");
#else
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"locked.txt", "old\n"}})));
    const auto locked_path = fixture.path() / "locked.txt";
    const auto handle = ::CreateFileW(locked_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) SKIP("could not acquire a Windows share-mode lock");
    const auto result = writer.write(file_set({{"locked.txt", "new\n"}}));
    ::CloseHandle(handle);
    CHECK_FALSE(result);
    CHECK(read_file(locked_path) == "old\n");
    CHECK_FALSE(std::filesystem::exists(fixture.path() / ".cmakegen/transaction"));
#endif
}

TEST_CASE("safe writer retains deterministic recovery artifacts when rollback fails") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"first.txt", "first-old\n"}, {"second.txt", "second-old\n"}})));
    const auto next = file_set({{"first.txt", "first-new\n"}, {"second.txt", "second-new\n"}});

    {
        FailureInjectionScope injection{{FailurePoint::during_replacement, 1}};
        cmakegen::detail::safe_writer_testing::queue_failure_injection({FailurePoint::rollback, 0});
        const auto result = writer.write(next);
        REQUIRE_FALSE(result);
    }

    const auto transaction = fixture.path() / ".cmakegen/transaction";
    REQUIRE(std::filesystem::exists(transaction / "journal.json"));
    REQUIRE(std::filesystem::exists(transaction / "pre-manifest.json"));
    REQUIRE(std::filesystem::exists(transaction / "post-manifest.json"));
    CHECK(read_file(transaction / "journal.json").find("\"state\": \"recovery_required\"") != std::string::npos);
    CHECK(std::filesystem::exists(transaction / "backups"));
    write_file(transaction / "journal.json.tmp", "interrupted journal write\n");

    {
        FailureInjectionScope recovery_injection{{FailurePoint::recovery, 0}};
        const auto recovery = writer.write(next);
        REQUIRE_FALSE(recovery);
        CHECK(std::filesystem::exists(transaction / "journal.json"));
        CHECK(recovery.error().diagnostics().front().message.find("requires operator recovery") != std::string::npos);
    }

    REQUIRE(writer.write(next));
    CHECK(read_file(fixture.path() / "first.txt") == "first-new\n");
    CHECK(read_file(fixture.path() / "second.txt") == "second-new\n");
    CHECK_FALSE(std::filesystem::exists(transaction));
}

TEST_CASE("safe writer transaction journal round-trips UTF-8 filesystem paths") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    const auto relative = utf8_path("generated/\xE6\x96\x87\xE4\xBB\xB6.txt");
    REQUIRE(writer.write(file_set({{relative, "old\n"}})));

    {
        FailureInjectionScope injection{{FailurePoint::during_replacement, 0}};
        cmakegen::detail::safe_writer_testing::queue_failure_injection({FailurePoint::rollback, 0});
        REQUIRE_FALSE(writer.write(file_set({{relative, "new\n"}})));
    }

    const auto transaction = fixture.path() / ".cmakegen/transaction";
    const auto journal = nlohmann::json::parse(read_file(transaction / "journal.json"));
    REQUIRE(journal["entries"].size() == 2);
    CHECK(journal["entries"][0]["destination"] == "generated/\xE6\x96\x87\xE4\xBB\xB6.txt");
    CHECK(journal["entries"][0]["temporary"] ==
          "generated/.\xE6\x96\x87\xE4\xBB\xB6.txt.cmakegen-transaction.tmp");

    REQUIRE(writer.write(file_set({{relative, "new\n"}})));
    CHECK(read_file(fixture.path() / relative) == "new\n");
    CHECK_FALSE(std::filesystem::exists(transaction));
}

TEST_CASE("safe writer refuses to recover or write while another writer holds the transaction lock") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"generated.txt", "old\n"}})));

    HeldTransactionLock lock{fixture.path() / ".cmakegen/transaction.lock"};
    REQUIRE(lock.held());
    const auto result = writer.write(file_set({{"generated.txt", "new\n"}}));

    REQUIRE_FALSE(result);
    CHECK(result.error().diagnostics().front().message.find("another SafeWriter writer is active") != std::string::npos);
    CHECK(read_file(fixture.path() / "generated.txt") == "old\n");
    CHECK_FALSE(std::filesystem::exists(fixture.path() / ".cmakegen/transaction"));
}

TEST_CASE("safe writer validates a retained backup before removing its destination") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"first.txt", "first-old\n"}, {"second.txt", "second-old\n"}})));
    const auto next = file_set({{"first.txt", "first-new\n"}, {"second.txt", "second-new\n"}});

    {
        FailureInjectionScope injection{{FailurePoint::during_replacement, 1}};
        cmakegen::detail::safe_writer_testing::queue_failure_injection({FailurePoint::rollback, 0});
        REQUIRE_FALSE(writer.write(next));
    }

    const auto transaction = fixture.path() / ".cmakegen/transaction";
    const auto first_backup = transaction / "backups/000000.bak";
    REQUIRE(std::filesystem::exists(first_backup));
    write_file(first_backup, "corrupted backup\n");
    const auto recovery = writer.write(next);

    REQUIRE_FALSE(recovery);
    CHECK(read_file(fixture.path() / "first.txt") == "first-new\n");
    CHECK(read_file(first_backup) == "corrupted backup\n");
    CHECK(std::filesystem::exists(transaction / "journal.json"));
}

TEST_CASE("safe writer verifies retained pre-manifest outputs before finalizing rollback") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"changed.txt", "old\n"}, {"retained.txt", "retained\n"}})));

    {
        FailureInjectionScope injection{{FailurePoint::during_replacement, 0}};
        cmakegen::detail::safe_writer_testing::queue_failure_injection({FailurePoint::rollback, 0});
        REQUIRE_FALSE(writer.write(file_set({{"changed.txt", "new\n"}, {"retained.txt", "retained\n"}})));
    }

    const auto transaction = fixture.path() / ".cmakegen/transaction";
    write_file(fixture.path() / "retained.txt", "external edit\n");
    const auto recovery = writer.write(file_set({{"changed.txt", "new\n"}, {"retained.txt", "retained\n"}}));

    REQUIRE_FALSE(recovery);
    CHECK(recovery.error().diagnostics().front().message.find("manifest-owned output") != std::string::npos);
    CHECK(read_file(fixture.path() / "retained.txt") == "external edit\n");
    CHECK(std::filesystem::exists(transaction / "journal.json"));
}

TEST_CASE("safe writer preserves staging paths recreated after publication") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"generated.txt", "old\n"}})));

    {
        FailureInjectionScope injection{{FailurePoint::after_replacement, 0}};
        cmakegen::detail::safe_writer_testing::queue_failure_injection({FailurePoint::rollback, 0});
        REQUIRE_FALSE(writer.write(file_set({{"generated.txt", "new\n"}})));
    }

    const auto transaction = fixture.path() / ".cmakegen/transaction";
    const auto staging = fixture.path() / ".generated.txt.cmakegen-transaction.tmp";
    write_file(staging, "new\n");
    const auto post_metadata = nlohmann::json::parse(read_file(transaction / "post-manifest.json"));
    write_file(fixture.path() / ".cmakegen/manifest.json", post_metadata["content"].get<std::string>());
    auto journal = nlohmann::json::parse(read_file(transaction / "journal.json"));
    journal["state"] = "committed";
    journal["phase"] = "commit";
    write_file(transaction / "journal.json", journal.dump(2) + "\n");

    REQUIRE(writer.write(file_set({{"generated.txt", "new\n"}})));
    CHECK(read_file(staging) == "new\n");
    CHECK_FALSE(std::filesystem::exists(transaction));
}

TEST_CASE("safe writer preserves staging paths recreated after rollback") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"generated.txt", "old\n"}})));

    {
        FailureInjectionScope injection{{FailurePoint::after_replacement, 0}};
        cmakegen::detail::safe_writer_testing::queue_failure_injection({FailurePoint::rollback, 0});
        REQUIRE_FALSE(writer.write(file_set({{"generated.txt", "new\n"}})));
    }

    const auto transaction = fixture.path() / ".cmakegen/transaction";
    auto journal = nlohmann::json::parse(read_file(transaction / "journal.json"));
    REQUIRE_FALSE(journal["entries"][0]["staging_claimed"].get<bool>());
    journal["state"] = "rolled_back";
    journal["phase"] = "rollback";
    write_file(transaction / "journal.json", journal.dump(2) + "\n");
    write_file(fixture.path() / "generated.txt", "old\n");
    const auto pre_metadata = nlohmann::json::parse(read_file(transaction / "pre-manifest.json"));
    write_file(fixture.path() / ".cmakegen/manifest.json", pre_metadata["content"].get<std::string>());
    const auto staging = fixture.path() / ".generated.txt.cmakegen-transaction.tmp";
    write_file(staging, "new\n");

    REQUIRE(writer.write(file_set({{"generated.txt", "old\n"}})));
    CHECK(read_file(staging) == "new\n");
    CHECK_FALSE(std::filesystem::exists(transaction));
}

TEST_CASE("safe writer preserves staging recreated after an interrupted publication") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"generated.txt", "old\n"}})));

    {
        FailureInjectionScope injection{{FailurePoint::after_replacement, 0}};
        cmakegen::detail::safe_writer_testing::queue_failure_injection({FailurePoint::rollback, 0});
        REQUIRE_FALSE(writer.write(file_set({{"generated.txt", "new\n"}})));
    }

    const auto transaction = fixture.path() / ".cmakegen/transaction";
    const auto staging = fixture.path() / ".generated.txt.cmakegen-transaction.tmp";
    write_file(staging, "new\n");
    auto journal = nlohmann::json::parse(read_file(transaction / "journal.json"));
    journal["entries"][0]["status"] = "in_progress";
    journal["entries"][0]["staging_claimed"] = true;
    write_file(transaction / "journal.json", journal.dump(2) + "\n");

    const auto recovery = writer.write(file_set({{"generated.txt", "new\n"}}));
    CHECK_FALSE(recovery);
    CHECK(recovery.error().diagnostics().front().message.find("staging ownership is ambiguous") != std::string::npos);
    CHECK(read_file(staging) == "new\n");
    CHECK(std::filesystem::exists(transaction / "journal.json"));
}

TEST_CASE("safe writer preserves backups recreated after rollback") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"generated.txt", "old\n"}})));

    {
        FailureInjectionScope injection{{FailurePoint::during_replacement, 0}};
        cmakegen::detail::safe_writer_testing::queue_failure_injection({FailurePoint::rollback, 0});
        REQUIRE_FALSE(writer.write(file_set({{"generated.txt", "new\n"}})));
    }

    const auto transaction = fixture.path() / ".cmakegen/transaction";
    auto journal = nlohmann::json::parse(read_file(transaction / "journal.json"));
    journal["entries"][0]["status"] = "restored";
    journal["state"] = "rolled_back";
    journal["phase"] = "rollback";
    write_file(transaction / "journal.json", journal.dump(2) + "\n");
    const auto backup = transaction / journal["entries"][0]["backup"].get<std::string>();
    write_file(backup, "external backup\n");

    const auto recovery = writer.write(file_set({{"generated.txt", "new\n"}}));
    CHECK_FALSE(recovery);
    CHECK(recovery.error().diagnostics().front().message.find("backup recreated after restoration") != std::string::npos);
    CHECK(read_file(backup) == "external backup\n");
    CHECK(std::filesystem::exists(transaction / "journal.json"));
}

TEST_CASE("safe writer preserves backups changed after commit") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"generated.txt", "old\n"}})));

    {
        FailureInjectionScope injection{{FailurePoint::after_replacement, 0}};
        cmakegen::detail::safe_writer_testing::queue_failure_injection({FailurePoint::rollback, 0});
        REQUIRE_FALSE(writer.write(file_set({{"generated.txt", "new\n"}})));
    }

    const auto transaction = fixture.path() / ".cmakegen/transaction";
    const auto post_metadata = nlohmann::json::parse(read_file(transaction / "post-manifest.json"));
    write_file(fixture.path() / ".cmakegen/manifest.json", post_metadata["content"].get<std::string>());
    auto journal = nlohmann::json::parse(read_file(transaction / "journal.json"));
    journal["state"] = "committed";
    journal["phase"] = "commit";
    write_file(transaction / "journal.json", journal.dump(2) + "\n");
    const auto backup = transaction / journal["entries"][0]["backup"].get<std::string>();
    write_file(backup, "external backup\n");

    const auto recovery = writer.write(file_set({{"generated.txt", "new\n"}}));
    CHECK_FALSE(recovery);
    CHECK(recovery.error().diagnostics().front().message.find("backup changed after commit") != std::string::npos);
    CHECK(read_file(backup) == "external backup\n");
    CHECK(std::filesystem::exists(transaction / "journal.json"));
}

TEST_CASE("safe writer preserves unclaimed directories at removed destinations during recovery") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"obsolete", "old\n"}})));

    {
        FailureInjectionScope injection{{FailurePoint::during_replacement, 0}};
        cmakegen::detail::safe_writer_testing::queue_failure_injection({FailurePoint::rollback, 0});
        REQUIRE_FALSE(writer.write(file_set({{"replacement.txt", "new\n"}})));
    }

    const auto transaction = fixture.path() / ".cmakegen/transaction";
    std::filesystem::create_directory(fixture.path() / "obsolete");
    const auto recovery = writer.write(file_set({{"replacement.txt", "new\n"}}));

    REQUIRE_FALSE(recovery);
    CHECK(recovery.error().diagnostics().front().message.find("stale destination changed") != std::string::npos);
    CHECK(std::filesystem::is_directory(fixture.path() / "obsolete"));
    CHECK(std::filesystem::exists(transaction / "journal.json"));
}

TEST_CASE("safe writer refuses symlinked backup parents during cleanup") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    const auto next = file_set({{"generated.txt", "new\n"}});

    {
        FailureInjectionScope injection{{FailurePoint::during_replacement, 0}};
        cmakegen::detail::safe_writer_testing::queue_failure_injection({FailurePoint::rollback, 0});
        const auto result = writer.write(next);
        REQUIRE_FALSE(result);
        CHECK(result.error().diagnostics().front().message.find("requires operator recovery") != std::string::npos);
    }

    const auto transaction = fixture.path() / ".cmakegen/transaction";
    const auto external_backups = fixture.path() / "external-backups";
    std::filesystem::create_directories(external_backups);
    write_file(external_backups / "000000.bak", "external backup\n");
    std::error_code error;
    std::filesystem::rename(transaction / "backups", fixture.path() / "parked-backups", error);
    REQUIRE_FALSE(error);
    std::filesystem::create_directory_symlink(external_backups, transaction / "backups", error);
    if (error) SKIP("filesystem cannot create directory symlinks");

    auto journal = nlohmann::json::parse(read_file(transaction / "journal.json"));
    journal["state"] = "committed";
    write_file(transaction / "journal.json", journal.dump(2) + "\n");

    const auto recovery = writer.write(next);
    REQUIRE_FALSE(recovery);
    CHECK(recovery.error().diagnostics().front().message.find("symbolic link") != std::string::npos);
    CHECK(read_file(external_backups / "000000.bak") == "external backup\n");
    CHECK(std::filesystem::exists(transaction / "journal.json"));
}

TEST_CASE("safe writer refuses symlinked backup parents during restoration") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    const auto next = file_set({{"generated.txt", "new\n"}});

    {
        FailureInjectionScope injection{{FailurePoint::during_replacement, 0}};
        cmakegen::detail::safe_writer_testing::queue_failure_injection({FailurePoint::rollback, 0});
        const auto result = writer.write(next);
        REQUIRE_FALSE(result);
        CHECK(result.error().diagnostics().front().message.find("requires operator recovery") != std::string::npos);
    }

    const auto transaction = fixture.path() / ".cmakegen/transaction";
    const auto external_backups = fixture.path() / "external-restoration-backups";
    std::filesystem::create_directories(external_backups);
    write_file(external_backups / "000000.bak", "external backup\n");
    std::error_code error;
    std::filesystem::rename(transaction / "backups", fixture.path() / "parked-restoration-backups", error);
    REQUIRE_FALSE(error);
    std::filesystem::create_directory_symlink(external_backups, transaction / "backups", error);
    if (error) SKIP("filesystem cannot create directory symlinks");

    const auto recovery = writer.write(next);
    REQUIRE_FALSE(recovery);
    CHECK(recovery.error().diagnostics().front().message.find("symbolic link") != std::string::npos);
    CHECK(read_file(external_backups / "000000.bak") == "external backup\n");
    CHECK(std::filesystem::exists(transaction / "journal.json"));
}

TEST_CASE("safe writer rejects symlinked parents while confirming post-state") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"out/generated.txt", "old\n"}})));
    const auto next = file_set({{"out/generated.txt", "new\n"}});

    {
        FailureInjectionScope injection{{FailurePoint::manifest_update, 0}};
        cmakegen::detail::safe_writer_testing::queue_failure_injection({FailurePoint::rollback, 0});
        const auto result = writer.write(next);
        REQUIRE_FALSE(result);
        CHECK(result.error().diagnostics().front().message.find("requires operator recovery") != std::string::npos);
    }

    const auto transaction = fixture.path() / ".cmakegen/transaction";
    const auto external_output = fixture.path() / "external-output";
    std::filesystem::create_directories(external_output);
    write_file(external_output / "generated.txt", "new\n");
    std::error_code error;
    std::filesystem::remove_all(fixture.path() / "out", error);
    REQUIRE_FALSE(error);
    std::filesystem::create_directory_symlink(external_output, fixture.path() / "out", error);
    if (error) SKIP("filesystem cannot create directory symlinks");

    const auto post_metadata = nlohmann::json::parse(read_file(transaction / "post-manifest.json"));
    write_file(fixture.path() / ".cmakegen/manifest.json", post_metadata["content"].get<std::string>());
    auto journal = nlohmann::json::parse(read_file(transaction / "journal.json"));
    journal["state"] = "manifest_pending";
    write_file(transaction / "journal.json", journal.dump(2) + "\n");

    const auto recovery = writer.write(next);
    REQUIRE_FALSE(recovery);
    CHECK(recovery.error().diagnostics().front().message.find("symbolic link") != std::string::npos);
    CHECK(read_file(external_output / "generated.txt") == "new\n");
    CHECK(std::filesystem::exists(transaction / "journal.json"));
}

TEST_CASE("safe writer rejects symlinked parents while confirming pre-state") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"first/generated.txt", "first-old\n"},
                                   {"second/generated.txt", "second-old\n"}})));
    const auto next = file_set({{"first/generated.txt", "first-new\n"},
                                {"second/generated.txt", "second-new\n"}});

    {
        FailureInjectionScope injection{{FailurePoint::during_replacement, 1}};
        cmakegen::detail::safe_writer_testing::queue_failure_injection({FailurePoint::rollback, 1});
        const auto result = writer.write(next);
        REQUIRE_FALSE(result);
        CHECK(result.error().diagnostics().front().message.find("requires operator recovery") != std::string::npos);
    }

    const auto transaction = fixture.path() / ".cmakegen/transaction";
    const auto external_output = fixture.path() / "external-output";
    std::filesystem::create_directories(external_output);
    write_file(external_output / "generated.txt", "second-old\n");
    std::error_code error;
    std::filesystem::remove_all(fixture.path() / "second", error);
    REQUIRE_FALSE(error);
    std::filesystem::create_directory_symlink(external_output, fixture.path() / "second", error);
    if (error) SKIP("filesystem cannot create directory symlinks");

    const auto recovery = writer.write(next);
    REQUIRE_FALSE(recovery);
    CHECK(recovery.error().diagnostics().front().message.find("symbolic link") != std::string::npos);
    CHECK(read_file(external_output / "generated.txt") == "second-old\n");
    CHECK(std::filesystem::exists(transaction / "journal.json"));
}

TEST_CASE("safe writer cleans recoverable journal-less transaction scaffolding") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};

    SECTION("transaction setup interrupted before the journal") {
        std::filesystem::create_directories(fixture.path() / ".cmakegen/transaction/backups");
    }

    SECTION("initial journal publication interrupted before replacement") {
        const auto transaction = fixture.path() / ".cmakegen/transaction";
        std::filesystem::create_directories(transaction / "backups");
        write_file(transaction / "pre-manifest.json", "metadata\n");
        write_file(transaction / "post-manifest.json", "metadata\n");
        write_file(transaction / "journal.json.tmp", "stale\n");
    }

    SECTION("unexpected artifacts preserve recognized backups") {
        const auto transaction = fixture.path() / ".cmakegen/transaction";
        const auto backup = transaction / "backups/000000.bak";
        std::filesystem::create_directories(backup.parent_path());
        write_file(backup, "pre-transaction\n");
        write_file(transaction / "unexpected", "do not remove\n");

        const auto result = writer.write(file_set({{"generated.txt", "generated\n"}}));
        REQUIRE_FALSE(result);
        CHECK(read_file(backup) == "pre-transaction\n");
        CHECK(read_file(transaction / "unexpected") == "do not remove\n");
        return;
    }

    SECTION("backup without a journal requires operator inspection") {
        const auto transaction = fixture.path() / ".cmakegen/transaction";
        const auto backup = transaction / "backups/000000.bak";
        std::filesystem::create_directories(backup.parent_path());
        write_file(backup, "external\n");

        const auto result = writer.write(file_set({{"generated.txt", "generated\n"}}));
        REQUIRE_FALSE(result);
        CHECK(result.error().diagnostics().front().message.find("unverifiable backup") != std::string::npos);
        CHECK(read_file(backup) == "external\n");
        return;
    }

    REQUIRE(writer.write(file_set({{"generated.txt", "generated\n"}})));
    CHECK(read_file(fixture.path() / "generated.txt") == "generated\n");
    CHECK_FALSE(std::filesystem::exists(fixture.path() / ".cmakegen/transaction"));
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

TEST_CASE("safe writer rejects outputs that collide with deterministic staging paths") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    const auto result = writer.write(file_set({
        {"foo", "foo\n"},
        {".foo.cmakegen-transaction.tmp", "temporary\n"},
    }));

    REQUIRE_FALSE(result);
    CHECK(result.error().diagnostics().front().message.find("staging path") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(fixture.path() / ".cmakegen/manifest.json"));
}

TEST_CASE("safe writer rejects descendants of deterministic staging paths") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    const auto result = writer.write(file_set({
        {"foo", "foo\n"},
        {".foo.cmakegen-transaction.tmp/child", "child\n"},
    }));

    REQUIRE_FALSE(result);
    CHECK(result.error().diagnostics().front().message.find("staging path") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(fixture.path() / ".cmakegen/manifest.json"));
}

TEST_CASE("safe writer rejects retained outputs that collide with staging paths") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{".foo.cmakegen-transaction.tmp", "retained\n"}})));

    const auto result = writer.write(file_set({{"foo", "new\n"}}), {.cleanup_stale = false});
    REQUIRE_FALSE(result);
    CHECK(result.error().diagnostics().front().message.find("staging path") != std::string::npos);
    CHECK(read_file(fixture.path() / ".foo.cmakegen-transaction.tmp") == "retained\n");
    CHECK_FALSE(std::filesystem::exists(fixture.path() / ".cmakegen/transaction"));
}

TEST_CASE("safe writer rejects missing or modified retained outputs before opening a transaction") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"retained.txt", "old\n"}, {"changed.txt", "first\n"}})));

    std::filesystem::remove(fixture.path() / "retained.txt");
    const auto missing = writer.write(file_set({{"changed.txt", "second\n"}}), {.cleanup_stale = false});
    CHECK_FALSE(missing);
    CHECK(missing.error().diagnostics().front().message.find("retained manifest-owned output") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(fixture.path() / ".cmakegen/transaction"));
    CHECK(read_file(fixture.path() / "changed.txt") == "first\n");

    write_file(fixture.path() / "retained.txt", "external\n");
    const auto modified = writer.write(file_set({{"changed.txt", "second\n"}}), {.cleanup_stale = false});
    CHECK_FALSE(modified);
    CHECK(modified.error().diagnostics().front().message.find("retained manifest-owned output") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(fixture.path() / ".cmakegen/transaction"));
    CHECK(read_file(fixture.path() / "changed.txt") == "first\n");
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

TEST_CASE("safe writer allows output under a similarly prefixed metadata directory") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};

    REQUIRE(writer.write(file_set({{".cmakegenerator/output.txt", "contents\n"}})));
    CHECK(read_file(fixture.path() / ".cmakegenerator/output.txt") == "contents\n");
}

TEST_CASE("safe writer keeps portable ASCII aliases host independent") {
    cmakegen::GeneratedFileSet files;
    REQUIRE(files.add({"CaseSensitive.txt", "one\n"}));
    CHECK_FALSE(files.add({"casesensitive.txt", "two\n"}));

    cmakegen::GeneratedFileSet win32_aliases;
    REQUIRE(win32_aliases.add({"output", "one\n"}));
    CHECK_FALSE(win32_aliases.add({"output. ", "two\n"}));
}

TEST_CASE("Windows directory case query fallback requires a known insensitive volume") {
    using cmakegen::detail::WindowsVolumeCaseSensitivity;
    using cmakegen::detail::windows_directory_case_query_fallback;

    CHECK_FALSE(windows_directory_case_query_fallback(
                    WindowsVolumeCaseSensitivity::indeterminate).has_value());
    CHECK(windows_directory_case_query_fallback(
              WindowsVolumeCaseSensitivity::case_insensitive) == false);
    CHECK_FALSE(windows_directory_case_query_fallback(
                    WindowsVolumeCaseSensitivity::case_sensitive).has_value());
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

TEST_CASE("safe writer validates macOS Unicode paths at existing target parents") {
    TemporaryDirectory fixture;
    std::filesystem::create_directory(fixture.path() / "output");
    cmakegen::SafeWriter writer{fixture.path()};
    const std::string filename = "caf\xc3\xa9.txt";
    REQUIRE(writer.write(file_set({{utf8_path("output/" + filename), "contents\n"}})));
    CHECK(std::filesystem::exists(fixture.path() / "output" / utf8_path(filename)));
    REQUIRE(writer.clean());
    CHECK_FALSE(std::filesystem::exists(fixture.path() / "output" / utf8_path(filename)));
}

TEST_CASE("safe writer accepts distinct ASCII parents beside macOS Unicode output") {
    TemporaryDirectory fixture;
    std::filesystem::create_directory(fixture.path() / "output");
    cmakegen::SafeWriter writer{fixture.path()};
    const std::string filename = "caf\xc3\xa9.txt";
    REQUIRE(writer.write(file_set({{utf8_path("output/" + filename), "unicode\n"},
                                   {"other/plain.txt", "plain\n"}})));
    CHECK(std::filesystem::exists(fixture.path() / "output" / utf8_path(filename)));
    CHECK(std::filesystem::exists(fixture.path() / "other/plain.txt"));
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
    CHECK_FALSE(updated);
    CHECK(read_file(fixture.path() / utf8_path(upper)) == "first\n");
    REQUIRE(writer.clean());
    CHECK_FALSE(std::filesystem::exists(fixture.path() / utf8_path(upper)));
}

TEST_CASE("safe writer preserves portable aliases below macOS Unicode-equivalent parents") {
    TemporaryDirectory fixture;
    const std::string composed_parent = "caf\xc3\xa9";
    const std::string decomposed_parent = "cafe\xcc\x81";
    if (!filesystem_has_alias(fixture.path(), composed_parent, decomposed_parent)) {
        SKIP("filesystem does not normalize this Unicode parent alias");
    }

    cmakegen::GeneratedFileSet files;
    REQUIRE(files.add({utf8_path(composed_parent + "/Foo.txt"), "first\n"}));
    REQUIRE(files.add({utf8_path(decomposed_parent + "/foo.txt"), "second\n"}));
    cmakegen::SafeWriter writer{fixture.path()};
    CHECK_FALSE(writer.write(files));
    CHECK_FALSE(std::filesystem::exists(fixture.path() / ".cmakegen/manifest.json"));
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

    const auto nested_files = file_set({{utf8_path("nested/" + filename), "nested\n"}});
    REQUIRE(writer.write(nested_files));
    CHECK(std::filesystem::exists(fixture.path() / "nested" / utf8_path(filename)));
    REQUIRE(writer.clean());
    CHECK_FALSE(std::filesystem::exists(fixture.path() / "nested" / utf8_path(filename)));

    const std::string emoji_directory = "\xf0\x9f\x98\x80";
    std::filesystem::create_directory(fixture.path() / utf8_path(emoji_directory));
    const auto nested_unicode_files = file_set(
        {{utf8_path(emoji_directory + "/" + filename), "nested unicode\n"}});
    REQUIRE(writer.write(nested_unicode_files));
    CHECK(std::filesystem::exists(fixture.path() / utf8_path(emoji_directory) / utf8_path(filename)));
    REQUIRE(writer.clean());
    CHECK_FALSE(std::filesystem::exists(fixture.path() / utf8_path(emoji_directory) / utf8_path(filename)));

    const std::string emoji_filename = "\xf0\x9f\x98\x80.txt";
    const auto emoji_files = file_set({{utf8_path(emoji_filename), "emoji\n"}});
    REQUIRE(writer.write(emoji_files));
    CHECK(std::filesystem::exists(fixture.path() / utf8_path(emoji_filename)));
    REQUIRE(writer.clean());
    CHECK_FALSE(std::filesystem::exists(fixture.path() / utf8_path(emoji_filename)));
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

TEST_CASE("safe writer keeps ASCII outputs independent of unrelated Unicode entries") {
    TemporaryDirectory fixture;
    write_file(fixture.path() / utf8_path("caf\xc3\xa9.txt"), "user\n");
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"generated.txt", "generated\n"}})));
    CHECK(read_file(fixture.path() / "generated.txt") == "generated\n");
    CHECK(read_file(fixture.path() / utf8_path("caf\xc3\xa9.txt")) == "user\n");
}

TEST_CASE("safe writer recognizes manifest ownership through portable path aliases") {
    TemporaryDirectory fixture;
    cmakegen::SafeWriter writer{fixture.path()};
    REQUIRE(writer.write(file_set({{"Foo.txt", "first\n"}})));

    const auto updated = writer.write(file_set({{"foo.txt", "second\n"}}));
    CHECK_FALSE(updated);
    CHECK(read_file(fixture.path() / "Foo.txt") == "first\n");
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
