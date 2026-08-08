# project.yml Specification — Schema Version 1

## 1. Minimal file

```yaml
schemaVersion: 1
name: hello

targets:
  hello:
    type: executable
    sources:
      - src
```

## 2. Complete example

```yaml
schemaVersion: 1

name: MyApp
version: 0.1.0

cmake:
  minimumVersion: "3.28"
  languages: [CXX]
  cxxStandard: 23
  extensions: false
  include:
    - cmake/custom.cmake

packages:
  fmt:
    name: fmt
    version: "11"
    required: true

targets:
  Core:
    type: staticLibrary

    sources:
      - path: Sources/Core
        exclude:
          - "**/*_test.cpp"

    includeDirectories:
      public:
        - include
      private:
        - Sources/Core

    definitions:
      private:
        - CORE_INTERNAL=1

    compileOptions:
      private:
        - -Wall

    dependencies:
      public:
        - fmt::fmt

    platform:
      macos:
        definitions:
          private:
            - CORE_MACOS=1

  App:
    type: executable
    sources:
      - Sources/App
    dependencies:
      - Core

presets:
  debug:
    buildType: Debug

  release:
    buildType: Release
```

## 3. Root fields

### schemaVersion

Required integer.

MVP only accepts:

```yaml
schemaVersion: 1
```

### name

Required string.

Must match:

```regex
^[A-Za-z0-9_.+-]+$
```

### version

Optional semantic-ish version string.

Do not strictly enforce SemVer in MVP.

### cmake

Optional.

### packages

Optional map.

### targets

Required non-empty map.

### presets

Optional map.

If omitted, generate default debug and release presets.

## 4. `cmake`

```yaml
cmake:
  minimumVersion: "3.28"
  languages: [C, CXX]
  cStandard: 17
  cxxStandard: 23
  extensions: false
  include:
    - cmake/custom.cmake
```

Rules:

- `languages` default: `[CXX]`
- supported language values: `C`, `CXX`
- an explicit `languages: []` is invalid
- `extensions` default: false
- paths are relative to project.yml directory
- included files must not be generated or modified by cmakegen

## 5. Target types

Allowed:

```yaml
type: executable
type: staticLibrary
type: sharedLibrary
type: interfaceLibrary
```

## 6. Sources

Shorthand:

```yaml
sources:
  - src
  - include/foo.hpp
```

Expanded:

```yaml
sources:
  - path: src
    recursive: true
    exclude:
      - "**/*_test.cpp"
```

Directories are scanned at generation time.

Resolved paths are emitted explicitly into generated CMake.

Do not use `file(GLOB)` in generated files.

## 7. Scoped properties

All of these support shorthand or scoped form.

### Shorthand

```yaml
dependencies:
  - Core
  - fmt::fmt
```

Equivalent to:

```yaml
dependencies:
  private:
    - Core
    - fmt::fmt
```

### Scoped form

```yaml
dependencies:
  public:
    - Core
  private:
    - fmt::fmt
```

Same structure applies to:

- `includeDirectories`
- `definitions`
- `compileOptions`
- `dependencies`

## 8. Packages

```yaml
packages:
  fmt:
    name: fmt
    version: "11"
    required: true
    components: []
```

Generated:

```cmake
find_package(fmt 11 REQUIRED)
```

Components:

```yaml
packages:
  Qt:
    name: Qt6
    version: "6.8"
    components:
      - Core
      - Widgets
```

Generated:

```cmake
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Widgets)
```

MVP does not automatically infer which imported targets a package exports.

Users reference imported targets explicitly.

Package acquisition is outside schema version 1. In particular, packages do
not contain repository URLs, source revisions, hashes, or package-manager
settings. Projects may supply dependencies through their CMake environment, or
through a hand-maintained CMake dependency file that runs before the generated
`find_package()` calls.

## 9. Platform overrides

```yaml
platform:
  macos:
    sources:
      - platform/macos
    definitions:
      private:
        - PLATFORM_MACOS

  linux:
    dependencies:
      - Threads::Threads

  windows:
    definitions:
      - NOMINMAX
```

Supported names in schema v1:

- `macos`
- `linux`
- `windows`

Overrides are additive.

## 10. Presets

```yaml
presets:
  debug:
    buildType: Debug
    generator: Ninja

  release:
    buildType: Release
```

Normalized fields:

```text
name
buildType
generator?
binaryDir?
cacheVariables?
```

Default binary directory:

```text
build/<preset-name>
```

MVP preset name is the YAML key.

Preset names must not contain `/`, `\\`, or `$` and must not be `.` or `..`,
because the default binary directory is derived as `build/<preset-name>`.

## 11. Reserved keys

Unknown keys are errors by default.

This catches typos and makes schema evolution explicit.

Future option:

```yaml
validation:
  unknownKeys: warning
```

Not part of MVP.

## 12. Path rules

- spec-relative
- internally canonicalized
- generated with `/`
- paths may point outside project root only if explicitly given
- source directory traversal must not follow symlinked directories by default

## 13. Validation examples

Invalid:

```yaml
targets:
  App:
    type: executable
    dependencies:
      - MissingInternalTarget
```

This is not automatically invalid because it may be an imported CMake target.

To make internal references explicit in a future schema, support:

```yaml
dependencies:
  - target: Core
```

For MVP, unresolved bare dependencies are considered external imported targets.


## 14. Self-hosting requirement

The schema must be expressive enough to describe the cmakegen repository itself.

Before MVP is considered complete, the root cmakegen repository must have a valid `project.yml` using schema version 1 and generated normal-build CMake files.

Features required only by cmakegen's own build should not receive hidden special cases in the generator. If the self-host project reveals a missing generally useful feature, either:

- add it to the documented schema, or
- use the standard custom `.cmake` include escape hatch.

cmakegen must not contain logic such as:

```text
if project.name == "cmakegen" ...
```

Self-hosting must exercise normal product behavior.
