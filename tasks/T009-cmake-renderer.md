# T009 — Deterministic CMake renderer

## Purpose

Render readable, byte-stable root and target CMake files from the resolved and
validated domain model.

## Scope

- Render `CMakeLists.txt` and `cmake/cmakegen.generated.cmake` fully in memory.
- Cover project metadata, packages, custom includes, all target types, explicit
  sources, scoped properties, dependencies, language features, and platform
  conditions.
- Apply the documented formatting and ownership headers.
- Keep rendering pure: no source scanning, model mutation, CMake invocation, or
  writes.

## Dependencies

T006, T007, and T008.

## Acceptance criteria

- Output matches `docs/05_CMAKE_GENERATION.md` for every MVP feature.
- Empty commands/scopes are omitted and ordering is stable.
- C and C++ standard features use the documented target scopes.
- Output has no timestamps, trailing spaces, or missing final newline.
- Two renders of identical input are byte-identical.

## Verification

- Run byte-for-byte golden tests for all examples and combined-feature
  fixtures.
- Configure representative rendered projects with CMake without network access.
