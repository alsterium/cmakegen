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
