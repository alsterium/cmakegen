# T000 — Decompose MVP implementation work

## Purpose

Turn the existing specifications and MVP backlog into dependency-aware,
reviewable implementation tasks.

## Scope

- Establish the task workflow and canonical status index.
- Decompose MVP work into individual task files.
- Assign difficulty, uncertainty, dependencies, and execution waves.
- Separate deferred post-MVP candidates from required MVP work.

## Dependencies

None.

## Acceptance criteria

- Every task states its purpose, scope, dependencies, acceptance criteria, and
  verification method.
- `tasks/STATUS.md` identifies implementation order and safe parallel lanes.
- Changeable external decisions are isolated in a research-first task.
- The decomposition covers the documented MVP and self-hosting Definition of
  Done.

## Verification

- Check all task links and required sections.
- Review the decomposition against `docs/08_IMPLEMENTATION_PLAN.md`,
  `docs/09_TEST_STRATEGY.md`, and `docs/12_MVP_BACKLOG.md`.

## Execution record

- Date: 2026-08-08
- Summary: Created the task workflow, canonical index, research-note guidance,
  and 16 dependency-aware implementation tasks.
- Verification: task links, required sections, and trailing whitespace checks
  passed.
- Generated files: unchanged
- Follow-ups: T001 through T016
