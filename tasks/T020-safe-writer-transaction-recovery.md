# T020 — SafeWriter transaction rollback and recovery

## Purpose

Make multi-file SafeWriter replacement recoverable when an atomic replacement
fails after earlier outputs have already been replaced.

## Scope

- Define the transaction lifecycle for output replacements, including journal
  or backup creation, commit, rollback, cleanup, and recovery after an
  interrupted transaction.
- Restore already-replaced outputs when a later replacement fails, without
  touching unowned files.
- Define manifest interaction so ownership metadata cannot describe a partial
  or rolled-back output set.
- Handle platform-specific replacement failures, including Windows locked-file
  behavior, with actionable diagnostics and safe retained artifacts where
  recovery requires them.
- Preserve check, dry-run, force, stale-cleanup, and clean safety guarantees.

## Dependencies

T011.

## Acceptance criteria

- For an injected replacement failure where rollback succeeds, the complete
  previous output set and previous manifest are restored; a partial set is
  never reported as committed. A successful transaction may instead expose
  the complete new output set with its new manifest.
- If rollback or recovery itself fails (for example because a file is locked),
  the writer leaves an explicitly uncommitted, deterministic recovery state:
  the journal/backups and pre/post metadata needed for recovery are retained,
  the manifest is not advanced as if the new set were committed, and the
  operator recovery procedure is documented.
- A successful transaction removes temporary journals and backups and writes a
  deterministic manifest last.
- Locked-file and replacement failures produce structured diagnostics and do
  not delete or overwrite unowned files.
- Check and dry-run modes remain write-free, including journal and backup
  creation.

## Verification

- Add isolated temporary-directory tests with injected failures before, during,
  and after replacement, including manifest update failures.
- Test recovery and locked-file behavior on each supported platform where the
  behavior differs, especially Windows.
- Run the SafeWriter test suite, bootstrap build, and
  `ctest --test-dir build/bootstrap --output-on-failure`.
- Run `git diff --check` and verify that generated manifests remain
  deterministic.

## Source handoff

Recorded in [T011](T011-safe-writer-and-manifest.md): SafeWriter currently does
not roll back already-replaced outputs and needs transaction journal/backups
with recovery semantics.

## Operator recovery

When a write reports that rollback or recovery is required, stop concurrent
writers and preserve `.cmakegen/transaction/` as a unit. The directory contains
`journal.json`, `pre-manifest.json`, `post-manifest.json`, and numbered files in
`backups/`; do not delete or rename those artifacts manually. Resolve the
reported filesystem problem first, especially a locked or read-only output,
then rerun the same generation or `clean` command. SafeWriter validates the
journal, restores the pre-state when the post-state is incomplete, or finalizes
the post-state only when every output and the manifest match their recorded
hashes. The manifest is not advanced during an unresolved recovery state.

If the retry still reports recovery-required, keep the artifacts and have an
operator inspect the journal entry statuses and compare the pre/post metadata
with the output tree. Restore only the numbered backups to their recorded
destinations after the outputs are quiescent, then rerun the command; never
remove an unowned file to force recovery. A successful retry removes the
transaction directory automatically.

An interrupted setup or cleanup may leave a transaction directory without
`journal.json`. SafeWriter automatically removes that directory only when it
contains the known metadata, temporary, and numbered-backup scaffolding; an
unexpected artifact still requires operator inspection. Sibling staging paths
are recorded as transaction-owned before writing, so a partial staging file is
removed during automatic rollback even when its hash does not match the
intended post-state.

## Execution record

- Date: 2026-08-12
- Summary: Added deterministic journal/pre-post metadata and numbered backups
  for multi-file replacement and clean; manifest publication remains last;
  reverse-order rollback verifies the old output/manifest state; unresolved
  rollback or recovery retains an explicit `recovery_required` state. Added a
  small internal failure-injection seam and isolated tests for before/during/
  after replacement, manifest update, rollback/recovery failure, and the
  Windows locked-file behavior (skipped on POSIX where open files can be
  renamed), and a file-to-directory rollback transition. Check/dry-run paths
  do not create transaction artifacts.
- Verification: `cmake --build build/t020-bootstrap --parallel 4` (pass);
  `cmake --build build/t020-normal --target cmakegen_tests --parallel 4`
  (pass); `build/t020-normal/cmakegen_tests "safe writer*" --reporter compact`
  (12 passed, 2 platform-dependent skips, 128 assertions passed); `ctest
  --test-dir build/t020-normal --output-on-failure` (SafeWriter tests pass;
  144 passed, 2 skipped, and unrelated CMake renderer test #14 failed in the
  local run; not investigated per task instruction); `git diff --check`
  (pass).
- Generated files: unchanged; the existing source list already includes the
  modified SafeWriter files.
- Follow-ups: none for T020; the full-suite CMake renderer failure #14 is
  outside this task and remains a local verification residual.

### Review follow-up

- A destination whose current hash still matches `pre_sha256` is retained
  when a backup move fails before it can be applied. Journal temporary files
  are discarded before retrying an atomic journal write. Staging ownership is
  journaled before each write, allowing partial transaction temporaries to be
  removed safely during rollback. A pre-existing sibling staging path is
  recorded as unclaimed and preserved. Journal-less setup/cleanup scaffolding
  is fully validated before any known artifact is removed, and is cleaned only
  when its contents are known and transaction-owned. Output directories whose
  names merely begin with `.cmakegen` remain valid; only the reserved metadata
  component is rejected.
- Cleanup removes the terminal journal before pre/post metadata, recreated
  file-to-directory parents are removed before restoring their file backups,
  transaction-directory creation is exclusive, the transaction lifetime lock
  prevents a competing writer from recovering active artifacts, and recovery
  revalidates destination/staging parents and retained backup hashes before
  mutation. Manifest-pending recovery verifies every post-manifest output
  including unchanged entries. Preflight rejects generated destinations that
  collide with another output's deterministic staging path, and rollback
  accepts an untouched remove destination when its pre-state hash still
  matches. Cleanup also revalidates the backup directory chain, and
  post-state verification rejects symlinked output parents before reading
  manifest-owned files. Staging collision preflight also rejects generated
  paths below or above another output's deterministic staging path.
- Staging collision preflight now compares generated staging paths with the
  complete post-write manifest, including outputs retained when stale cleanup
  is disabled.
- Pre-state verification revalidates every output parent before accepting a
  partially restored transaction, including entries already marked restored.
- Directory creation keeps planned paths out of the journal. Each path becomes
  transaction-owned and is persisted only after its `create_directory` call
  succeeds, so recovery cannot remove a directory another process creates in
  the pre-creation window. Existing outputs are revalidated as regular files
  with their recorded pre-hash immediately before they are moved to backups.
- Rollback now verifies every file recorded by the pre-manifest before it
  removes recovery artifacts. A successful replacement clears its staging
  ownership before terminal cleanup, and rollback rejects an empty directory
  recreated at a removed destination unless that directory is journaled as a
  transaction-created file-to-directory transition.
- Initial journal publication now uses the same temporary-and-atomic-rename
  protocol as later updates. Directory creation records an explicit
  `creating_directory` state before the system call, allowing recovery to
  finish an interrupted file-to-directory transition without treating it as
  an unclaimed external directory.
- Tests: added partial-staging, stale-journal-temporary, and journal-less
  recovery coverage, including preservation of unclaimed staging paths and
  journal-less backup validation. Added lifetime-lock contention and retained
  backup corruption coverage, plus symlinked backup-parent cleanup and
  symlinked post-state output-parent recovery coverage and staging-path
  ancestor collision coverage. Backup parents are also revalidated before
  restoration. The existing Windows locked-file test now exercises the
  pre-hash-preservation path. Added coverage that an interrupted
  file-to-directory transition journals the directory only after creation,
  retained pre-manifest output validation, recreated post-publication staging
  paths, unclaimed directories at removed destinations, and recovery from a
  journaled in-progress directory creation.
- Verification: `cmake --build build/t020-normal --parallel 2` and
  `cmake --build build/t020-bootstrap --parallel 2` passed;
  `build/t020-normal/cmakegen_tests "safe writer*"` passed with 36 runnable
  cases, 3 platform-appropriate skips, and 312 assertions; `git diff --check`
  passed. CI failures remain outside this task.
- Rebased on the finalized T019 Unicode-path capability branch. Case-only or
  Unicode-alias replacement remains safely rejected here; the explicit rename
  transaction belongs to T021. Before recording `committed`, SafeWriter now
  verifies the complete post-manifest tree. For an interrupted directory
  creation, an empty directory indistinguishable from one created by another
  process is never deleted during recovery; the transaction is retained with
  recovery-required diagnostics. Verification: normal and bootstrap builds
  passed; `cmakegen_tests "safe writer*" --reporter compact` passed with 37
  cases, 3 platform skips, and 316 assertions; `git diff --check` passed.
- Recovery preserves a backup recreated after its recorded restore, and a
  staging file recreated after an interrupted publish whose journal still
  claims it. Both cases retain the transaction with recovery-required
  diagnostics instead of deleting an indistinguishable external artifact.
- Terminal cleanup also verifies every remaining transaction backup against
  its recorded pre-hash, preserving a backup changed after `committed` rather
  than deleting an external replacement.
- Before every backup rename, SafeWriter revalidates the backup parent chain
  and requires the numbered backup destination to be absent. Planning also
  validates retained manifest outputs when stale cleanup is disabled, avoiding
  a transaction that cannot later satisfy either pre- or post-state checks.
- Rollback verifies planned entries against their captured pre-transaction
  state, so forced replacement of a modified owned file restores correctly.
  A journal-less transaction directory never deletes numbered backups: without
  a journal they cannot be authenticated as transaction-owned.
