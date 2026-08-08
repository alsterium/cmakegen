# Task Status

This is the canonical task index. The ordering below is the recommended
dependency-aware implementation sequence; tasks in the same wave may proceed
in parallel after their dependencies are complete.

## MVP tasks

| ID | Task | Status | Difficulty | Uncertainty | Depends on | Wave |
|---|---|---|---|---|---|---|
| [T000](T000-decompose-mvp-work.md) | Decompose MVP implementation work | done | S | low | — | planning |
| [T001](T001-toolchain-and-dependency-baseline.md) | Toolchain and dependency baseline | done | M | high | — | 0 |
| [T002](T002-bootstrap-project-foundation.md) | Bootstrap project foundation | done | M | medium | T001 | 1 |
| [T003](T003-diagnostics-and-result-model.md) | Diagnostics and result model | todo | S | low | T002 | 2 |
| [T004](T004-core-yaml-schema-parser.md) | Core YAML schema parser | todo | L | medium | T003 | 3 |
| [T005](T005-extended-yaml-schema-parser.md) | Extended YAML schema parser | todo | M | low | T004 | 4 |
| [T006](T006-domain-normalization-and-dump.md) | Domain normalization and deterministic dump | todo | L | medium | T004, T005 | 5 |
| [T007](T007-source-resolver.md) | Deterministic source resolver | todo | M | medium | T006 | 6 |
| [T008](T008-dependency-graph-and-semantic-validation.md) | Dependency graph and semantic validation | todo | M | low | T006, T007 | 7 |
| [T009](T009-cmake-renderer.md) | Deterministic CMake renderer | todo | L | medium | T006, T007, T008 | 8 |
| [T010](T010-presets-renderer.md) | CMake Presets renderer | todo | M | high | T001, T006 | 8 |
| [T011](T011-safe-writer-and-manifest.md) | Safe writer and ownership manifest | todo | L | medium | T003 | 6 |
| [T012](T012-cli-commands.md) | Complete MVP CLI | todo | L | medium | T006–T011 | 9 |
| [T013](T013-integration-and-determinism-tests.md) | Integration and determinism test suite | todo | L | medium | T009, T010, T011, T012 | 10 |
| [T014](T014-self-host-project-definition.md) | Self-hosted project definition and generated files | todo | L | high | T012, T013 | 11 |
| [T015](T015-self-host-ci-matrix.md) | Cross-platform self-host CI matrix | todo | M | medium | T014 | 12 |
| [T016](T016-mvp-documentation-and-release-readiness.md) | MVP documentation and release readiness | todo | M | medium | T015 | 13 |

`T012` depends on every task from `T006` through `T011`, inclusive.

## Suggested allocation by difficulty

- Starter/localized: T003
- Intermediate: T001, T002, T005, T007, T008, T010, T015, T016
- Advanced/cross-cutting: T004, T006, T009, T011, T012, T013, T014

The advanced tasks are intentionally bounded by a single subsystem or
acceptance milestone. If parallel ownership is available, the main safe lanes
are:

- parsing/model lane: T003 → T004 → T005 → T006
- filesystem lane after the model exists: T007 and T011
- generation lane after normalization: T009 and T010
- integration lane: T012 → T013 → T014 → T015

## Deferred post-MVP candidates

These remain outside the current task set until MVP acceptance or an explicit
priority decision:

- schema-level FetchContent dependency declarations (the bootstrap fallback is
  required in T002)
- CTest/test-target schema abstraction
- install/export and package workflow beyond basic release readiness
- explicit internal/external dependency syntax
- vcpkg and Conan integrations
- CMake File API inspection
- configure/build/test/run wrappers
- watch mode and editor integrations
