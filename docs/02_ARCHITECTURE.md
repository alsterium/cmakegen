# Architecture

## 1. Language and runtime

cmakegen is a **C++23** application.

No language runtime beyond the C++ runtime should be required.

Preferred minimum compiler family targets for development:

- Clang
- GCC
- MSVC

Exact minimum versions should be selected based on the C++23 features actually used and tested in CI.

## 2. Pipeline architecture

```text
project.yml
    ↓
YamlParser
    ↓
RawSpec
    ↓
SchemaValidator
    ↓
Normalizer
    ↓
Domain Model
    ↓
SourceResolver
    ↓
SemanticValidator
    ↓
CMakeRenderer / PresetRenderer
    ↓
GeneratedFileSet
    ↓
SafeWriter + Manifest
```

Never render directly from yaml-cpp nodes.

## 3. Proposed C++ repository structure

```text
include/
  cmakegen/
    diagnostics/
      diagnostic.hpp

    spec/
      raw_spec.hpp
      parser.hpp
      validator.hpp

    model/
      project.hpp
      target.hpp
      package.hpp
      scoped_values.hpp

    normalize/
      normalizer.hpp

    resolver/
      source_resolver.hpp

    graph/
      dependency_graph.hpp

    generator/
      cmake_renderer.hpp
      presets_renderer.hpp
      generated_file.hpp
      manifest.hpp

    fs/
      atomic_writer.hpp

    cli/
      app.hpp

src/
  diagnostics/
  spec/
  model/
  normalize/
  resolver/
  graph/
  generator/
  fs/
  cli/
  main.cpp

tests/
  unit/
  golden/
  integration/
  fixtures/

bootstrap/
  CMakeLists.txt
```

Headers under `include/cmakegen` define module boundaries.

## 4. Library/executable split

cmakegen should be internally split into:

```text
cmakegen_core   static library
cmakegen        executable
```

`cmakegen_core` contains all behavior except process entry point.

Benefits:

- unit tests link directly to core
- CLI logic stays thin
- future editor/tool integrations can reuse the core
- bootstrap can compile the same core source set

Example self-host spec:

```yaml
targets:
  cmakegen_core:
    type: staticLibrary
    sources:
      - src
    exclude:
      - src/main.cpp
    includeDirectories:
      public:
        - include

  cmakegen:
    type: executable
    sources:
      - src/main.cpp
    dependencies:
      - cmakegen_core
```

The exact source schema may use a source entry exclusion rather than target-level `exclude`.

## 5. C++ data modeling

Prefer strongly typed models.

Example:

```cpp
enum class TargetType {
    executable,
    static_library,
    shared_library,
    interface_library,
};

enum class Scope {
    private_,
    public_,
    interface_,
};

struct SourceEntry {
    std::filesystem::path path;
    bool recursive = true;
    std::vector<std::string> exclude_patterns;
};
```

Use enums instead of string switching after normalization.

## 6. Error handling

Expected user errors must not use exceptions as normal control flow.

Preferred design:

```cpp
std::expected<T, DiagnosticSet>
```

If compiler support constraints make `std::expected` impractical, use a small project-local result type.

Exceptions are acceptable for truly exceptional lower-level failures if translated at application boundaries.

`Diagnostic` carries a stable `CMG` code, severity, message, logical YAML path,
and an optional 1-based source location. `DiagnosticSet` sorts by logical path,
source location, code, message, then severity; this makes multi-error reporting
reproducible. Human formatting is intentionally performed at the CLI boundary,
where ANSI color may be disabled with `--no-color`.

## 7. YAML layer

Recommended library:

```text
yaml-cpp
```

Raw YAML structures are separate from domain structures.

The parser must be capable of producing diagnostics with logical YAML paths such as:

```text
targets.Core.sources[1].exclude
```

Precise line/column data should be retained when yaml-cpp exposes marks.

## 8. JSON layer

Recommended:

```text
nlohmann/json
```

Uses:

- CMakePresets.json
- manifest
- normalized dump output

All JSON output must use deterministic key ordering controlled by the renderer rather than relying accidentally on container implementation behavior.

## 9. Rendering

Renderers should operate on `const Project&` and return generated strings.

Example:

```cpp
GeneratedFileSet render_project(const Project&);
```

They must not:

- walk source directories
- mutate project model
- invoke CMake
- write files

## 10. Dependency graph

Internal graph nodes are project targets.

Only dependencies matching project target names create internal edges.

Use a simple adjacency-list implementation unless a graph library provides clear value.

Cycle diagnostics should report the cycle:

```text
Core -> Platform -> Core
```

## 11. Filesystem safety

Use `std::filesystem`.

Write pattern:

1. render all outputs in memory
2. validate target destinations
3. create a deterministic transaction journal with pre/post manifest metadata;
   create the numbered backup directory before moving existing outputs
4. record ownership of each sibling temporary path in the journal, then write
   the temporary file and flush/close
5. move existing owned outputs into the transaction backups
6. atomically replace outputs where supported
7. update the manifest last
8. remove journal, backups, and temporary files only after commit verification;
   journal-less cleanup validates every recognized artifact before removing any
   of them, and pre-existing staging paths remain outside transaction cleanup

A failed render must produce no partial generated project.

If replacement or manifest publication fails, SafeWriter rolls back in reverse
order and verifies the pre-transaction hashes and manifest bytes before
reporting the failure. If rollback or automatic recovery fails, it writes a
deterministic uncommitted state under `.cmakegen/transaction/` containing
`journal.json`, `pre-manifest.json`, `post-manifest.json`, and `backups/`.
Check and dry-run stop before creating that state. Recovery removes a partial
transaction-owned sibling temporary even when its hash is incomplete, while
never deleting unowned files. A sibling staging path that existed before the
transaction is never claimed or removed. A journal-less transaction is
cleaned only after its complete contents match the known setup/cleanup
scaffolding; validation finishes before any backup is deleted. Recovery does
not advance the manifest until the complete post-state has been verified.

## 12. Self-host architecture

There are two build modes.

### Bootstrap mode

Input:

```text
bootstrap/CMakeLists.txt
```

Purpose:

Build a minimal usable cmakegen without requiring cmakegen.

### Normal mode

Input:

```text
project.yml
```

Generated:

```text
CMakeLists.txt
CMakePresets.json
cmake/cmakegen.generated.cmake
```

The normal mode is canonical.

Bootstrap is only recovery/first-build infrastructure.

## 13. Dependency policy for bootstrap

The bootstrap project must be able to obtain the same required third-party libraries as normal CMake.

Preferred order:

1. system-installed packages through `find_package`
2. FetchContent only if bootstrap reproducibility requires it
3. vendoring only for tiny stable components

Avoid making bootstrap dependent on cmakegen-generated CMake.

## 14. Self-host invariant

The following must hold:

```text
bootstrap binary
      │
      ▼
generate from project.yml
      │
      ▼
normal build
      │
      ▼
new cmakegen binary
      │
      ▼
generate --check
      │
      ▼
no diff
```

This is the central self-host acceptance test.
