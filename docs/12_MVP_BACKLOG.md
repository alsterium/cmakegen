# MVP Backlog — C++ Self-Hosting

## C++ foundation

- [ ] C++23 enabled
- [ ] `cmakegen_core` target
- [ ] `cmakegen` executable target
- [ ] C++ unit test target
- [ ] yaml-cpp integration
- [ ] JSON integration
- [ ] CLI library/infrastructure
- [ ] test framework
- [ ] bootstrap CMake project

## Spec

- [ ] raw C++ schema structs
- [ ] unknown key rejection
- [ ] schema version
- [ ] target types
- [ ] source shorthand
- [ ] scoped values
- [ ] packages
- [ ] platform overrides
- [ ] presets
- [ ] YAML diagnostic locations

## Domain model

- [ ] strongly typed enums
- [ ] normalized paths
- [ ] normalized scoped values
- [ ] canonical ordering
- [ ] JSON dump

## Sources

- [ ] recursive std::filesystem scan
- [ ] extension filtering
- [ ] excludes
- [ ] symlink policy
- [ ] deterministic sorting

## Validation

- [ ] target validation
- [ ] interface-library rules
- [ ] cycle detection
- [ ] source errors
- [ ] diagnostic tests

## Generation

- [ ] generated root CMakeLists
- [ ] generated targets file
- [ ] packages
- [ ] sources
- [ ] include directories
- [ ] compile definitions
- [ ] compile options
- [ ] target links
- [ ] platform conditions
- [ ] C/C++ compile features
- [ ] CMakePresets.json

## Filesystem safety

- [ ] manifest
- [ ] stable hash
- [ ] ownership checks
- [ ] atomic writes
- [ ] `--force`
- [ ] `--check`
- [ ] clean only generated files

## CLI

- [ ] generate
- [ ] validate
- [ ] dump
- [ ] graph
- [ ] clean
- [ ] help/version

## Self-hosting

- [ ] root project.yml describes cmakegen
- [ ] bootstrap binary can generate root files
- [ ] generated root CMake committed
- [ ] generated root project builds cmakegen
- [ ] normal cmakegen `generate --check` passes
- [ ] bootstrap and normal generators produce equivalent outputs
- [ ] CI checks self-host cycle on Linux
- [ ] CI checks self-host cycle on macOS
- [ ] CI checks self-host cycle on Windows

## Definition of Done

A fresh repository clone with no cmakegen installed can execute:

```bash
cmake -S bootstrap -B build/bootstrap
cmake --build build/bootstrap
```

then use that binary to validate/regenerate the project, and finally:

```bash
cmake --preset debug
cmake --build --preset debug
```

to build cmakegen from its own generated CMake project.

The resulting cmakegen binary must report:

```bash
cmakegen generate --check
```

with no generated differences.
