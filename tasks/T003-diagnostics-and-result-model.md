# T003 — Diagnostics and result model

## Purpose

Provide consistent, testable user-error propagation before parser and
filesystem components are built.

## Scope

- Define diagnostic codes, severity, message, logical YAML path, and optional
  source location.
- Define `DiagnosticSet` and the project result abstraction selected in T001.
- Add human-readable formatting with color suppression support at the formatting
  boundary.
- Add focused tests for ordering and the documented diagnostic shape.

## Dependencies

T002.

## Acceptance criteria

- Expected user errors can cross component boundaries without exception-based
  normal control flow.
- Diagnostics can retain paths such as `targets.Core.sources[1].exclude`.
- Multiple diagnostics have deterministic ordering.
- Formatting can produce the human-readable form required by the CLI spec.

## Verification

- Run diagnostic/result unit tests.
- Build with each locally available supported compiler.

## Execution record

- Date: 2026-08-08
- Summary: Added the reusable diagnostics module with stable CMG code constants,
  severities, logical YAML paths, optional 1-based locations, deterministic
  ordering, and CLI-boundary human formatting with optional ANSI color. Added
  the `Result<T>` alias for `std::expected<T, DiagnosticSet>` and focused unit
  coverage for ordering, formatting, color suppression, and result propagation.
- Verification: `cmake -S bootstrap -B build/bootstrap && cmake --build
  build/bootstrap --parallel 2 && ctest --test-dir build/bootstrap
  --output-on-failure && build/bootstrap/cmakegen --help && git diff --check`
  (pass; 4/4 tests). Local `clang++` and `g++` both identify as Apple Clang
  21.0.0; the bootstrap verification used that locally available compiler.
- Generated files: unchanged; normal generated CMake does not exist until T014.
- Follow-ups: T004 consumes the diagnostics and `Result<T>` interfaces for YAML
  parsing and schema validation.

## Execution record — review follow-up

- Date: 2026-08-08
- Summary: Changed diagnostic ordering keys to own their location values rather
  than retain references to temporary local variables during sorting. Extended
  ordering coverage with multiple source locations on the same logical path.
- Verification: `cmake --build build/bootstrap --parallel 2 && ctest --test-dir
  build/bootstrap --output-on-failure && git diff --check` (pass; 4/4 tests).
- Generated files: unchanged.
- Follow-ups: none.

## Execution record — review follow-up 2

- Date: 2026-08-08
- Summary: Replaced full-set sorting on every diagnostic insertion with sorted
  insertion, and made comparator string fields non-owning views while retaining
  location values. This preserves deterministic ordering without repeatedly
  copying diagnostic strings or performing full re-sorts.
- Verification: `cmake --build build/bootstrap --parallel 2 && ctest --test-dir
  build/bootstrap --output-on-failure && git diff --check` (pass; 4/4 tests).
- Generated files: unchanged.
- Follow-ups: none.
