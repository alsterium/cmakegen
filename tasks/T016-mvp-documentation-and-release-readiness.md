# T016 — MVP documentation and release readiness

## Purpose

Close the gap between a technically self-hosting implementation and an MVP that
users can build, understand, and verify.

## Scope

- Document prerequisites, fresh-clone bootstrap, normal development,
  regeneration, recovery, and CLI examples.
- Add install/version behavior and a minimal release workflow only to the extent
  required by the implementation plan's release-polish phase.
- Reconcile completed behavior with the specifications and ADRs.
- Run and record the complete Definition of Done checklist.

## Dependencies

T015.

## Acceptance criteria

- A new contributor can follow documented commands from a fresh clone.
- Version output and installation behavior are documented and tested.
- No generated/manual ownership boundary is ambiguous.
- All MVP checklist items in `docs/09_TEST_STRATEGY.md` and
  `docs/12_MVP_BACKLOG.md` are satisfied or explicitly recorded as follow-up
  work rather than silently omitted.

## Verification

- Execute the documented bootstrap and normal-build instructions verbatim.
- Run the full test suite and generation checks.
- Review documentation links and commands on all CI platforms.
