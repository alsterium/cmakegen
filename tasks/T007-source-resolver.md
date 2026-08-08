# T007 — Deterministic source resolver

## Purpose

Expand source entries into stable explicit file lists without relying on CMake
globs.

## Scope

- Resolve explicit files and recursive/non-recursive directories with
  `std::filesystem`.
- Filter documented source and header extensions for directory expansion.
- Preserve explicitly listed files with nonstandard extensions.
- Implement exclusion patterns and the default no-directory-symlink-traversal
  policy.
- Normalize and sort output paths deterministically.

## Dependencies

T006.

## Acceptance criteria

- Resolver behavior matches the source model and path rules.
- Missing paths and filesystem errors produce structured diagnostics.
- Directory symlinks are not traversed by default.
- Equivalent logical trees yield stable ordered results.
- Exclusion examples in the specifications pass.

## Verification

- Run resolver tests for recursion, ordering, exclusions, missing paths,
  explicit extensions, symlinks, and Windows-style path normalization.
- Ensure test fixtures require no network access.
