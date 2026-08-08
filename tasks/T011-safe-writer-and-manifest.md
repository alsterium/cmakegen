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
