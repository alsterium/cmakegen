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

## Execution record

- Date: 2026-08-09
- Summary: Added a `std::filesystem`-based source resolver that expands
  recursive and non-recursive directory entries into stable, slash-normalized
  source lists. Directory expansion recognizes the MVP source/header extension
  set; explicit files remain present regardless of extension. Exclusions match
  normalized entry-relative and project-relative glob paths. Directory
  symlinks are not traversed, and missing or unreadable paths return structured
  `CMG2001` diagnostics.
- Verification: `cmake -S bootstrap -B build/bootstrap && cmake --build
  build/bootstrap --parallel 2 && ctest --test-dir build/bootstrap
  --output-on-failure` (pass; 59/59 tests).
- Generated files: unchanged; normal generated CMake does not exist until
  T014.
- Follow-ups: T008 consumes the resolved source lists during semantic
  validation.

## Execution record — review follow-up

- Date: 2026-08-09
- Summary: Stopped silently skipping unreadable directories during source
  traversal. Permission failures now produce the existing structured `CMG2001`
  diagnostic instead of generating an incomplete source list.
- Verification: `cmake --build build/bootstrap --parallel 2 && ctest --test-dir
  build/bootstrap --output-on-failure` (pass; 60/60 tests).
- Generated files: unchanged.
- Follow-ups: none.

## Execution record — review follow-up 4

- Date: 2026-08-09
- Summary: Preserved each normalized source entry's YAML path and source
  location so resolver filesystem diagnostics identify the corresponding
  `targets.<name>.sources[...]` entry rather than only the filesystem path.
- Verification: `cmake --build build/bootstrap --parallel 2 && ctest --test-dir
  build/bootstrap --output-on-failure`.
- Generated files: unchanged.
- Follow-ups: none.

## Execution record — review follow-up 3

- Date: 2026-08-09
- Summary: Made single-character glob wildcards path-component local, so `?`
  cannot match `/`. The resolver now reports an invalid-value diagnostic when
  overlapping entries give the same source path different kinds, eliminating
  implementation-dependent duplicate selection.
- Verification: `cmake --build build/bootstrap --parallel 2 && ctest --test-dir
  build/bootstrap --output-on-failure`.
- Generated files: unchanged.
- Follow-ups: none.

## Execution record — review follow-up 2

- Date: 2026-08-09
- Summary: Applied an explicit source kind consistently to files expanded from
  directories, matching explicit-file resolution semantics.
- Verification: `cmake --build build/bootstrap --parallel 2 && ctest --test-dir
  build/bootstrap --output-on-failure`.
- Generated files: unchanged.
- Follow-ups: none.
