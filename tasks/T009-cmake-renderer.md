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

## Execution record

- Date: 2026-08-09
- Summary: Added a pure deterministic CMake renderer that consumes the
  normalized project, T008 validation graph, and explicitly resolved common
  and platform source lists. It renders the root `CMakeLists.txt` and
  `cmake/cmakegen.generated.cmake`, including metadata, packages, custom
  includes, all target types, scoped properties, target language features,
  and native macOS/Linux/Windows conditions.
- Verification: `cmake -S bootstrap -B build/t009c
  -DCMAKEGEN_BOOTSTRAP_TESTS=ON ... && cmake --build build/t009c -j2 &&
  ctest --test-dir build/t009c --output-on-failure && git diff --check`
  (pass; 76/76 tests). The renderer tests compare full byte-for-byte root and
  combined-feature output, assert repeat-render equality and final newlines,
  and configure a representative generated project without packages or
  network access.
- Generated files: unchanged; normal generated CMake is introduced when T014
  establishes the self-hosted project definition.
- Follow-ups: T012 must resolve common and platform source entries, call
  `validate_project`, then pass its `TargetGraph` and resolved source map to
  `render_cmake` before T011 writes the returned files.
