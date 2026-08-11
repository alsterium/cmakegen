# T019 — Unicode-aware path equivalence and collision safety

## Purpose

Make SafeWriter path equivalence and collision checks reflect the target
filesystem's case and Unicode behavior before claiming that equivalent output
names are collision-safe.

## Scope

- Define a filesystem-capability boundary for case sensitivity, Unicode
  normalization, and platform aliases instead of relying on a universal string
  folding rule.
- Apply the capability-aware equivalence rules to SafeWriter preflight,
  ownership lookup, manifest comparison, stale cleanup, and output collision
  checks.
- Preserve the existing handling of ASCII and Win32 aliases while covering
  filesystem-specific Unicode behavior where it can be detected safely.
- Add coverage for macOS case-insensitive filesystems and document behavior
  when a capability cannot be detected or supported.
- Keep manifest ordering and generated output deterministic.

## Dependencies

T011.

## Acceptance criteria

- Equivalent path spellings cannot bypass ownership, conflict, or collision
  checks on supported target filesystems.
- Distinct names on a case-sensitive filesystem remain distinct.
- Unicode composed/decomposed and case-equivalent names follow the documented
  capability of the target filesystem rather than locale-dependent guesses.
- macOS case-insensitive behavior and existing Windows alias behavior have
  regression coverage.
- Unsupported or indeterminate filesystem capabilities fail safely and are
  documented; they do not silently claim collision safety.

## Verification

- Before implementation, research the supported filesystem APIs and record
  changeable platform findings in `tasks/research/T019.md` if required.
- Add isolated SafeWriter tests for equivalent and non-equivalent Unicode,
  case, and platform-alias paths.
- Run the relevant platform test suites, the bootstrap build, and
  `ctest --test-dir build/bootstrap --output-on-failure`.
- Run `git diff --check` and verify deterministic manifest output.

## Source handoff

Recorded in [T011](T011-safe-writer-and-manifest.md): the current portable
comparison handles ASCII and Win32 aliases but cannot safely emulate
filesystem-specific Unicode case and normalization.
