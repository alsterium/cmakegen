# T005 — Extended YAML schema parser

## Purpose

Complete schema-version-1 parsing beyond the minimal executable case.

## Scope

- Parse scoped values in shorthand and scoped forms.
- Parse packages, components, platform overrides, presets, and custom CMake
  includes.
- Reject unknown keys at every supported mapping level.
- Add positive and negative fixtures covering the complete specification.

## Dependencies

T004.

## Acceptance criteria

- The complete example in `docs/04_PROJECT_SPEC.md` parses.
- All scoped properties accept both documented forms.
- Only `macos`, `linux`, and `windows` platform names are accepted.
- Package and preset fields preserve enough information for normalization.
- Unknown nested keys report their full logical YAML path.

## Verification

- Run the complete parser fixture suite.
- Map every schema-v1 field in `docs/04_PROJECT_SPEC.md` to at least one test.
