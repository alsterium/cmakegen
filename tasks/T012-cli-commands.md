# T012 — Complete MVP CLI

## Purpose

Connect the implemented pipeline to the documented command-line interface while
keeping process entry and command handlers thin.

## Scope

- Implement global options and `generate`, `validate`, `dump`, `graph`, and
  `clean`.
- Support generate check/force, validate resolved mode, dump formats, graph text
  and DOT output, external-node inclusion, and clean dry-run.
- Map diagnostics and failures to documented exit statuses.
- Implement quiet, verbose, and no-color behavior plus version output.

## Dependencies

T006 through T011, inclusive.

## Acceptance criteria

- Every MVP command and option in `docs/06_CLI_SPEC.md` is available.
- Validation and complete in-memory rendering occur before writes.
- Check mismatch exits 1; filesystem errors exit 2; internal generation errors
  exit 3.
- Default project root and explicit `--project`/`--root` behavior are correct.
- `main.cpp` only delegates to the CLI application boundary.

## Verification

- Run command-level tests for success, failure, output text, exit code, and
  no-write guarantees.
- Exercise help/version and every option combination that changes safety
  behavior.

## Execution record

- 2026-08-09: Added a testable CLI application boundary and made `main.cpp`
  delegate to it. Implemented global project/root/output options and the
  `generate`, `validate`, `dump`, `graph`, and `clean` commands, including
  rendering-before-writing, check/force, resolved validation, graph formats,
  external dependency inclusion, and clean dry-run behavior.
- Verification: `cmake -S bootstrap -B build/t012 -DCMAKEGEN_BOOTSTRAP_TESTS=ON`,
  `cmake --build build/t012 --parallel 4`, and
  `ctest --test-dir build/t012 --output-on-failure` (90/90 passed).
