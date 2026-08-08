# T005 — Extended YAML schema parser

## Purpose

Complete schema-version-1 parsing beyond the minimal executable case.

## Scope

- Parse scoped values in shorthand and scoped forms.
- Parse packages, components, platform overrides, presets, and custom CMake
  includes.
- Reject unknown keys at every supported mapping level.
- Add positive and negative fixtures covering the complete specification.

## Dependencies

T004.

## Acceptance criteria

- The complete example in `docs/04_PROJECT_SPEC.md` parses.
- All scoped properties accept both documented forms.
- Only `macos`, `linux`, and `windows` platform names are accepted.
- Package and preset fields preserve enough information for normalization.
- Unknown nested keys report their full logical YAML path.

## Verification

- Run the complete parser fixture suite.
- Map every schema-v1 field in `docs/04_PROJECT_SPEC.md` to at least one test.

## Execution record

- Date: 2026-08-08
- Summary: Extended the YAML-independent raw schema with scoped target values,
  platform overrides, packages, CMake include files, and presets (including
  cache variables). The parser now accepts shorthand and all documented scoped
  forms, preserves source locations and logical paths, and rejects unknown keys
  at every newly supported mapping level. Added parser coverage for the full
  schema-v1 example, all scopes and platform overrides, malformed mappings, and
  nested unknown keys.
- Verification: `cmake --build build/bootstrap --parallel 2 && ctest --test-dir
  build/bootstrap --output-on-failure && cmake -S bootstrap -B
  build/bootstrap-gcc -DCMAKE_CXX_COMPILER=g++ && cmake --build
  build/bootstrap-gcc --parallel 2 && ctest --test-dir build/bootstrap-gcc
  --output-on-failure && git diff --check` (pass; 28/28 tests in each
  configuration). Both `clang++` and `g++` resolve to Apple Clang 21.0.0
  locally.
- Generated files: unchanged; normal generated CMake does not exist until T014.
- Follow-ups: T006 consumes the complete raw schema for normalization and
  deterministic dumping.

## Execution record — review follow-up

- Date: 2026-08-09
- Summary: Corrected the default package requirement to `true`, preserving an
  explicit `required: false`. This matches the documented Qt package example,
  which omits `required` yet generates a required `find_package` call.
- Verification: `cmake --build build/bootstrap --parallel 2 && ctest --test-dir
  build/bootstrap --output-on-failure && cmake -S bootstrap -B
  build/bootstrap-gcc -DCMAKE_CXX_COMPILER=g++ && cmake --build
  build/bootstrap-gcc --parallel 2 && ctest --test-dir build/bootstrap-gcc
  --output-on-failure && git diff --check` (pass; 29/29 tests in each
  configuration). Both `clang++` and `g++` resolve to Apple Clang 21.0.0
  locally.
- Generated files: unchanged.
- Follow-ups: none.
