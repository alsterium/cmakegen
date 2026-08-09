# T008 — Dependency graph and semantic validation

## Purpose

Reject invalid normalized projects and expose the graph representation needed
by generation and CLI output.

## Scope

- Validate project/target names, duplicate logical definitions, source paths,
  scopes, and interface-library rules.
- Build the internal target adjacency list while retaining imported targets for
  optional display.
- Detect internal dependency cycles and report the concrete cycle path.
- Provide deterministic text and DOT-ready graph data without implementing CLI
  option parsing.

## Dependencies

T006 and T007.

## Acceptance criteria

- All documented semantic errors are diagnosed before rendering or writing.
- Imported targets are not incorrectly rejected as missing internal targets.
- Cycle diagnostics show a stable path such as `Core -> Platform -> Core`.
- Interface libraries reject implementation sources and invalid scopes.
- Graph ordering is deterministic.

## Verification

- Run semantic-validator and graph unit tests, including multi-node cycles and
  acyclic diamonds.

## Execution record

- Date: 2026-08-09
- Summary: Added semantic validation and deterministic target graph data. The
  validator retains imported dependencies, recognizes internal dependencies by
  project target name, detects stable concrete cycle paths, and rejects invalid
  logical names, source paths, interface-library scopes, and implementation
  sources (including resolved sources).
- Verification: `cmake -S bootstrap -B build/bootstrap && cmake --build
  build/bootstrap --parallel 2 && ctest --test-dir build/bootstrap
  --output-on-failure && git diff --check` (pass; 72/72 tests).
- Generated files: unchanged; normal generated CMake does not exist until T014.
- Follow-ups: T009 and T012 should consume `validate_project` before rendering
  or writing generated output.
