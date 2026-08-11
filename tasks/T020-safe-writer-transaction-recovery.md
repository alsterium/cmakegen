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

- Fault injection at each replacement point leaves either the complete new
  output set or the complete previous output set; no partial set is reported
  as committed.
- A failed transaction restores the previous manifest and output contents, or
  leaves a deterministic recovery state with documented operator behavior.
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
