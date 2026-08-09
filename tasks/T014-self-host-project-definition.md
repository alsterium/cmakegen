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

## Execution record

- 2026-08-09: Added the schema-v1 root `project.yml` for the core library,
  CLI, and Catch2 test executable. Generated and committed the root CMake
  files and ownership manifest from the bootstrap binary. Kept CTest/Catch2
  discovery, bootstrap source-drift, and stage-binary equivalence checks in
  the documented hand-maintained `cmake/custom.cmake` escape hatch.
  Bootstrap now builds only the functional generator and skips the
  normal-build-only Catch2 acquisition.
- Verification: Stage A `cmake -S bootstrap -B /private/tmp/cmakegen-T014-build-bootstrap -G 'Unix Makefiles'` and `cmake --build /private/tmp/cmakegen-T014-build-bootstrap --parallel 4`; stage B bootstrap `cmakegen generate --check --no-color`; stage C `cmake --preset debug` and `cmake --build --preset debug --parallel 4`; stage D `build/debug/cmakegen generate --check --no-color`; `ctest --test-dir build/debug --output-on-failure` (97/97 passed, including bootstrap drift and equivalence); direct stage-binary equivalence script passed.
