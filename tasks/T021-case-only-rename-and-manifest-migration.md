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
- Descendants are inspected recursively before moving an owned ancestor, and
  mixed rename/replace transactions suppress replacement-parent creation and
  stage new siblings below the existing physical parent until migration.
- Rename temporary ownership is journaled before the first temporary move;
  cleanup and rollback leave an unclaimed pre-existing sibling untouched even
  when its hash matches the rename source.
- Forced replacements are retargeted when a case-only parent is migrated, so
  the replacement staging path remains below the physical parent after the
  rename and cannot be mistaken for an unowned destination.
- Rollback keeps logical source and destination spellings stable while
  retargeting transaction artifacts. If a shared parent was restored before a
  later filename rollback, an equivalent physical source spelling such as
  `Foo/a.txt` is accepted and moved back to the recorded `Foo/A.txt` source.
- Generated output paths that equal deterministic case-only rename temporaries
  are rejected during preflight, and the legacy manifest-alias test now checks
  the requested physical spelling rather than the previous alias.
- Parent subtree conflicts are validated while preparing the plan, so check
  and dry-run report the same unowned-descendant error without creating a
  transaction; the execution-time validation remains in place for races.
- Later entries in a multi-output nested parent migration derive their source
  components from the currently resolved physical tree, so an earlier
  ancestor move cannot leave a later `Foo/...` lookup stale.
- When stale cleanup is disabled, retained manifest-owned siblings are added
  to the parent migration and their manifest paths are updated with the
  directory spelling change, preserving ownership of omitted outputs.
- Intermediate ancestors on the path to a transaction artifact are treated
  as owned directory structure and are still recursively inspected for
  unowned descendants.
- Retained outputs are matched against every changed parent prefix, so a
  retained file in a nested sibling branch follows an outer directory
  spelling migration even when the requested output is deeper in another
  branch.
- Forced parent migrations permit modified retained outputs and record the
  retained content's current hash while preserving its content.
- Rename temporary collision checks now run only for planned case-only renames
  and compare against the complete post-write manifest, including retained
  outputs.
- Parent migration planning rejects incompatible requested spellings for the
  same portable-equivalent ancestor before any transaction is created.
- Occupied case-only rename temporaries are rejected during plan preparation,
  so check and dry-run use the same target validation as a real write.
- New sibling branches below a parent spelling migration are created under the
  currently existing source parent and retargeted with the directory move, so
  nested paths such as `foo/b/y.txt` can accompany `Foo/A/x.txt` →
  `foo/a/x.txt` without creating a second parent alias.
- Unicode normalization-only aliases preserve the established manifest and
  physical spelling.  They are no longer treated as case-only physical
  renames, making an identical re-generation a no-op on APFS/HFS.
- Every planned content staging artifact is now checked against the complete
  post-write manifest.  This rejects retained outputs occupying a case-only
  rename's `.cmakegen-transaction.tmp` path during check and dry-run, before
  a transaction is created.
- Verification: the forced-replacement, shared-parent rollback, and
  parent-migrated source-spelling, rename-temporary collision, and physical
  spelling, preflight-conflict, resolved-tree migration, retained-sibling,
  nested-retained-migration, forced-retained-migration,
  planned-rename-collision, shared-parent-conflict,
  occupied-rename-temporary, and deep-intermediate-directory cases passed; the
  full isolated SafeWriter suite passed (49 runnable cases, 4
  platform-appropriate skips, 487 assertions), and `git diff --check`
  passed. CI failures remain outside this
  task.
