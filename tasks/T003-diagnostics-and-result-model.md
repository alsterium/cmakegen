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
