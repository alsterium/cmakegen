# Task Workflow

`STATUS.md` is the canonical index for implementation work. Individual task
files define the reviewable unit used for the normal `1 task = 1 branch = 1 PR`
workflow.

## Status values

- `todo`: ready or waiting for dependencies
- `doing`: active implementation work
- `done`: acceptance criteria and verification are complete
- `blocked`: progress requires an external decision or dependency

Only `STATUS.md` carries canonical status. Do not duplicate mutable status in
individual task files.

## Difficulty and uncertainty

Difficulty estimates review and implementation effort, not calendar time:

- `S`: localized change with a narrow test surface
- `M`: several components or non-trivial edge cases
- `L`: cross-cutting behavior, safety-sensitive code, or broad integration

Uncertainty indicates how likely research or design discovery is to change the
implementation approach:

- `low`: behavior and approach are specified
- `medium`: implementation choices remain
- `high`: current external behavior or a missing project decision must be
  verified first

An `L` task should be split before implementation if one branch cannot remain
reviewable. Record any split in `STATUS.md` and preserve the original acceptance
coverage among the replacement tasks.

## Starting a task

1. Check dependencies and acceptance criteria.
2. Create `task/TNNN-short-description`.
3. Change the task status to `doing`.
4. Read the listed specification sources.
5. Perform required external research and record it in
   `tasks/research/TNNN.md` before implementation.

## Completing a task

1. Add or update tests alongside behavior.
2. Run the verification listed by the task.
3. Check generated-file consistency when applicable.
4. Append an execution record to the task file.
5. Set the canonical status to `done`.
6. Commit using Conventional Commits and include `Task: TNNN` in the body or
   footer.

## Execution record format

Append the following section when work is performed:

```markdown
## Execution record

- Date: YYYY-MM-DD
- Summary: ...
- Verification: `command` (pass/fail)
- Generated files: unchanged / updated and checked
- Follow-ups: none / TNNN / candidate description
```
