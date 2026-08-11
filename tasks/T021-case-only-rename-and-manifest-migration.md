# T021 — Case-only rename and manifest migration

## Purpose

Ensure an owned output whose requested path differs only by case is physically
renamed to the requested spelling and remains consistent with the ownership
manifest.

## Scope

- Detect an owned case-only path change without treating an ownership lookup
  no-op as satisfying the requested spelling.
- Perform an alias-aware atomic rename that produces the requested physical
  path spelling on case-sensitive filesystems and behaves correctly on
  case-insensitive filesystems.
- Migrate manifest ownership and hashes together with the rename.
- Reuse SafeWriter transaction and rollback semantics for conflicts, failures,
  and recovery.
- Cover stale-path cleanup and unowned-file conflict behavior without
  broadening force semantics.

## Dependencies

T011 and T020.

## Acceptance criteria

- A requested case-only rename results in the exact requested physical spelling
  on supported filesystems.
- The old spelling is not left as an owned stale path, and the manifest points
  only to the new spelling with the correct hash.
- The operation is atomic from SafeWriter's perspective and rollback restores
  both the physical path and manifest when a later step fails.
- Unowned conflicting paths are protected according to existing force rules.
- Tests cover case-sensitive and case-insensitive filesystems, unchanged
  spelling, conflict, stale cleanup, and rollback behavior.

## Verification

- Add isolated filesystem tests for `Foo.txt` → `foo.txt` and the reverse,
  including manifest migration and injected replacement failures.
- Run the relevant platform test suites and the SafeWriter/bootstrap tests.
- Run `git diff --check` and verify deterministic manifest output.

## Source handoff

Recorded in [T011](T011-safe-writer-and-manifest.md): an owned case-only path
rename currently retains the existing physical spelling and needs an
alias-aware atomic rename transaction with manifest migration.
