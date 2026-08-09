# T013 — Integration and determinism test suite

## Purpose

Verify the complete generator against real CMake and cross-component safety
properties before it becomes its own build authority.

## Scope

- Create basic, internal-library, package, platform, and exclusion fixtures.
- Add full-pipeline golden tests and offline real-CMake configure/build tests.
- Test repeated generation, check mode, validation-before-write, ownership, and
  path normalization across component boundaries.
- Add infrastructure for cross-platform fixture equivalence without encoding
  host-specific expected output unnecessarily.

## Dependencies

T009, T010, T011, and T012.

## Acceptance criteria

- Every MVP example has executable acceptance coverage.
- Representative generated projects configure and build offline.
- A second generation changes no bytes.
- Check mode detects deliberate drift and performs no writes.
- Golden output is normally identical across supported platforms.

## Verification

```bash
ctest --test-dir <build-dir> --output-on-failure
```

- Run the integration suite twice from clean temporary output directories.
- Inspect fixtures to confirm they do not fetch network dependencies.

## Execution record

- 2026-08-09: Added basic, internal-library, package, platform, and exclusion
  fixtures with byte-for-byte golden generated output. Added full-pipeline
  tests for deterministic regeneration, check-mode no-write behavior,
  validation-before-write, manifest stability, and slash-normalized paths.
  The basic, internal-library, platform, and exclusion fixtures also configure
  and build with CMake in isolated temporary directories. The package fixture
  is golden-only because it intentionally emits `find_package(fmt)`; no
  fixture or integration test fetches dependencies from the network.
- Verification: `cmake -S bootstrap -B build/t013 -DCMAKEGEN_BOOTSTRAP_TESTS=ON`,
  `cmake --build build/t013 --parallel 4`, and
  `ctest --test-dir build/t013 --output-on-failure` (95/95 passed).
