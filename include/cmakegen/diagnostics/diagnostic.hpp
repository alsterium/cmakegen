#pragma once

#include <compare>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cmakegen {

enum class DiagnosticSeverity {
    warning,
    error,
};

struct SourceLocation {
    std::size_t line;
    std::size_t column;

    auto operator<=>(const SourceLocation&) const = default;
};

struct Diagnostic {
    std::string code;
    DiagnosticSeverity severity;
    std::string message;
    std::string path;
    std::optional<SourceLocation> location;
};

class DiagnosticSet {
public:
    void add(Diagnostic diagnostic);

    [[nodiscard]] bool has_errors() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const noexcept;

private:
    std::vector<Diagnostic> diagnostics_;
};

struct DiagnosticFormatOptions {
    bool color = false;
};

[[nodiscard]] std::string format_diagnostic(
    const Diagnostic& diagnostic,
    const DiagnosticFormatOptions& options = {});
[[nodiscard]] std::string format_diagnostics(
    const DiagnosticSet& diagnostics,
    const DiagnosticFormatOptions& options = {});

template <typename T>
using Result = std::expected<T, DiagnosticSet>;

namespace diagnostic_codes {

inline constexpr std::string_view invalid_schema_version = "CMG1001";
inline constexpr std::string_view invalid_value = "CMG1002";
inline constexpr std::string_view unknown_target_type = "CMG1003";
inline constexpr std::string_view unknown_key = "CMG1004";
inline constexpr std::string_view filesystem_error = "CMG2001";
inline constexpr std::string_view semantic_error = "CMG3001";
inline constexpr std::string_view dependency_cycle = "CMG3002";

} // namespace diagnostic_codes

} // namespace cmakegen
