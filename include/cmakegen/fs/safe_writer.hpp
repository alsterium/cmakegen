#pragma once

#include "cmakegen/diagnostics/diagnostic.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
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
