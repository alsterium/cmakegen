# T004 — Core YAML schema parser

## Purpose

Parse the minimal schema-version-1 project into YAML-specific raw structures
without leaking YAML nodes into the domain or renderer layers.

## Scope

- Define `RawSpec` structures for root, CMake, targets, and source entries.
- Parse required root fields, CMake settings, target types, source shorthand,
  and expanded source entries.
- Reject unknown keys in the implemented sections.
- Retain logical YAML paths and available line/column marks.
- Cover type errors, missing fields, invalid schema versions, and malformed
  source entries.

## Dependencies

T003.

## Acceptance criteria

- The minimal project example parses successfully.
- Unsupported schema versions and unknown keys fail with precise diagnostics.
- Source shorthand and expanded forms produce equivalent raw semantics.
- The parser returns raw schema types and no renderer accepts YAML nodes.

## Verification

- Run parser unit tests using valid and invalid fixtures from
  `docs/04_PROJECT_SPEC.md`.
- Confirm failed parses do not touch the filesystem.
