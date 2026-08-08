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
