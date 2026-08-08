# T014 — Self-hosted project definition and generated files

## Purpose

Make `project.yml` the canonical normal-build definition and prove the local
bootstrap-to-normal-build loop.

## Scope

- Add the root schema-v1 `project.yml` describing core, CLI, and test targets as
  far as the MVP schema supports.
- Generate and commit root `CMakeLists.txt`, `CMakePresets.json`, target CMake,
  and manifest together.
- Update the manual bootstrap source list and dependency acquisition only as
  needed for a functional generator.
- Remove the transitional bootstrap test wiring introduced by T002 so the final
  bootstrap contains only the functional generator.
- Add bootstrap drift and bootstrap/normal output-equivalence checks.
- Document any standard custom-CMake escape hatch required; do not add
  project-name special cases.

## Dependencies

T012 and T013.

## Acceptance criteria

- A fresh local checkout can build the bootstrap generator.
- The bootstrap generator reproduces checked-in generated files.
- The generated normal project builds cmakegen and its tests.
- The normal binary reports `generate --check` with no changes.
- Bootstrap and normal generators produce byte-identical canonical output.

## Verification

Follow stages A–D in `docs/09_TEST_STRATEGY.md`, run all tests, and inspect the
final Git diff for generated-file consistency.
