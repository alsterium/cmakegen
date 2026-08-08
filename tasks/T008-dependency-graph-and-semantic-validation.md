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
