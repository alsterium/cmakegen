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

### Review follow-up (UTF-8 storage and symmetric macOS comparison)

- SafeWriter now reconstructs normalized path strings through an explicit
  UTF-8 (`char8_t`) representation before storing them in `GeneratedFileSet`.
  This preserves the requested native Windows spelling independently of the
  active narrow code page; the Windows regression test asserts both stored path
  identity and manifest spelling.
- macOS comparisons now detect both physical parent capabilities and use a
  case-insensitive comparison when either side requires it. The result is
  symmetric even when equivalent parent spellings lead to mounted volumes with
  different case modes.

### Review follow-up (UTF-8 destination resolution)

- Manifest keys and generated relative paths now pass through the explicit
  UTF-8-to-native conversion before alias resolution, parent validation, and
  normalization. This covers write destinations, stale cleanup, and `clean`.
- The Windows Unicode path regression now verifies its requested spelling
  through write, stale removal, and clean.

### Review follow-up (native temporary path names)

- Transaction temporary basenames are now composed with native
  `std::filesystem::path` operations rather than a narrow filename string.
  This preserves non-ACP names such as emoji through atomic output staging.
- The Windows Unicode regression covers an emoji filename through write and
  clean in addition to UTF-8 manifest ownership paths.

### Review follow-up (nested capability validation)

- Windows directory cursors now reconstruct every UTF-8 component through the
  native UTF-8 path conversion, including nonexistent descendants. This keeps
  per-directory case-sensitivity discovery attached to the requested path
  rather than an ACP-mangled alias.
- macOS applies the portable ASCII/Win32 alias rule at every path component,
  including below Unicode-normalization-equivalent parents. Each non-ASCII
  destination and manifest entry also validates the last existing physical
  parent capability, so an unsupported nested mount fails safely even for a
  single output.
- Verification: normal and bootstrap builds passed; the isolated SafeWriter
  suite passed 14 cases with 2 platform-appropriate skips (89 assertions),
  bootstrap CTest completed with no registered tests, and `git diff --check`
  passed.

### Review follow-up (Windows target-parent validation)

- Windows now traverses each non-ASCII destination to its last existing parent
  before accepting the path. The per-directory `FileCaseSensitiveInfo` query
  therefore rejects an indeterminate nested mount before its first generated
  file is written.
- The Windows Unicode path regression covers a non-ASCII filename inside an
  existing nested output directory through write and clean.
- Verification: normal and bootstrap builds passed; the isolated SafeWriter
  suite passed 14 cases with 2 platform-appropriate skips (89 assertions),
  bootstrap CTest completed with no registered tests, and `git diff --check`
  passed.

### Review follow-up (mounted target capability)

- Target capability validation now separates UTF-8 decoding from the
  filesystem decision. A non-ASCII destination below an ASCII-named supported
  mount is accepted even when the project root itself has no supported Unicode
  capability; the mount's last existing parent remains authoritative.
- The macOS regression covers write and clean for a Unicode filename beneath
  an existing ASCII target directory.
- Verification: normal and bootstrap builds passed; the isolated SafeWriter
  suite passed 15 cases with 2 platform-appropriate skips (94 assertions),
  bootstrap CTest completed with no registered tests, and `git diff --check`
  passed.

### Review follow-up (distinct ASCII parents)

- macOS path comparison now returns non-equivalence immediately for distinct
  ASCII components after portable folding. It no longer probes an unrelated
  project-root filesystem capability when only a later Unicode component needs
  target-parent validation.
- The macOS regression combines a Unicode output below an existing target
  directory with an unrelated ASCII path and verifies both outputs are
  accepted.
- Verification: normal and bootstrap builds passed; the isolated SafeWriter
  suite passed 16 cases with 2 platform-appropriate skips (99 assertions),
  bootstrap CTest completed with no registered tests, and `git diff --check`
  passed.

### Review follow-up (Windows Unicode diagnostics)

- Failed Windows per-directory case-capability probes now retain the native
  directory spelling through the lossless UTF-8 diagnostic conversion, rather
  than routing it through the active narrow code page.
- The Windows Unicode regression exercises a non-ASCII filename below an
  existing emoji-named directory through write and clean, covering the native
  parent traversal used by capability probing.
- Verification: normal and bootstrap builds passed; the isolated SafeWriter
  suite passed 16 cases with 2 platform-appropriate skips (99 assertions),
  bootstrap CTest completed with no registered tests, and `git diff --check`
  passed.
