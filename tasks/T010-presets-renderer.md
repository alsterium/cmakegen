# T010 — CMake Presets renderer

## Purpose

Generate deterministic, minimum-CMake-compatible configure, build, and test
presets.

## Scope

- Implement the compatibility mapping decided in T001.
- Normalize omitted presets to documented debug/release defaults.
- Render generator, binary directory, build type, and cache variables.
- Produce deterministic JSON with stable key and preset ordering.

## Dependencies

T001 and T006.

## Acceptance criteria

- `CMakePresets.json` uses a schema version supported by the selected minimum
  CMake version.
- Default and explicit presets match the specification.
- Binary directories default to `${sourceDir}/build/<preset-name>`.
- Repeated rendering is byte-identical.

## Verification

- Run Presets golden tests.
- Validate generated fixtures with the selected minimum supported CMake and the
  development CMake version when available.

## Execution record

- Date: 2026-08-09
- Summary: Added a schema-8 compatibility mapping for the documented CMake
  3.28 baseline and a deterministic `CMakePresets.json` renderer. Configure,
  build, and test presets are emitted in stable name order; default binary
  directories use `${sourceDir}/build/<preset-name>` and explicit cache
  variables retain a stable ordering. Added golden and compatibility unit tests
  plus a CMake fixture.
- Verification: `cmake -S bootstrap -B build -DCMAKE_BUILD_TYPE=Debug
  -DCMAKEGEN_BOOTSTRAP_TESTS=ON && cmake --build build --parallel && ctest
  --test-dir build --output-on-failure` (pass, 71 tests); `cmake
  --list-presets` in `tests/fixtures/presets/default` with CMake 4.4.0 (pass).
  CMake 3.28 is not installed in this environment, so direct minimum-version
  execution was not available; its schema-8 compatibility is the T001
  documented baseline.
- Generated files: fixture `CMakePresets.json` added and validated; no
  repository-generated project files exist before T014.
- Follow-ups: T012 wires the renderer into the CLI; T013 covers end-to-end
  generated output.
