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
