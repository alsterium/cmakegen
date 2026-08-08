# Domain Model

This is the normalized internal representation. The implementation is C++23; examples below describe semantics and should map to strongly typed C++ structs/enums.

The exact implementation syntax may vary, but semantics should match this document.

## Project

```text
Project
  schema_version: u32
  name: String
  version: Optional<String>
  languages: Set<Language>
  cmake: CMakeConfig
  targets: OrderedMap<TargetName, Target>
  packages: OrderedMap<PackageAlias, Package>
  presets: PresetConfig
```

## Language

MVP:

```text
C
CXX
```

Default:

```text
CXX
```

## CMakeConfig

```text
minimum_version: String
c_standard: Optional<String>
cxx_standard: Optional<String>
extensions: bool
include_files: Vec<Path>
```

Defaults:

```text
minimum_version = "3.28"
cxx_standard = "20"
extensions = false
```

## Target

```text
Target
  name: String
  type: TargetType
  sources: Vec<ResolvedSource>
  include_dirs: ScopedValues<Path>
  compile_definitions: ScopedValues<String>
  compile_options: ScopedValues<String>
  dependencies: ScopedValues<LinkDependency>
  platform: PlatformOverrides
```

## TargetType

```text
Executable
StaticLibrary
SharedLibrary
InterfaceLibrary
```

Generated CMake:

```text
Executable      -> add_executable(name)
StaticLibrary   -> add_library(name STATIC)
SharedLibrary   -> add_library(name SHARED)
InterfaceLibrary -> add_library(name INTERFACE)
```

## Scope

```text
PRIVATE
PUBLIC
INTERFACE
```

Rules:

- Executables normally use PRIVATE dependencies/settings.
- Interface libraries may only use INTERFACE source-like usage requirements where applicable.
- Invalid scopes should be rejected rather than silently rewritten.

## ScopedValues<T>

```text
private: Vec<T>
public: Vec<T>
interface: Vec<T>
```

User shorthand defaults:

- `includeDirectories: [x]` => PRIVATE
- `definitions: [x]` => PRIVATE
- `compileOptions: [x]` => PRIVATE
- `dependencies: [x]` => PRIVATE

## SourceEntry

Before resolution:

```text
SourceEntry
  path: Path
  recursive: bool
  exclude: Vec<Glob>
  kind: auto | source | header
```

Default:

```text
recursive = true
kind = auto
```

## ResolvedSource

```text
ResolvedSource
  path: Path
  kind: SourceKind
```

MVP extensions:

Sources:

- `.c`
- `.cc`
- `.cpp`
- `.cxx`

Headers:

- `.h`
- `.hh`
- `.hpp`
- `.hxx`
- `.inl`

Unknown extensions are ignored during directory expansion.

Explicit file entries are preserved even if extension is unknown.

## Dependency

```text
LinkDependency =
  ProjectTarget(name)
  ImportedTarget(name)
```

Resolution rule:

1. if dependency string exactly matches a project target -> project target
2. otherwise treat as imported/external CMake target

This permits:

```yaml
dependencies:
  - Core
  - fmt::fmt
```

## Package

MVP:

```text
Package
  name: String
  version: Optional<String>
  required: bool
  components: Vec<String>
```

Alias key and package name may differ.

Example:

```yaml
packages:
  fmt:
    name: fmt
    version: "11"
```

## PlatformOverrides

```text
macos: TargetOverride
linux: TargetOverride
windows: TargetOverride
```

Overrides are additive in MVP.

Do not support removal/negation in schema v1.

## TargetOverride

May add:

- sources
- include directories
- definitions
- compile options
- dependencies
