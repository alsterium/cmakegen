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
