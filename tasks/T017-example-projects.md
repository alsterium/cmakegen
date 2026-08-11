# T017 — Usage example projects

## Purpose

Provide standalone, human-oriented example projects that make the schema and
CLI workflow easy to understand without coupling the examples to the automated
test fixture suite.

## Scope

- Add an `examples/` directory with a usage guide and schema-v1 input projects.
- Cover a minimal executable, an internal library, platform overrides, source
  exclusions, and an external package declaration.
- Include small source trees where an example can be built after generation.
- Keep generated CMake output and build directories out of version control.
- Do not add the examples to the automated test suite or CI in this task.

## Dependencies

T012 and T014.

## Acceptance criteria

- A new contributor can choose an example, run the documented cmakegen command,
  and understand which generated files and target relationships result.
- The runnable examples configure and build with local CMake after generation,
  without requiring network access.
- The package example clearly documents its external dependency requirement.
- The examples are distinct from `tests/fixtures` and do not become test input
  implicitly.

## Verification

- Manually run `validate --resolved`, `generate`, `generate --check`, and CMake
  configure/build for the runnable examples.
- Run `git diff --check` and inspect that generated outputs are ignored rather
  than committed.

## Execution record

- Date: 2026-08-11
- Summary: Added hand-maintained `examples/` usage projects for the minimal,
  internal-library, platform, exclusion, and external-package schema patterns.
  Added a guide covering bootstrap/normal CLI usage and kept generated output
  and build directories ignored. The examples remain separate from automated
  test fixtures and CI.
- Verification: Rebuilt `build/bootstrap`, then copied the four local examples
  to a temporary directory and ran `validate --resolved`, `generate`, CMake
  configure/build, and `generate --check`; all passed. The package example also
  passed CLI validation, dump, graph, generation, and check-mode verification.
  Trailing-whitespace checks passed.
- Generated files: not added; generated output was created only in temporary
  directories and is ignored under `examples/`.
- Follow-ups: candidate to exercise selected examples in CI after the CI matrix
  is established; not part of T017.
