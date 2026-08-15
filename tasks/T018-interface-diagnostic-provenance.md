# T018 — Provenance-carrying interface diagnostics

## Purpose

Preserve the exact YAML source provenance of scoped values so semantic
diagnostics for invalid interface-library configuration identify the offending
entry, not only its scope.

## Scope

- Define a provenance-carrying representation for scoped values, replacing the
  relevant `ScopedValues<string>` uses without losing the value or scope.
- Carry the existing source-location information from parsing through
  normalization and semantic validation.
- Report the exact YAML entry location for interface-library diagnostics
  involving non-dependency scoped values.
- Update rendering and tests that consume the normalized scoped-value model,
  and review compatibility before changing shared interfaces.
- Preserve deterministic normalization and existing valid-project behavior.

## Dependencies

T008.

## Acceptance criteria

- An invalid interface-library scoped value produces a diagnostic containing
  the exact YAML entry location supported by the parser's location model.
- Provenance survives normalization for common and platform-scoped values and
  remains associated with the effective value selected for validation.
- Valid projects render exactly as before, apart from intentional diagnostic
  metadata changes.
- Tests cover the relevant common/platform combinations and verify stable
  diagnostic locations and ordering.
- Any compatibility impact on shared domain or renderer interfaces is
  documented before implementation is merged.

## Verification

- Add or update parser, normalization, semantic-validation, and diagnostic
  tests, including invalid interface-library scoped values.
- Run `cmake -S bootstrap -B build/bootstrap`,
  `cmake --build build/bootstrap --parallel 2`, and
  `ctest --test-dir build/bootstrap --output-on-failure`.
- Run `git diff --check` and inspect generated-file consistency if the shared
  model affects self-hosted output.

## Source handoff

Recorded in [T008](T008-dependency-graph-and-semantic-validation.md): the
current diagnostic retains the scope but not the exact YAML entry location.

## Execution record

- Date: 2026-08-12
- Summary: Added provenance-carrying normalized scoped string values for common
  and platform include directories, definitions, and compile options. Preserved
  parser paths and source locations through normalization, surfaced exact entry
  provenance in interface-library semantic diagnostics, adapted rendering,
  dumps, and CLI path rebasing, and documented the shared-model compatibility
  decision in ADR-013.
- Verification: `cmake -S bootstrap -B build/bootstrap` (pass), `cmake --build
  build/bootstrap --parallel 2` (pass), `cmake -S . -B build/normal` (pass),
  `cmake --build build/normal --parallel 2` (pass), `ctest --test-dir
  build/normal --output-on-failure` (pass; 145/145 tests, 1 skipped), bootstrap
  `generate --check` (pass; `No changes.`), and `git diff --check` (pass).
- Generated files: unchanged; bootstrap source drift and self-host generated
  output remain synchronized.
- Follow-ups: none.

### Review follow-up

- Added the scoped-entry index to interface-scope fallback diagnostic paths so
  manually constructed projects distinguish multiple invalid values.
- Verification: normal configure/build passed; the targeted CTest filter was
  not registered by this build, so the built test executable was used for the
  relevant regression coverage.
