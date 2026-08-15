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

T011, T019, and T020.

## Acceptance criteria

- A requested case-only rename results in the exact requested physical spelling
  on supported filesystems.
- The old spelling is not left as an owned stale path, and the manifest points
  only to the new spelling with the correct hash.
- If a later step fails and rollback succeeds, the operation restores both the
  physical path and manifest atomically from SafeWriter's perspective.
- If rollback or recovery also fails, follow T020's deterministic uncommitted
  recovery state: retain the journal/backups and pre/post metadata needed for
  recovery, and do not advance the manifest as if the rename were committed.
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
alias-aware atomic rename transaction with manifest migration. The operation
must use the finalized equivalence semantics from [T019](T019-unicode-path-equivalence.md)
and recovery semantics from [T020](T020-safe-writer-transaction-recovery.md).

## Execution record

- Date: 2026-08-12
- Summary: Added an internal SafeWriter rename transaction entry carrying the
  old physical source spelling and requested destination. Owned equivalent
  paths now use a direct rename on case-sensitive filesystems and a sibling
  temporary spelling when both aliases resolve to one case-insensitive entry.
  Rename backups, manifest migration, stale cleanup exclusion, reverse rename
  rollback, and retained recovery state are journaled with the existing T020
  transaction lifecycle.
- Tests: Added Foo.txt to foo.txt and reverse migration coverage with exact
  directory-entry spelling checks, unchanged-spelling no-op coverage, unowned
  alias conflict protection, rollback after physical rename, and injected
  rollback/recovery failure retry coverage. Existing T019 Unicode and T020
  transaction tests remain green.
- Verification: Normal and bootstrap builds passed. Focused case-only tests
  passed (3 cases, 1 platform-appropriate skip, 50 assertions); the isolated
  SafeWriter suite passed (20 cases, 4 platform-appropriate skips, 205
  assertions). Bootstrap CTest had no registered tests. Normal CTest passed
  157/158 tests with the unrelated existing CMake renderer dynamic-values
  test #14 failing; `git diff --check` passed.
- Generated files: unchanged; normal CTest's bootstrap source-drift check
  passed.
- Follow-ups: The unrelated CMake renderer test #14 remains outside T021 and
  was not modified.

### Review follow-up

- Case-only parent components are now migrated from deepest to outermost
  (`Foo/file.txt` → `foo/file.txt`) while the transaction-owned rename and
  content temporaries move with them. Destination-parent creation avoids
  creating a second portable-equivalent directory, and rollback reverses the
  parent spelling migration before restoring the old file and hash.
- Parent migration refuses to move directories containing entries outside the
  active transaction, preserving unowned files. Nested-parent coverage now
  verifies exact directory spelling and rollback after the physical rename.
- A shared case-only parent is migrated for multiple rename entries, while
  transaction-owned sibling outputs and staging files are retained. Recovery
  adopts a resolved rename temporary when the journal records the pre-move
  parent spelling.
- Verification: the nested-parent case and full isolated SafeWriter suite
  passed (26 runnable cases, 4 platform-appropriate skips, 269 assertions),
  and `git diff --check` passed. CI failures remain outside this task.
