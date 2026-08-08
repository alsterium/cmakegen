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

## Execution record

- Date: 2026-08-08
- Summary: Added a yaml-cpp-backed, in-memory schema-v1 raw parser with
  YAML-independent raw structures for root, CMake, targets, and source
  entries. The parser retains logical paths and 1-based locations, translates
  YAML failures into structured diagnostics, and rejects unsupported versions,
  invalid target types, type errors, missing fields, invalid project names,
  and unknown keys in the implemented schema sections.
- Verification: `cmake -S bootstrap -B build/bootstrap && cmake --build
  build/bootstrap --parallel 2 && ctest --test-dir build/bootstrap
  --output-on-failure && cmake -S bootstrap -B build/bootstrap-gcc
  -DCMAKE_CXX_COMPILER=g++ && cmake --build build/bootstrap-gcc --parallel 2
  && ctest --test-dir build/bootstrap-gcc --output-on-failure && git diff
  --check` (pass; 10/10 tests in each configuration). Both `clang++` and
  `g++` resolve to Apple Clang 21.0.0 locally.
- Generated files: unchanged; normal generated CMake does not exist until
  T014.
- Follow-ups: T005 extends the raw parser for scoped properties, packages,
  platform overrides, presets, and custom CMake includes.

## Execution record — review follow-up

- Date: 2026-08-08
- Summary: Rejected multi-document YAML streams so trailing configuration cannot
  be silently ignored, and rejected duplicate keys in fixed-schema and
  dynamically named target mappings.
- Verification: `cmake --build build/bootstrap --parallel 2 && ctest
  --test-dir build/bootstrap --output-on-failure && cmake -S bootstrap -B
  build/bootstrap-gcc -DCMAKE_CXX_COMPILER=g++ && cmake --build
  build/bootstrap-gcc --parallel 2 && ctest --test-dir build/bootstrap-gcc
  --output-on-failure && git diff --check` (pass; 12/12 tests in each
  configuration).
- Generated files: unchanged.
- Follow-ups: none.

## Execution record — review follow-up 4

- Date: 2026-08-08
- Summary: Rejected implicit YAML Boolean and numeric scalars for string-valued
  fields, and applied the same string-semantics validation to target mapping
  keys.
- Verification: `cmake --build build/bootstrap --parallel 2 && ctest
  --test-dir build/bootstrap --output-on-failure && cmake -S bootstrap -B
  build/bootstrap-gcc -DCMAKE_CXX_COMPILER=g++ && cmake --build
  build/bootstrap-gcc --parallel 2 && ctest --test-dir build/bootstrap-gcc
  --output-on-failure && git diff --check` (pass; 17/17 tests in each
  configuration).
- Generated files: unchanged.
- Follow-ups: none.

## Execution record — review follow-up 3

- Date: 2026-08-08
- Summary: Accepted explicit YAML core integer and Boolean tags for typed
  fields while rejecting explicit non-string tags for string-valued fields.
- Verification: `cmake --build build/bootstrap --parallel 2 && ctest
  --test-dir build/bootstrap --output-on-failure && cmake -S bootstrap -B
  build/bootstrap-gcc -DCMAKE_CXX_COMPILER=g++ && cmake --build
  build/bootstrap-gcc --parallel 2 && ctest --test-dir build/bootstrap-gcc
  --output-on-failure && git diff --check` (pass; 15/15 tests in each
  configuration).
- Generated files: unchanged.
- Follow-ups: none.

## Execution record — review follow-up 2

- Date: 2026-08-08
- Summary: Rejected quoted YAML strings for schema integer and Boolean fields;
  yaml-cpp now accepts conversions for those strings only when the scalar has
  the plain-scalar tag used by YAML's implicit typed values.
- Verification: `cmake --build build/bootstrap --parallel 2 && ctest
  --test-dir build/bootstrap --output-on-failure && cmake -S bootstrap -B
  build/bootstrap-gcc -DCMAKE_CXX_COMPILER=g++ && cmake --build
  build/bootstrap-gcc --parallel 2 && ctest --test-dir build/bootstrap-gcc
  --output-on-failure && git diff --check` (pass; 13/13 tests in each
  configuration).
- Generated files: unchanged.
- Follow-ups: none.
