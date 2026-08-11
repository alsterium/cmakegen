#pragma once

#include "cmakegen/diagnostics/diagnostic.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cmakegen {

struct GeneratedFile {
    std::filesystem::path path;
    std::string content;
};

class GeneratedFileSet {
public:
    [[nodiscard]] Result<void> add(GeneratedFile file);
    [[nodiscard]] const std::vector<GeneratedFile>& files() const noexcept;

private:
    std::vector<GeneratedFile> files_;
};

struct WriteOptions {
    bool check = false;
    bool dry_run = false;
    bool force = false;
    bool cleanup_stale = true;
};

struct WriteResult {
    std::size_t files_written = 0;
    std::size_t files_removed = 0;
    bool would_change = false;
};

// This seam is intentionally kept in an internal namespace.  It lets the
// filesystem tests exercise failure points that are otherwise timing- and
// platform-dependent without adding failure controls to the production write
// options or CLI.
namespace detail::safe_writer_testing {

enum class FailurePoint {
    before_replacement,
    during_replacement,
    after_replacement,
    manifest_update,
    rollback,
    recovery,
};

struct FailureInjection {
    FailurePoint point;
    std::size_t operation_index = 0;
};

void set_failure_injection(FailureInjection injection);
void queue_failure_injection(FailureInjection injection);
void clear_failure_injection();

} // namespace detail::safe_writer_testing

/// Owns generation output below one project root.  All GeneratedFile paths are
/// relative to that root; absolute and parent-traversal paths are rejected.
class SafeWriter {
public:
    explicit SafeWriter(std::filesystem::path project_root);

    [[nodiscard]] Result<WriteResult> write(const GeneratedFileSet& files,
                                             const WriteOptions& options = {}) const;
    /// Refuses to remove generated files modified since their recorded hash,
    /// unless force is explicitly requested.
    [[nodiscard]] Result<WriteResult> clean(bool dry_run = false, bool force = false) const;

    [[nodiscard]] static std::string sha256(std::string_view content);

private:
    std::filesystem::path project_root_;
};

} // namespace cmakegen
