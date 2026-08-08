# Architecture Decisions and Tradeoffs

## ADR-001 — Production implementation is C++23

Decision:

cmakegen is implemented entirely in modern C++.

Why:

- dogfoods the ecosystem it manages
- avoids requiring another language toolchain
- makes self-hosting conceptually clean
- keeps distribution as a native binary

Tradeoff:

C++ application infrastructure is more verbose than Rust/Go for parsing and CLI concerns.

## ADR-002 — cmakegen is self-hosted

Decision:

The repository root project is described by `project.yml`.

Generated CMake is used for the normal build.

Why:

- proves the product works on a real nontrivial C++ project
- continuously exercises core features
- creates a strong feedback loop for schema design

## ADR-003 — Keep generated CMake in Git

Decision:

Self-hosted generated files are committed.

Why:

- fresh clones work immediately
- IDEs can configure immediately
- no chicken-and-egg requirement
- generation diffs are reviewable

CI verifies them with `generate --check`.

## ADR-004 — Separate bootstrap CMake exists

Decision:

Maintain a minimal manual CMake project under `bootstrap/`.

Why:

A generator cannot generate its own first build without an already existing binary.

Tradeoff:

There is a small amount of unavoidable duplicate build knowledge.

Mitigation:

Keep bootstrap intentionally minimal and test it continuously.

## ADR-005 — Bootstrap is not canonical

Decision:

If bootstrap and `project.yml` disagree about normal project behavior, `project.yml` is canonical.

Bootstrap only needs to build a generator capable of regeneration.

## ADR-006 — Explicit generated source lists

Decision:

cmakegen scans source directories and emits explicit CMake target sources.

Why:

- deterministic
- visible
- IDE-friendly
- avoids CMake glob semantics

## ADR-007 — Separate RawSpec and Project model

Decision:

Do not render from YAML structures.

Why:

- schema syntax stays decoupled from semantics
- self-host schema can evolve without contaminating generation logic

## ADR-008 — Minimal custom CMake escape hatch

Decision:

MVP includes user-owned `.cmake` files rather than arbitrary inline fragments.

Why:

This preserves clean ownership boundaries for generated code.

## ADR-009 — External dependency strings remain CMake targets

Decision:

A dependency matching a project target is internal; otherwise it is emitted as an external/imported target string.

Future schemas may introduce explicit typed dependencies to catch typos.

## ADR-010 — CMake and compiler compatibility baseline

Decision:

MVP requires CMake 3.28 or later and emits `CMakePresets.json` schema version
8. Supported compiler baselines are GCC 12.1 or later, Clang 16 or later when
used with libc++ 16 or later, and Visual Studio 2022 17.6 or later.

Why:

- CMake 3.28 supports preset schema version 8.
- These standard-library baselines provide `std::expected`, which is part of
  the planned result model.
- The set covers the Linux, macOS, and Windows CI families without requiring a
  nonstandard result type.

Consequence:

`std::expected` availability is checked during the bootstrap configure step.
MVP does not require `std::format`; diagnostics may use ordinary string
composition, avoiding an additional formatting dependency.

## ADR-011 — Dependencies use CMake packages; bootstrap fallback is pinned FetchContent

Decision:

`project.yml` records package names, compatible versions, and imported target
names only. It never contains GitHub URLs or acquisition settings. Generated
CMake uses `find_package()`.

The repository keeps one user-owned `cmake/dependencies.cmake`. It first uses
externally supplied CMake packages and otherwise uses `FetchContent` with
pinned upstream release archives and `URL_HASH` values. It is included before
generated package lookup. The bootstrap build includes the same file; Catch2
is excluded there because bootstrap builds only the functional generator.

Why:

- preserves CMake's normal package-manager integration for cmakegen users
- allows a fresh checkout of this repository to bootstrap with network access
  and no preinstalled C++ libraries
- pins the fallback sources without extending the project schema into a
  package-manager configuration language

Selected MVP packages:

| Role | Package and version | CMake target |
|---|---|---|
| YAML | yaml-cpp 0.8.0 | `yaml-cpp::yaml-cpp` |
| JSON | nlohmann_json 3.12.0 | `nlohmann_json::nlohmann_json` |
| CLI | CLI11 2.6.2 | `CLI11::CLI11` |
| Tests | Catch2 3.15.3 | `Catch2::Catch2WithMain` |

`cmake/dependencies.cmake` selects exact releases. Any dependency upgrade changes its
version, archive URL, and SHA-256 together in one reviewable commit.

Hashing for generated-file ownership uses an in-tree, audited SHA-256
implementation rather than adding a cryptography dependency. Its source and
license must be recorded when T011 introduces it.
