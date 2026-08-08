# Development Guide

This repository is a self-hosting C++23 project generator. Follow the existing
specifications in `docs/` as the source of truth for product behavior and
design decisions.

## Development principles

- Respect the existing C++23, CMake, and self-hosting requirements.
- Use the specifications and design documents as the basis for changes.
- Distinguish generated files from files maintained by hand.
- Keep changes within the requested scope.
- Record specification changes and design decisions in the relevant existing
  documentation.
- For self-hosting, treat `project.yml` as the source of truth and keep
  checked-in generated CMake synchronized with it.
- Generated output must remain reproducible and readable.

## Task management

The standard task layout will be:

```text
tasks/
├── STATUS.md          # Canonical task list, dependencies, and progress
├── README.md          # Task workflow guide
├── T001-*.md          # Individual tasks
└── research/          # Research notes when needed
```

`tasks/STATUS.md` is the canonical task index. Task status must be one of:
`todo`, `doing`, `done`, or `blocked`.

Each task should state its purpose, scope, dependencies, acceptance criteria,
and verification method. Work outside a task must not be implemented silently;
add it to an existing task or record it as a candidate for a new task.

Task decomposition from the specifications will be performed separately after
this guide is established. Do not create `tasks/` files as part of this initial
documentation-only change.

External research is required before implementation only when the task depends
on information that may change, such as specifications, libraries, APIs, or
platform behavior. Record research notes in `tasks/research/` when they are
needed.

## Branches and pull requests

- The normal relationship is `1 task = 1 branch = 1 PR`.
- Name branches `task/T001-short-description`.
- Address review fixes on the same branch and in the same PR.
- Do not reuse a branch after it has been merged.
- Combine multiple small, closely related tasks in one PR only when their
  dependencies and review scope are explicit.

GitHub Issues are exception-based, not required for every task. Create an Issue
when the work spans multiple PRs, requires external discussion or user
decisions, is a long-term blocker, or needs tracking as a release-sized or
larger improvement. Small work that can be completed in one context may proceed
without an Issue.

### GitHub CLI in Codex

In this workspace, sandboxed `gh` commands may be unable to read the macOS
Keychain credential even when GitHub CLI is correctly authenticated outside the
sandbox. For any authenticated `gh` command, first run `gh auth status` with
`require_escalated`; run subsequent `gh` API, PR, and review commands with the
same permission when needed. Do not treat an authentication failure observed
only inside the sandbox as a request for the user to log in again.

## Commits

Use Conventional Commits with the task ID in the body or footer:

```text
<type>(<scope>): <short summary>
```

Allowed types:

```text
feat      New functionality
fix       Bug fix
refactor  Structural change without behavior change
perf      Performance improvement
test      Test changes
docs      Documentation changes
chore     Tooling, configuration, or maintenance
```

Commit rules:

- After completing and verifying requested work, create the appropriate
  commit or commits automatically unless the user explicitly asks to leave the
  changes uncommitted.
- Commit only files within the completed work's scope. Preserve unrelated user
  changes and leave them uncommitted.
- Each commit should represent one meaningful change.
- Separate behavior changes, refactors, tests, and documentation when
  practical.
- Keep intermediate commits buildable and testable.
- Do not use low-information messages such as `WIP` or `fix typo`.
- Include the corresponding task ID in the commit body or footer.
- For specification changes or design decisions, include the reason and
  verification result in the commit body.
- When generated CMake is updated, handle the input specification and generated
  output in the same change unit.

Example:

```text
feat(parser): add schema version validation

Task: T002
Tests: parser schema validation tests
```

## Implementation workflow

1. Read `AGENTS.md` and the relevant specification documents.
2. Check `tasks/STATUS.md` and the target task when they exist.
3. Inspect dependencies and the current state of the affected files.
4. Create research notes only when the task requires external or changeable
   information.
5. Add or update tests.
6. Implement the change.
7. Verify the build, tests, and generated-file differences as applicable.
8. Add an execution record to the individual task.
9. Update `tasks/STATUS.md`.
10. Organize the work into reviewable commits.
11. Open a PR and address review feedback in that same PR.

For changes affecting self-hosting, preserve the bootstrap path, verify the
generated CMake, and keep `project.yml` and generated files synchronized.

## Definition of done

A task is complete only when:

- Its acceptance criteria are satisfied.
- Relevant tests have been added or updated.
- Build and test results have been checked.
- Generated-file consistency has been checked when applicable.
- The task execution record has been updated.
- Incomplete work and handoff items for follow-up tasks are recorded.

## Scope of this initial setup

This guide establishes the development rules only. The initial setup does not
decompose the specifications into tasks and does not add implementation code.
The future `tasks/` structure and specification-derived task files are separate
work.
