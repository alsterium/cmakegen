# Product Requirements Document

## 1. Product summary

`cmakegen` is a C++ command-line application that converts a declarative YAML project specification into idiomatic CMake configuration.

Primary goal:

> Make `project.yml` the concise source of truth for a C/C++ project while keeping CMake as the real build system.

A defining product requirement is **self-hosting**:

> Once the first usable binary exists, cmakegen must be capable of generating the CMake project used to build cmakegen itself.

## 2. Implementation requirements

The production implementation must be written in **C++23**.

Core source code must not depend on another implementation language.

Allowed implementation dependencies should preferably be available through standard CMake package flows.

Recommended dependency profile:

- CLI parsing: CLI11 or lightweight internal parser
- YAML parsing: yaml-cpp
- JSON: nlohmann/json
- formatting: `std::format` if toolchain support is sufficient, otherwise fmt
- testing: Catch2 or GoogleTest
- hashing: small embedded SHA-256 implementation or a stable library dependency

Prefer standard library facilities whenever reasonable:

- `std::filesystem`
- `std::expected` where available
- `std::variant`
- `std::optional`
- ranges
- `std::span`
- `std::string_view`

## 3. Target user

Primary:

- C/C++ developers
- small-to-medium native applications and libraries
- developers who want cleaner project definitions than hand-maintained repetitive CMake
- developers using CLion, VS Code, Visual Studio, Xcode, Ninja, or command-line CMake

## 4. Product principles

### CMake remains the build system

cmakegen must never become a compiler driver or independent build engine.

### C++ first

cmakegen should feel native to the ecosystem it manages.

### Self-hosting

The repository's canonical project structure is defined by its own `project.yml`.

### Deterministic generation

Identical spec + filesystem state + cmakegen version must produce byte-identical generated output.

### Readable generated CMake

Generated files should be understandable without cmakegen.

### Convention over configuration

Simple projects must stay simple.

### Safe bootstrap

A fresh clone must be buildable even when cmakegen is not already installed.

## 5. Self-hosting user story

Given a fresh clone:

```bash
cmake -S bootstrap -B build/bootstrap
cmake --build build/bootstrap
```

produces a bootstrap `cmakegen` binary.

Then:

```bash
build/bootstrap/cmakegen generate
```

regenerates the canonical root CMake files.

Then:

```bash
cmake --preset debug
cmake --build --preset debug
```

builds cmakegen using CMake generated from its own `project.yml`.

## 6. Core user stories

### Minimal executable

```yaml
schemaVersion: 1
name: hello

targets:
  hello:
    type: executable
    sources:
      - src
```

### Internal library dependency

Targets may link project targets.

### External package

Packages can be declared with `find_package` and imported targets linked explicitly.

### Source expansion

Directories are scanned by cmakegen and emitted as explicit paths.

### Platform-specific additions

macOS, Linux, and Windows target overrides are supported.

### Presets

Configure/build presets are generated.

### Validation

Invalid specs fail before files are written.

### Self-host consistency

CI can run:

```bash
cmakegen generate --check
```

and fail if checked-in generated CMake does not match `project.yml`.

## 7. Repository canonical files

A self-hosted cmakegen repository should contain:

```text
project.yml
CMakeLists.txt                     # generated, checked in
CMakePresets.json                  # generated, checked in
cmake/
  cmakegen.generated.cmake         # generated, checked in
  dependencies.cmake               # optional user-owned
bootstrap/
  CMakeLists.txt                   # manually maintained minimal bootstrap
src/
include/
tests/
.cmakegen/
  manifest.json
```

## 8. Generated files in version control

For self-hosted repositories, generated CMake files **should be committed**.

Reasons:

- fresh clone is immediately usable
- IDEs can open without first having cmakegen installed
- changes to generation are reviewable
- bootstrap is not needed for normal development
- `generate --check` can enforce synchronization

## 9. Bootstrap non-goals

Bootstrap must not become a second full project definition.

It should contain only what is necessary to compile a functional cmakegen binary.

Do not duplicate all tests, install rules, packaging, or developer conveniences in bootstrap.

## 10. Success criteria

MVP is successful when:

- cmakegen is implemented entirely in C++23
- a fresh clone can bootstrap using only CMake + C++ toolchain
- bootstrapped cmakegen regenerates root CMake
- root generated CMake builds the same project
- `cmakegen generate --check` passes after regeneration
- macOS/Linux/Windows CI validates the self-host cycle
