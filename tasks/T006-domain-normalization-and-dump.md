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
