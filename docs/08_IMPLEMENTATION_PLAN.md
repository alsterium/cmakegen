# Implementation Plan

This plan assumes the entire production implementation is C++23 and that the project will become self-hosting.

## Phase 0 — Manual bootstrap skeleton

Create:

```text
bootstrap/CMakeLists.txt
src/main.cpp
include/cmakegen/
tests/
```

Bootstrap CMake should build:

- `cmakegen_core`
- `cmakegen`

At this point it may use a manually listed source set.

Acceptance:

```bash
cmake -S bootstrap -B build/bootstrap
cmake --build build/bootstrap
build/bootstrap/cmakegen --help
```

On Windows, account for configuration-specific executable paths where required.

## Phase 1 — C++ project foundation

Set C++23.

Add dependencies:

- yaml-cpp
- CLI parser
- nlohmann/json
- test framework

Create target split:

```text
cmakegen_core
cmakegen
cmakegen_tests
```

Keep `main.cpp` minimal.

## Phase 2 — Raw YAML parsing

Implement schema-version-1 parsing.

Requirements:

- unknown keys rejected
- source shorthand supported
- scoped-property shorthand supported
- YAML logical paths retained for errors

No CMake generation yet.

## Phase 3 — Normalized domain model

Create strongly typed C++ structs/enums independent of YAML.

Implement:

- defaults
- enum conversion
- path normalization
- shorthand normalization
- stable ordering

Add:

```bash
cmakegen dump --format json
```

## Phase 4 — Source resolver

Use `std::filesystem`.

Implement:

- explicit files
- recursive directory scanning
- supported source/header extensions
- exclusion patterns
- no directory symlink traversal by default
- stable path sorting

## Phase 5 — Semantic validation

Implement:

- target names
- interface target rules
- duplicate logical definitions
- internal cycle detection
- missing source paths
- invalid scope combinations

## Phase 6 — CMake renderer

Implement root and generated target renderers.

Required output:

```text
CMakeLists.txt
cmake/cmakegen.generated.cmake
```

Golden-test exact bytes.

## Phase 7 — Preset renderer

Generate deterministic:

```text
CMakePresets.json
```

Keep preset/CMake compatibility mapping in a dedicated component.

## Phase 8 — Safe writer and manifest

Implement:

- SHA-256 or equivalent stable content hashing
- ownership manifest
- atomic writes
- `--force`
- `--check`
- stale generated file cleanup

## Phase 9 — Complete CLI

Implement:

```text
generate
validate
dump
graph
clean
```

## Phase 10 — Define cmakegen with cmakegen

Create repository root `project.yml` describing:

```text
cmakegen_core
cmakegen
cmakegen_tests
```

Include required packages.

Use the bootstrap binary to run:

```bash
build/bootstrap/cmakegen generate
```

Commit generated:

```text
CMakeLists.txt
CMakePresets.json
cmake/cmakegen.generated.cmake
.cmakegen/manifest.json
```

From this phase onward, root hand-edited target declarations are forbidden.

## Phase 11 — Self-host build

Build using generated project:

```bash
cmake --preset debug
cmake --build --preset debug
```

Run newly built cmakegen:

```bash
<normal-build>/cmakegen generate --check
```

Acceptance:

No generated files differ.

## Phase 12 — CI self-host loop

CI matrix:

```text
Linux
macOS
Windows
```

Each platform must:

1. bootstrap cmakegen
2. run bootstrap cmakegen `generate --check`
3. configure normal generated project
4. build normal cmakegen
5. run unit tests
6. run normal cmakegen `generate --check`

Optional stronger test:

- generate outputs with bootstrap binary
- generate outputs with normal binary
- compare bytes

## Phase 13 — Bootstrap minimization

Once self-hosting works, remove unnecessary logic from `bootstrap/CMakeLists.txt`.

Bootstrap should remain boring and stable.

Do not mirror every normal project feature into bootstrap.

## Phase 14 — Release polish

Add:

- install rules
- package/release workflow
- version output
- documentation
- pre-commit or CI generation check

## Recommended post-MVP order

1. FetchContent support
2. CTest/test target abstraction
3. install/export
4. explicit internal/external dependency syntax
5. vcpkg convenience
6. Conan integration
7. CMake File API inspection
8. build/test/run wrappers
