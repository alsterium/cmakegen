# T006 — Domain normalization and deterministic dump

## Purpose

Convert syntax-oriented raw input into the strongly typed, stable domain model
used by all later stages.

## Scope

- Implement documented project, target, package, source-entry, scoped-value,
  platform, and preset model types.
- Apply defaults, enum conversion, shorthand expansion, spec-relative path
  normalization, and canonical ordering.
- Classify dependency strings as project or imported targets.
- Serialize the normalized model deterministically to JSON and YAML for the
  future dump command.

## Dependencies

T004 and T005.

## Acceptance criteria

- Domain types do not depend on yaml-cpp.
- Equivalent shorthand and expanded specs normalize identically.
- Defaults match the documented schema, with any prior ambiguity resolved by
  T001 documentation.
- Paths use normalized `/` representation where serialized or rendered.
- Repeated JSON and YAML serialization is byte-identical.

## Verification

- Run normalizer plus JSON/YAML golden tests.
- Cover defaults, ordering, enums, scoped values, dependency classification,
  and paths on the available host platform.

## Execution record

- Date: 2026-08-09
- Summary: Added a YAML-independent normalized domain model and conversion from
  the raw schema. Normalization applies documented defaults, canonicalizes
  logical paths with `/`, expands scoped values, classifies internal versus
  imported dependencies, and stores keyed entities in canonical key order.
  Added deterministic JSON and YAML dump APIs, including default versus
  explicit-empty preset handling.
- Verification: `cmake -S bootstrap -B build/bootstrap-gcc
  -DCMAKE_CXX_COMPILER=g++ && cmake --build build/bootstrap --parallel 2 &&
  ctest --test-dir build/bootstrap --output-on-failure && cmake --build
  build/bootstrap-gcc --parallel 2 && ctest --test-dir build/bootstrap-gcc
  --output-on-failure && git diff --check` (pass; 34/34 tests in each
  configuration). Both `clang++` and `g++` resolve to Apple Clang 21.0.0
  locally.
- Generated files: unchanged; normal generated CMake does not exist until T014.
- Follow-ups: T007 resolves normalized logical source paths relative to the
  supplied project.yml directory; T012 exposes the dump APIs through the CLI.

## Execution record — review follow-up

- Date: 2026-08-09
- Summary: Rejected unsupported CMake language names during parsing so
  normalization cannot silently coerce them to CXX. Normalized `\\` to `/`
  before lexical path processing, and quoted every YAML dump mapping key to
  ensure user-controlled keys remain valid YAML.
- Verification: `cmake --build build/bootstrap --parallel 2 && ctest --test-dir
  build/bootstrap --output-on-failure && git diff --check` (pass; 37/37 tests).
- Generated files: unchanged.
- Follow-ups: none.

## Execution record — review follow-up 4

- Date: 2026-08-09
- Summary: Preserved Windows drive-relative prefixes such as `C:foo` during
  host-independent logical-path normalization, including parent traversal.
- Verification: `cmake --build build/bootstrap --parallel 2 && ctest --test-dir
  build/bootstrap --output-on-failure && git diff --check` (pass; 43/43 tests).
- Generated files: unchanged.
- Follow-ups: none.

## Execution record — review follow-up 3

- Date: 2026-08-09
- Summary: Rejected preset names that could become path traversal or separator
  components in their derived binary directories. Added YAML NEL (U+0085)
  escaping so emitted scalar values do not change when reparsed.
- Verification: `cmake --build build/bootstrap --parallel 2 && ctest --test-dir
  build/bootstrap --output-on-failure && git diff --check` (pass; 42/42 tests).
- Generated files: unchanged.
- Follow-ups: none.

## Execution record — review follow-up 2

- Date: 2026-08-09
- Summary: Made an explicit empty CMake language sequence invalid instead of
  applying the omission default. Replaced host-native path normalization with
  host-independent logical-path normalization that preserves Windows drive
  roots. Extended YAML double-quoted scalar escaping to cover all control
  bytes.
- Verification: `cmake --build build/bootstrap --parallel 2 && ctest --test-dir
  build/bootstrap --output-on-failure && git diff --check` (pass; 40/40 tests).
- Generated files: unchanged.
- Follow-ups: none.
