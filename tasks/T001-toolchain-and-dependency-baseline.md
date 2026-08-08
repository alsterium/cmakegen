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
