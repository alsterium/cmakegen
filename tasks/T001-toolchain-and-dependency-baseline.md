# T001 — Toolchain and dependency baseline

## Purpose

Resolve the changeable compatibility and dependency decisions needed before the
C++ foundation is implemented.

## Scope

- Verify current CMake Presets schema compatibility for the selected minimum
  CMake release.
- Select supported Clang, GCC, and MSVC baselines based on required C++23
  features, especially `std::expected`.
- Select YAML, JSON, CLI, and test libraries and their bootstrap acquisition
  strategy.
- Decide whether hashing is embedded or provided by a dependency.
- Record decisions in existing architecture/decision documentation and capture
  sourced research in `tasks/research/T001.md`.

## Dependencies

None.

## Acceptance criteria

- Exact minimum toolchain and CMake versions are documented.
- Dependency names, CMake targets, versions/ranges, and acquisition strategy are
  documented for bootstrap and normal builds.
- The Presets file-version mapping needed by MVP is documented.
- Any specification ambiguity discovered is resolved in the relevant `docs/`
  file rather than left implicit in implementation.

## Verification

- Review every selected version and target name against primary upstream
  documentation.
- Check that decisions satisfy `docs/01_PRD.md`, `docs/02_ARCHITECTURE.md`, and
  `docs/13_SELF_HOSTING.md`.

## Execution record

- Date: 2026-08-08
- Summary: Selected CMake 3.28/Preset schema 8; GCC 12.1, Clang/libc++ 16, and
  Visual Studio 2022 17.6 baselines; CLI11 2.6.2, yaml-cpp 0.8.0,
  nlohmann_json 3.12.0, and Catch2 3.15.3. Defined a user-owned dependency
  hook that prefers installed CMake packages and otherwise uses hash-pinned
  FetchContent archives. Chose an in-tree SHA-256 implementation for T011.
- Verification: primary upstream documentation review and `git diff --check`
  (pass); selected versions and targets cross-checked against PRD and
  self-hosting specification (pass)
- Generated files: unchanged; no generated CMake exists before T014
- Follow-ups: T002 implements `cmake/dependencies.cmake` and bootstrap checks;
  T011 selects and licenses the embedded SHA-256 source

## Execution record — review follow-up

- Date: 2026-08-08
- Summary: Distinguished the MVP bootstrap dependency fallback from deferred
  schema-level FetchContent declarations, and made the T002 fallback
  implementation and acceptance criterion explicit.
- Verification: `git diff --check` (pass)
- Generated files: unchanged
- Follow-ups: T002 implements and verifies the documented fallback

## Execution record — review follow-up 2

- Date: 2026-08-08
- Summary: Corrected the transitional bootstrap policy: T002 through T013
  acquire Catch2 when bootstrap test wiring is enabled; T014 removes that
  acquisition when it minimizes bootstrap to the functional generator.
- Verification: `git diff --check` (pass)
- Generated files: unchanged
- Follow-ups: T002 conditionally acquires Catch2 and T014 removes it
