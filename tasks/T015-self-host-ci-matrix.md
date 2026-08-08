# T015 — Cross-platform self-host CI matrix

## Purpose

Continuously enforce the self-host invariant on Linux, macOS, and Windows.

## Scope

- Add CI jobs for bootstrap configure/build, generated-file check, normal
  configure/build, tests, and normal-generator check.
- Handle multi-config executable paths portably.
- Add bootstrap/normal byte-equivalence and bootstrap source-drift coverage.
- Add at least practical compiler diversity without weakening the required
  platform matrix.

## Dependencies

T014.

## Acceptance criteria

- Linux, macOS, and Windows run the complete invariant described in
  `docs/13_SELF_HOSTING.md`.
- CI fails on stale generated files without modifying the checkout.
- Logs make the failed invariant stage clear.
- Dependency caching does not become required for correctness.

## Verification

- Validate workflow syntax locally where tooling exists.
- Record links/results for successful matrix runs in the execution record.
- Deliberately exercise the stale-generation failure in a branch or fixture.
