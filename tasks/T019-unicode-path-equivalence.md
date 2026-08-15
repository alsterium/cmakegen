# T019 — Unicode-aware path equivalence and collision safety

## Purpose

Make SafeWriter path equivalence and collision checks preserve the portable
ASCII/Win32 collision invariant while handling filesystem-specific Unicode case
and normalization behavior safely.

## Scope

- Preserve the portable ASCII case-folding and Win32 alias rules on every host,
  and define a filesystem-capability boundary only for additional Unicode case
  and normalization behavior.
- Apply the capability-aware equivalence rules to SafeWriter preflight,
  ownership lookup, manifest comparison, stale cleanup, and output collision
  checks.
- Cover filesystem-specific Unicode behavior where it can be detected safely
  without weakening the portable ASCII/Win32 invariant.
- Add coverage for macOS case-insensitive filesystems and document behavior
  when a capability cannot be detected or supported.
- Keep manifest ordering and generated output deterministic.

## Dependencies

T011.

## Acceptance criteria

- Equivalent path spellings cannot bypass ownership, conflict, or collision
  checks on supported target filesystems.
- ASCII case aliases remain equivalent for portable ownership and collision
  checks on every host, including a case-sensitive host; generated output cannot
  contain both portable spellings.
- Non-ASCII composed/decomposed and case-equivalent names follow the documented
  filesystem capability rather than locale-dependent guesses.
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
filesystem-specific Unicode case and normalization. The invariant and its
Unicode-only extension are defined in
[ADR-012](../docs/11_DECISIONS_AND_TRADEOFFS.md#adr-012--preserve-portable-output-identity-across-filesystems).

## Execution record

- Date: 2026-08-12
- Summary: Added an explicit ASCII/Win32 portable equivalence layer and a
  capability-aware Unicode boundary for SafeWriter. macOS APFS/HFS capability
  detection uses `getattrlist` and Core Foundation normalization/case folding;
  Windows uses volume/per-directory case capability queries and ordinal Unicode
  comparison. Unsupported or indeterminate Unicode capability fails preflight.
  The same relation now covers preflight, ownership lookup, manifest migration
  and comparison, stale cleanup, ancestor checks, and output collisions.
- Tests: Added isolated SafeWriter coverage for portable aliases, Win32
  trailing-dot/space ownership, case-sensitive-host aliases, macOS Unicode
  normalization/case behavior, deterministic distinct Unicode output, and
  unsupported capability failure.
- Verification: Normal and bootstrap builds passed. The isolated SafeWriter
  suite passed all 13 runnable cases with 2 platform-appropriate skips; full
  normal CTest passed 148/149 tests, with the unrelated pre-existing CMake
  renderer dynamic-values test failing. Bootstrap CTest completed successfully
  with no registered tests, and `git diff --check` passed.
- Follow-ups: T020 remains responsible for transaction rollback/recovery, and
  T021 remains responsible for case-only rename/manifest migration semantics.

### Review follow-up

- Windows Unicode comparisons now query the case-sensitivity capability from
  each existing parent directory while resolving a relative path. Existing
  alias, ancestor, and symlink-parent scans use the physical directory they
  are enumerating rather than reusing the project-root setting.
- Verification: normal build passed, the isolated SafeWriter suite passed 13
  runnable cases with 2 platform-appropriate skips, and `git diff --check`
  passed. The initial in-sandbox dependency download failed because DNS was
  unavailable; the same build completed with the approved network retry.

### Review follow-up (Unicode capability boundaries)

- Native Windows filesystem paths are now converted with `generic_u8string()`
  before SafeWriter validates them through the UTF-8/UTF-16 Unicode layer; the
  Windows test covers a non-ASCII native path through preflight and manifest
  rendering.
- macOS Unicode comparison now discovers APFS/HFS case capability at each
  existing physical parent and carries the last existing parent capability for
  new descendants. This prevents a nested mounted volume from inheriting the
  project-root comparison mode.
- ADR-012 now records both finalized constraints.
- Verification: normal and bootstrap builds passed; the isolated SafeWriter
  suite passed 13 cases with 2 platform-appropriate skips; `git diff --check`
  passed.
