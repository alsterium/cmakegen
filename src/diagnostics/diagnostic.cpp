#include "cmakegen/diagnostics/diagnostic.hpp"

#include <algorithm>
#include <sstream>
#include <tuple>

namespace cmakegen {
namespace {

[[nodiscard]] std::string_view severity_name(DiagnosticSeverity severity) {
    switch (severity) {
    case DiagnosticSeverity::warning:
        return "warning";
    case DiagnosticSeverity::error:
        return "error";
    }

    return "error";
}

[[nodiscard]] int severity_order(DiagnosticSeverity severity) {
    return severity == DiagnosticSeverity::error ? 0 : 1;
}

[[nodiscard]] std::string colorize(std::string_view text, std::string_view color, bool enabled) {
    if (!enabled) {
        return std::string{text};
    }
    return "\x1b[" + std::string{color} + "m" + std::string{text} + "\x1b[0m";
}

[[nodiscard]] auto ordering_key(const Diagnostic& diagnostic) {
    const auto line = diagnostic.location ? diagnostic.location->line : 0;
    const auto column = diagnostic.location ? diagnostic.location->column : 0;
    return std::tuple<std::string_view, std::size_t, std::size_t, std::string_view, std::string_view>{
        diagnostic.path,
        line,
        column,
        diagnostic.code,
        diagnostic.message,
    };
}

[[nodiscard]] bool diagnostic_less(const Diagnostic& left, const Diagnostic& right) {
    if (ordering_key(left) != ordering_key(right)) {
        return ordering_key(left) < ordering_key(right);
    }
    return severity_order(left.severity) < severity_order(right.severity);
}

} // namespace

void DiagnosticSet::add(Diagnostic diagnostic) {
    const auto position = std::lower_bound(diagnostics_.begin(), diagnostics_.end(), diagnostic, diagnostic_less);
    diagnostics_.insert(position, std::move(diagnostic));
}

bool DiagnosticSet::has_errors() const noexcept {
    return std::ranges::any_of(diagnostics_, [](const Diagnostic& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::error;
    });
}

bool DiagnosticSet::empty() const noexcept {
    return diagnostics_.empty();
}

const std::vector<Diagnostic>& DiagnosticSet::diagnostics() const noexcept {
    return diagnostics_;
}

std::string format_diagnostic(const Diagnostic& diagnostic, const DiagnosticFormatOptions& options) {
    std::ostringstream output;
    const auto severity = severity_name(diagnostic.severity);
    const auto color = diagnostic.severity == DiagnosticSeverity::error ? "31" : "33";

    output << colorize(severity, color, options.color) << '[' << diagnostic.code << "]: "
           << diagnostic.message;
    if (!diagnostic.path.empty()) {
        output << "\n  at " << diagnostic.path;
        if (diagnostic.location) {
            output << " (line " << diagnostic.location->line << ", column "
                   << diagnostic.location->column << ')';
        }
    } else if (diagnostic.location) {
        output << "\n  at line " << diagnostic.location->line << ", column "
               << diagnostic.location->column;
    }
    return output.str();
}

std::string format_diagnostics(const DiagnosticSet& diagnostics, const DiagnosticFormatOptions& options) {
    std::ostringstream output;
    bool first = true;
    for (const auto& diagnostic : diagnostics.diagnostics()) {
        if (!first) {
            output << '\n';
        }
        output << format_diagnostic(diagnostic, options);
        first = false;
    }
    return output.str();
}

} // namespace cmakegen
