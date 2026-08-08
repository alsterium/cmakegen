# Vibe Coding Prompt Pack — C++ Self-Hosting Edition

These prompts are intended to be given to a coding agent sequentially.

## Global instruction for every coding task

Follow these rules unless a later specification explicitly changes them:

- production code must be C++23
- CMake is the build system
- `project.yml` will become the canonical project description
- keep raw YAML model separate from normalized domain model
- never hand-edit generated root CMake after self-hosting begins
- do not add another language runtime to the production build
- keep output deterministic
- add tests for every implemented behavior

## Prompt 1 — Bootstrap C++ repository

Create the initial C++23 repository and `bootstrap/CMakeLists.txt`.

Targets:

```text
cmakegen_core
cmakegen
cmakegen_tests
```

At this stage the bootstrap CMake may manually enumerate sources.

Use documented libraries for YAML/JSON/CLI/testing.

Do not implement generation yet.

## Prompt 2 — YAML parser

Implement schema v1 parsing in C++.

Use yaml-cpp.

Requirements:

- reject unknown keys
- preserve YAML path and line/column where practical
- support shorthand syntax
- return structured diagnostics
- add unit tests

## Prompt 3 — Domain model

Implement normalized C++ domain structs/enums.

Renderer code must never depend on yaml-cpp node types.

Add canonical JSON dump.

## Prompt 4 — Filesystem source resolver

Implement with `std::filesystem`.

Requirements:

- recursive scan
- exclusions
- stable sorting
- normalized `/` output paths
- no recursive directory-symlink following by default

## Prompt 5 — Semantic validation

Implement target and graph validation.

Return diagnostics rather than printing from core library.

## Prompt 6 — Deterministic CMake renderer

Implement pure rendering:

```text
Project -> GeneratedFileSet
```

No filesystem discovery or writes inside renderer.

Add golden tests.

## Prompt 7 — Presets renderer

Implement deterministic CMakePresets generation.

Do not assume unordered-map iteration order.

## Prompt 8 — Safe writer

Implement manifest, ownership checks, hashes, `--check`, `--force`, and atomic writes.

All files must be rendered before the first mutation.

## Prompt 9 — CLI

Implement:

```text
generate
validate
dump
graph
clean
```

Keep CLI layer thin.

## Prompt 10 — Make cmakegen describe itself

Create a root `project.yml` representing the cmakegen repository.

It must define at least:

- `cmakegen_core`
- `cmakegen`
- test target(s)

Use the bootstrap cmakegen binary to generate root CMake files.

Commit those generated files.

From this point onward, treat root CMake files as generated output.

## Prompt 11 — Self-host verification

Implement CI or scripts that prove:

```text
bootstrap build
→ bootstrap cmakegen
→ generate/check
→ normal generated build
→ normal cmakegen
→ generate --check
```

No stage may require a preinstalled cmakegen.

## Prompt 12 — Remove duplication

Review `bootstrap/CMakeLists.txt`.

Reduce it to the minimum required to build a functional cmakegen.

Do not replicate testing/install/developer-only configuration unnecessarily.

Document exactly when bootstrap CMake needs manual changes.
