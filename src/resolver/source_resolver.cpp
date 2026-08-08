#include "cmakegen/resolver/source_resolver.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string_view>
#include <vector>

namespace cmakegen {
namespace {

[[nodiscard]] std::string slash_path(std::string value) {
    std::ranges::replace(value, '\\', '/');
    return value;
}

[[nodiscard]] std::string normalized_path(const std::filesystem::path& path) {
    return slash_path(path.lexically_normal().generic_string());
}

[[nodiscard]] std::string lowercase_extension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return extension;
}

[[nodiscard]] SourceKind extension_kind(const std::filesystem::path& path) {
    const auto extension = lowercase_extension(path);
    if (extension == ".c" || extension == ".cc" || extension == ".cpp" || extension == ".cxx") {
        return SourceKind::source;
    }
    if (extension == ".h" || extension == ".hh" || extension == ".hpp" || extension == ".hxx" ||
        extension == ".inl") {
        return SourceKind::header;
    }
    return SourceKind::automatic;
}

[[nodiscard]] bool glob_matches(std::string_view pattern, std::string_view value) {
    std::vector<std::int8_t> memo((pattern.size() + 1) * (value.size() + 1), -1);
    const auto matches = [&](const auto& self, std::size_t pattern_index, std::size_t value_index) -> bool {
        auto& cached = memo[pattern_index * (value.size() + 1) + value_index];
        if (cached != -1) return cached != 0;

        bool result = false;
        if (pattern_index == pattern.size()) {
            result = value_index == value.size();
        } else if (pattern[pattern_index] == '*') {
            const auto is_globstar = pattern_index + 1 < pattern.size() && pattern[pattern_index + 1] == '*';
            const auto next = pattern_index + (is_globstar ? 2 : 1);
            result = (is_globstar && next < pattern.size() && pattern[next] == '/' &&
                      self(self, next + 1, value_index)) ||
                     self(self, next, value_index) ||
                     (value_index < value.size() && (is_globstar || value[value_index] != '/') &&
                      self(self, pattern_index, value_index + 1));
        } else {
            result = value_index < value.size() &&
                     (pattern[pattern_index] == '?' ? value[value_index] != '/'
                                                    : pattern[pattern_index] == value[value_index]) &&
                     self(self, pattern_index + 1, value_index + 1);
        }
        cached = result ? 1 : 0;
        return result;
    };
    return matches(matches, 0, 0);
}

struct ResolvedSourceCandidate {
    ResolvedSource source;
    std::string diagnostic_path;
    std::optional<SourceLocation> diagnostic_location;
};

[[nodiscard]] bool excluded(const SourceEntry& entry, std::string_view project_relative,
                            std::string_view entry_relative) {
    return std::ranges::any_of(entry.exclude, [&](const std::string& pattern) {
        const auto normalized = slash_path(pattern);
        return glob_matches(normalized, project_relative) || glob_matches(normalized, entry_relative);
    });
}

void add_filesystem_diagnostic(DiagnosticSet& diagnostics, const SourceEntry& entry,
                               const std::string& message) {
    diagnostics.add({std::string{diagnostic_codes::filesystem_error}, DiagnosticSeverity::error,
                     message, entry.diagnostic_path.empty() ? entry.path : entry.diagnostic_path,
                     entry.diagnostic_location});
}

[[nodiscard]] std::filesystem::path entry_filesystem_path(const std::filesystem::path& base,
                                                           const SourceEntry& entry) {
    const auto logical_path = std::filesystem::path{slash_path(entry.path)};
    return logical_path.is_absolute() ? logical_path : base / logical_path;
}

[[nodiscard]] std::string rendered_path(const std::filesystem::path& base,
                                        const std::filesystem::path& file) {
    const auto relative = file.lexically_relative(base);
    return normalized_path(relative.empty() ? file : relative);
}

void add_resolved(std::vector<ResolvedSourceCandidate>& resolved, const std::filesystem::path& base,
                  const std::filesystem::path& file, SourceKind kind, const SourceEntry& entry) {
    resolved.push_back({{rendered_path(base, file), kind}, entry.diagnostic_path, entry.diagnostic_location});
}

} // namespace

Result<std::vector<ResolvedSource>> resolve_sources(const std::filesystem::path& project_directory,
                                                    const std::vector<SourceEntry>& entries) {
    DiagnosticSet diagnostics;
    std::vector<ResolvedSourceCandidate> resolved;
    const auto base = project_directory.lexically_normal();

    for (const auto& entry : entries) {
        const auto input_path = entry_filesystem_path(base, entry);
        std::error_code error;
        const auto status = std::filesystem::symlink_status(input_path, error);
        if (error || status.type() == std::filesystem::file_type::not_found) {
            add_filesystem_diagnostic(diagnostics, entry,
                                      "source path does not exist: " + entry.path);
            continue;
        }

        if (std::filesystem::is_regular_file(status) ||
            (std::filesystem::is_symlink(status) && std::filesystem::is_regular_file(input_path, error))) {
            if (error) {
                add_filesystem_diagnostic(diagnostics, entry,
                                          "could not inspect source file: " + entry.path);
            } else if (!excluded(entry, rendered_path(base, input_path), input_path.filename().generic_string())) {
                const auto detected_kind = extension_kind(input_path);
                add_resolved(resolved, base, input_path,
                             entry.kind == SourceKind::automatic ? detected_kind : entry.kind, entry);
            }
            continue;
        }

        const auto is_directory_symlink =
            std::filesystem::is_symlink(status) && std::filesystem::is_directory(input_path, error);
        if (error) {
            add_filesystem_diagnostic(diagnostics, entry,
                                      "could not inspect source path: " + entry.path);
            continue;
        }
        if (is_directory_symlink) {
            add_filesystem_diagnostic(diagnostics, entry,
                                      "source directory is a symlink and will not be traversed: " + entry.path);
            continue;
        }
        if (!std::filesystem::is_directory(status)) {
            add_filesystem_diagnostic(diagnostics, entry,
                                      "source path is neither a regular file nor a directory: " + entry.path);
            continue;
        }

        constexpr auto options = std::filesystem::directory_options::none;
        const auto add_directory_file = [&](const std::filesystem::path& file) {
            const auto kind = extension_kind(file);
            if (kind == SourceKind::automatic) return;
            const auto project_relative = rendered_path(base, file);
            const auto entry_relative = normalized_path(file.lexically_relative(input_path));
            if (!excluded(entry, project_relative, entry_relative)) {
                add_resolved(resolved, base, file,
                             entry.kind == SourceKind::automatic ? kind : entry.kind, entry);
            }
        };

        if (entry.recursive) {
            std::filesystem::recursive_directory_iterator iterator{input_path, options, error};
            const std::filesystem::recursive_directory_iterator end;
            while (!error && iterator != end) {
                const auto current = iterator->path();
                const auto current_status = iterator->symlink_status(error);
                if (error) break;
                if (std::filesystem::is_symlink(current_status) && std::filesystem::is_directory(current, error)) {
                    iterator.disable_recursion_pending();
                } else if (std::filesystem::is_regular_file(current, error)) {
                    add_directory_file(current);
                }
                iterator.increment(error);
            }
        } else {
            std::filesystem::directory_iterator iterator{input_path, options, error};
            const std::filesystem::directory_iterator end;
            while (!error && iterator != end) {
                const auto current_status = iterator->symlink_status(error);
                if (error) break;
                if (std::filesystem::is_regular_file(iterator->path(), error)) add_directory_file(iterator->path());
                iterator.increment(error);
            }
        }
        if (error) {
            add_filesystem_diagnostic(diagnostics, entry,
                                      "could not scan source directory: " + entry.path + ": " + error.message());
        }
    }

    std::ranges::sort(resolved, [](const ResolvedSourceCandidate& left, const ResolvedSourceCandidate& right) {
        return left.source.path == right.source.path ? left.source.kind < right.source.kind
                                                      : left.source.path < right.source.path;
    });
    for (std::size_t index = 1; index < resolved.size(); ++index) {
        if (resolved[index - 1].source.path == resolved[index].source.path &&
            resolved[index - 1].source.kind != resolved[index].source.kind) {
            diagnostics.add({std::string{diagnostic_codes::invalid_value}, DiagnosticSeverity::error,
                             "source path resolves with conflicting kinds: " + resolved[index].source.path,
                             resolved[index].diagnostic_path.empty() ? resolved[index].source.path
                                                                      : resolved[index].diagnostic_path,
                             resolved[index].diagnostic_location});
        }
    }
    if (diagnostics.has_errors()) return std::unexpected{std::move(diagnostics)};
    resolved.erase(std::unique(resolved.begin(), resolved.end(), [](const ResolvedSourceCandidate& left,
                                                                     const ResolvedSourceCandidate& right) {
                       return left.source.path == right.source.path && left.source.kind == right.source.kind;
                   }),
                   resolved.end());
    std::vector<ResolvedSource> result;
    result.reserve(resolved.size());
    for (auto& candidate : resolved) result.push_back(std::move(candidate.source));
    return result;
}

} // namespace cmakegen
