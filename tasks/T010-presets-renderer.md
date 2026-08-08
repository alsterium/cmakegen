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
