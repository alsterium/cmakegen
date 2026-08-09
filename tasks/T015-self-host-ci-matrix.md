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

## Execution record

- 2026-08-09: Added `.github/workflows/self-host.yml`. The explicit matrix
  runs the complete A–E self-host invariant on GCC/Linux, AppleClang/macOS,
  and MSVC/Windows, with a second Clang/Linux job for compiler diversity.
  The normal configuration receives the located stage-0 executable, so its
  CTest run covers both bootstrap source drift and stage-binary equivalence.
  The FetchContent cache is optional and keyed by OS, compiler, and
  `cmake/dependencies.cmake`; every configure command works on a cold cache.
- Verification: local workflow YAML structural parse passed. The T014 stage-0
  and stage-1 binaries both reported `No changes.` against this exact source
  tree; the source-drift and direct stage-binary-equivalence CMake scripts also
  passed. Hosted matrix-run URLs are pending the PR workflow run. A deliberately
  stale generated-file fixture is rejected by `generate --check`; both CI
  checks then use `git diff --exit-code` to verify that they did not modify the
  checkout.
