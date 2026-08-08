#include "cmakegen/diagnostics/diagnostic.hpp"

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <string>

namespace {

TEST_CASE("diagnostics are sorted deterministically by their logical location") {
    cmakegen::DiagnosticSet diagnostics;
    diagnostics.add({"CMG1004", cmakegen::DiagnosticSeverity::error, "unknown key", "targets.App", cmakegen::SourceLocation{4, 3}});
    diagnostics.add({"CMG1003", cmakegen::DiagnosticSeverity::error, "unknown target type", "targets.Core.type", cmakegen::SourceLocation{2, 1}});
    diagnostics.add({"CMG1002", cmakegen::DiagnosticSeverity::warning, "value is deprecated", "targets.App", cmakegen::SourceLocation{1, 1}});
    diagnostics.add({"CMG1001", cmakegen::DiagnosticSeverity::error, "invalid schema version", "targets.App", cmakegen::SourceLocation{1, 2}});

    const auto& ordered = diagnostics.diagnostics();
    REQUIRE(ordered.size() == 4);
    CHECK(ordered[0].path == "targets.App");
    CHECK((ordered[0].location == cmakegen::SourceLocation{1, 1}));
    CHECK(ordered[1].path == "targets.App");
    CHECK((ordered[1].location == cmakegen::SourceLocation{1, 2}));
    CHECK(ordered[3].path == "targets.Core.type");
    CHECK(diagnostics.has_errors());
}

TEST_CASE("human diagnostics retain the documented shape and can suppress color") {
    const cmakegen::Diagnostic diagnostic{
        std::string{cmakegen::diagnostic_codes::unknown_target_type},
        cmakegen::DiagnosticSeverity::error,
        "unknown target type \"static\"",
        "targets.Core.type",
        cmakegen::SourceLocation{8, 5},
    };

    CHECK(cmakegen::format_diagnostic(diagnostic) ==
          "error[CMG1003]: unknown target type \"static\"\n"
          "  at targets.Core.type (line 8, column 5)");
    CHECK(cmakegen::format_diagnostic(diagnostic, {.color = false}).find("\x1b[") == std::string::npos);
    CHECK(cmakegen::format_diagnostic(diagnostic, {.color = true}).find("\x1b[31m") == 0);
}

TEST_CASE("results transport diagnostics without exceptions") {
    cmakegen::DiagnosticSet diagnostics;
    diagnostics.add({"CMG1001", cmakegen::DiagnosticSeverity::error, "unsupported schema version", "schemaVersion", std::nullopt});

    cmakegen::Result<int> result = std::unexpected{diagnostics};

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().has_errors());
    CHECK(result.error().diagnostics().front().code == "CMG1001");
}

} // namespace
