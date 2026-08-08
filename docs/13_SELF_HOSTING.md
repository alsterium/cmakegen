# Self-Hosting Specification

## 1. Goal

cmakegen must manage its own normal build definition.

Canonical source:

```text
project.yml
```

Generated normal-build files:

```text
CMakeLists.txt
CMakePresets.json
cmake/cmakegen.generated.cmake
```

Bootstrap source:

```text
bootstrap/CMakeLists.txt
```

## 2. The bootstrap problem

A generator cannot require itself to exist before it can be built.

Therefore cmakegen uses a two-stage model:

```text
Stage 0: plain CMake bootstrap
Stage 1: self-generated CMake
```

The bootstrap project is a recovery mechanism, not the main developer build definition.

## 3. Required fresh-clone flow

### Step 1

```bash
cmake -S bootstrap -B build/bootstrap
```

### Step 2

```bash
cmake --build build/bootstrap
```

### Step 3

Locate bootstrap executable.

Unix-like example:

```bash
build/bootstrap/cmakegen generate --check
```

If checked-in generated files are stale, run:

```bash
build/bootstrap/cmakegen generate
```

### Step 4

Configure normal project:

```bash
cmake --preset debug
```

### Step 5

Build normal project:

```bash
cmake --build --preset debug
```

### Step 6

Run the newly built binary:

```bash
cmakegen generate --check
```

The repository is self-consistent only if this succeeds.

## 4. What belongs in bootstrap

Bootstrap must include only:

- C++ standard/compiler configuration
- cmakegen_core source files
- cmakegen executable source
- third-party dependencies required for a functional generator

The dependency acquisition rules live in the hand-maintained
`cmake/dependencies.cmake`, which bootstrap and the generated normal project
both include. It uses externally supplied packages when available, with a
pinned `FetchContent` fallback for fresh checkouts. Test-only Catch2 is not
fetched by bootstrap.

Bootstrap should not normally contain:

- project tests
- golden tests
- packaging
- install/export
- benchmarks
- docs targets
- example projects
- development-only tooling

## 5. Avoiding source-list maintenance pain

The bootstrap source list is the unavoidable duplicated piece.

Recommended policy:

- manually list the small set of generator implementation `.cpp` files
- keep one source file per component rather than hyper-fragmenting implementation
- CI always builds bootstrap, so missing new files are caught quickly
- every PR adding a new core `.cpp` file should update bootstrap

Do not make bootstrap depend on a generated source-list file that is absent in a fresh clone.

## 6. Repository development workflow after self-hosting

For normal development:

1. edit C++ source
2. if only editing existing files, normal generated CMake remains valid
3. when adding/removing files under source directories, run `cmakegen generate`
4. when modifying `project.yml`, run `cmakegen generate`
5. commit `project.yml` and generated files together
6. CI runs `cmakegen generate --check`

Because source directories are expanded by cmakegen, adding a `.cpp` file requires regeneration before CMake sees the explicit source entry.

## 7. Optional developer convenience

Post-MVP, implement:

```bash
cmakegen watch
```

or IDE/editor integrations that regenerate on source-tree changes.

This is not required for initial self-hosting.

## 8. CI invariant

Every CI platform should conceptually run:

```text
A. bootstrap configure/build
B. bootstrap generator checks canonical generated files
C. normal configure/build
D. tests
E. normal generator checks canonical generated files
```

Stronger reproducibility job:

```text
bootstrap generator output
==
normal generator output
```

byte-for-byte.

## 9. Upgrade safety

When generator formatting or schema behavior changes:

1. update implementation
2. build using existing checked-in generated CMake
3. run new cmakegen
4. review generated diff
5. commit implementation + generated diff together
6. verify bootstrap still builds

This makes generator evolution possible without breaking the repository.

## 10. Disaster recovery

If checked-in generated CMake becomes invalid:

```bash
rm -rf build/bootstrap
cmake -S bootstrap -B build/bootstrap
cmake --build build/bootstrap
<bootstrap-cmakegen> generate --force
```

Then use the regenerated root project.

The bootstrap path must be kept simple enough that it remains dependable for this purpose.

## 11. Self-hosted project.yml sketch

Example direction:

```yaml
schemaVersion: 1

name: cmakegen

cmake:
  minimumVersion: "3.28"
  languages: [CXX]
  cxxStandard: 23

packages:
  yaml_cpp:
    name: yaml-cpp
    version: "0.8.0"

  nlohmann_json:
    name: nlohmann_json
    version: "3.12.0"

  CLI11:
    name: CLI11
    version: "2.6.2"

  Catch2:
    name: Catch2
    version: "3.15.3"

targets:
  cmakegen_core:
    type: staticLibrary
    sources:
      - path: src
        exclude:
          - main.cpp
      - include

    includeDirectories:
      public:
        - include

    dependencies:
      private:
        - yaml-cpp::yaml-cpp
        - nlohmann_json::nlohmann_json
        - CLI11::CLI11

  cmakegen:
    type: executable
    sources:
      - src/main.cpp
    dependencies:
      - cmakegen_core

  cmakegen_tests:
    type: executable
    sources:
      - tests
    dependencies:
      - cmakegen_core
      - Catch2::Catch2WithMain

presets:
  debug:
    buildType: Debug

  release:
    buildType: Release
```

The package names and imported targets above are fixed by the T001 dependency
baseline. Their acquisition remains outside `project.yml`.

## 12. Future dogfooding target

Once test/CTest schema support exists, cmakegen should describe its entire repository through project.yml, including:

- unit tests
- integration test executables
- install targets
- package export rules

The long-term ideal is:

> Everything except the tiny bootstrap definition is generated from cmakegen's own specification.
