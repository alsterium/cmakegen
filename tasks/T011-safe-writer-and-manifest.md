# T011 — Safe writer and ownership manifest

## Purpose

Make generation and cleanup transactional enough to protect user-owned files
and support reproducibility checks.

## Scope

- Define `GeneratedFileSet` and `.cmakegen/manifest.json` ownership metadata.
- Implement stable content hashing, conflict/ownership checks, sibling temporary
  files, atomic replacement where supported, and manifest-last updates.
- Implement check, force, stale-file cleanup, clean, and dry-run operations as
  library behavior.
- Ensure destination paths cannot escape the intended project root accidentally.

## Dependencies

T003.

## Acceptance criteria

- Unowned files are never overwritten or deleted without the documented force
  behavior.
- Check and dry-run modes perform no writes.
- Failed validation/preflight produces no partial output.
- Stale cleanup and clean remove only manifest-owned files.
- Manifest output and hashes are deterministic.

## Verification

- Run isolated temporary-directory tests for conflicts, force, no-op writes,
  stale files, clean, failed replacement, and manifest ordering.
- Verify no test can delete outside its fixture root.

## Execution record

- 2026-08-09: Implemented the `SafeWriter` library with a deterministic,
  versioned JSON ownership manifest, SHA-256 content hashes, preflight
  ownership and path checks, temporary sibling writes, manifest-last updates,
  stale cleanup, clean, check, force, and dry-run behavior. Added isolated
  filesystem tests covering conflicts, no-op writes, dry runs, stale cleanup,
  clean, deterministic manifest ordering, and root-escape protection.
- Verification: `cmake -S bootstrap -B build/t011 -DCMAKEGEN_BOOTSTRAP_TESTS=ON`,
  `cmake --build build/t011 --parallel 4`, and
  `ctest --test-dir build/t011 --output-on-failure` (73/73 passed).
- Handoff (2026-08-09): Portable path equivalence currently folds ASCII and
  Win32 aliases but cannot safely emulate filesystem-specific Unicode case and
  normalization without a Unicode/filesystem capability layer. Add that layer
  (and macOS case-insensitive coverage) in a dedicated follow-up before
  claiming Unicode-equivalent output names are collision-safe.
- Handoff (2026-08-09): `SafeWriter` does not yet roll back already-replaced
  outputs if a later atomic replacement fails. Implementing this safely needs
  a transaction journal/backups with recovery semantics, including Windows
  locked-file behavior; track it as a dedicated writer transaction task.
- Handoff (2026-08-09): An owned case-only path rename (for example
  `Foo.txt` to `foo.txt`) currently retains the existing physical spelling on
  a case-sensitive filesystem. Support it with an alias-aware atomic rename
  transaction and manifest migration; do not treat a no-op ownership lookup as
  satisfying the requested output spelling.
