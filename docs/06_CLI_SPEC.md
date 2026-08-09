# CLI Specification

Binary:

```text
cmakegen
```

## Global options

```text
--project <path>    Project spec path. Default: project.yml
--root <path>       Explicit project root. Default: spec parent directory
--verbose
--quiet
--no-color
```

## `cmakegen generate`

Generate project files.

```bash
cmakegen generate
cmakegen generate --check
cmakegen generate --force
```

Options:

```text
--check    Do not write. Exit 1 if generated files differ.
--force    Allow replacing conflicting generated destinations.
```

Behavior:

- validation always happens before write
- all output is rendered before first write
- writes should be atomic

Output:

```text
Generated 3 files.
```

If unchanged:

```text
No changes.
```

## `cmakegen validate`

Parse and semantically validate the project.

```bash
cmakegen validate
```

Optional:

```text
--resolved
```

When present, source directories are scanned and resolved as part of validation.

## `cmakegen dump`

Print normalized project representation.

```bash
cmakegen dump
cmakegen dump --format json
cmakegen dump --format yaml
```

Without `--format`, `dump` emits the normalized JSON representation. The
default is deliberately machine-readable while `--format yaml` is available
for a YAML view.

Useful for debugging both humans and AI coding agents.

## `cmakegen graph`

Print target dependency graph.

Default tree-ish text:

```text
└── App
    └── Core
```

Options:

```text
--format text
--format dot
--include-external
```

Default should include project targets and may visually distinguish imported targets.

`--verbose` writes command context and progress (including source-resolution
and validation summaries) to standard error. It does not alter `dump` or
`graph` standard output, so those formats remain consumable by tools.

## `cmakegen clean`

Delete only files owned by `.cmakegen/manifest.json`.

Never delete:

- user custom CMake files
- source files
- build directories

Options:

```text
--dry-run
```

## Post-MVP commands

Not required initially:

```text
cmakegen configure
cmakegen build
cmakegen test
cmakegen run
cmakegen inspect
```

These should be thin wrappers around CMake presets, not new build-system behavior.

## Exit status

```text
0 success
1 invalid spec / check mismatch
2 filesystem error
3 internal generation error
```

## Diagnostics

Human mode:

```text
error[CMG1003]: unknown target type "static"
  at targets.Core.type
  expected one of:
    executable
    staticLibrary
    sharedLibrary
    interfaceLibrary
```

Machine-readable diagnostics are post-MVP.
