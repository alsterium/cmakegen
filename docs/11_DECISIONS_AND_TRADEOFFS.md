# Architecture Decisions and Tradeoffs

## ADR-001 — Production implementation is C++23

Decision:

cmakegen is implemented entirely in modern C++.

Why:

- dogfoods the ecosystem it manages
- avoids requiring another language toolchain
- makes self-hosting conceptually clean
- keeps distribution as a native binary

Tradeoff:

C++ application infrastructure is more verbose than Rust/Go for parsing and CLI concerns.

## ADR-002 — cmakegen is self-hosted

Decision:

The repository root project is described by `project.yml`.

Generated CMake is used for the normal build.

Why:

- proves the product works on a real nontrivial C++ project
- continuously exercises core features
- creates a strong feedback loop for schema design

## ADR-003 — Keep generated CMake in Git

Decision:

Self-hosted generated files are committed.

Why:

- fresh clones work immediately
- IDEs can configure immediately
- no chicken-and-egg requirement
- generation diffs are reviewable

CI verifies them with `generate --check`.

## ADR-004 — Separate bootstrap CMake exists

Decision:

Maintain a minimal manual CMake project under `bootstrap/`.

Why:

A generator cannot generate its own first build without an already existing binary.

Tradeoff:

There is a small amount of unavoidable duplicate build knowledge.

Mitigation:

Keep bootstrap intentionally minimal and test it continuously.

## ADR-005 — Bootstrap is not canonical

Decision:

If bootstrap and `project.yml` disagree about normal project behavior, `project.yml` is canonical.

Bootstrap only needs to build a generator capable of regeneration.

## ADR-006 — Explicit generated source lists

Decision:

cmakegen scans source directories and emits explicit CMake target sources.

Why:

- deterministic
- visible
- IDE-friendly
- avoids CMake glob semantics

## ADR-007 — Separate RawSpec and Project model

Decision:

Do not render from YAML structures.

Why:

- schema syntax stays decoupled from semantics
- self-host schema can evolve without contaminating generation logic

## ADR-008 — Minimal custom CMake escape hatch

Decision:

MVP includes user-owned `.cmake` files rather than arbitrary inline fragments.

Why:

This preserves clean ownership boundaries for generated code.

## ADR-009 — External dependency strings remain CMake targets

Decision:

A dependency matching a project target is internal; otherwise it is emitted as an external/imported target string.

Future schemas may introduce explicit typed dependencies to catch typos.

## ADR-010 — CMake and compiler compatibility baseline

Decision:

MVP requires CMake 3.28 or later and emits `CMakePresets.json` schema version
8. Supported compiler baselines are GCC 12.1 or later, Clang 16 or later when
used with libc++ 16 or later, and Visual Studio 2022 17.6 or later.

Why:

- CMake 3.28 supports preset schema version 8.
- These standard-library baselines provide `std::expected`, which is part of
  the planned result model.
- The set covers the Linux, macOS, and Windows CI families without requiring a
  nonstandard result type.

Consequence:

`std::expected` availability is checked during the bootstrap configure step.
MVP does not require `std::format`; diagnostics may use ordinary string
composition, avoiding an additional formatting dependency.

## ADR-011 — Dependencies use CMake packages; bootstrap fallback is pinned FetchContent

Decision:

`project.yml` records package names, compatible versions, and imported target
names only. It never contains GitHub URLs or acquisition settings. Generated
CMake uses `find_package()`.

The repository keeps one user-owned `cmake/dependencies.cmake`. It first uses
externally supplied CMake packages and otherwise uses `FetchContent` with
pinned upstream release archives and `URL_HASH` values. It is included before
generated package lookup. The bootstrap build includes the same file. While
T002 through T013 retain transitional bootstrap test wiring, it conditionally
acquires Catch2; T014 removes that wiring and the final minimal bootstrap does
not acquire test-only dependencies.

Why:

- preserves CMake's normal package-manager integration for cmakegen users
- allows a fresh checkout of this repository to bootstrap with network access
  and no preinstalled C++ libraries
- pins the fallback sources without extending the project schema into a
  package-manager configuration language

Selected MVP packages:

| Role | Package and version | CMake target |
|---|---|---|
| YAML | yaml-cpp 0.8.0 | `yaml-cpp::yaml-cpp` |
| JSON | nlohmann_json 3.12.0 | `nlohmann_json::nlohmann_json` |
| CLI | CLI11 2.6.2 | `CLI11::CLI11` |
| Tests | Catch2 3.15.3 | `Catch2::Catch2WithMain` |

`cmake/dependencies.cmake` selects exact releases. Any dependency upgrade changes its
version, archive URL, and SHA-256 together in one reviewable commit.

Hashing for generated-file ownership uses an in-tree, audited SHA-256
implementation rather than adding a cryptography dependency. Its source and
license must be recorded when T011 introduces it.

## ADR-012 — Preserve portable output identity across filesystems

Decision:

SafeWriter treats ASCII case aliases and Win32 trailing-dot/space aliases as
equivalent for generated-file ownership and collision checks on every host.
This is a portability invariant: `Foo.txt` and `foo.txt` are
collision-equivalent even on a case-sensitive host, so generated output cannot
contain both portable spellings.

The Unicode extension is filesystem-capability-aware and must not replace or
relax the portable ASCII/Win32 invariant. On supported macOS APFS/HFS volumes,
SafeWriter compares Unicode names using Core Foundation normalization and
case-folding rules, re-detecting the capability at each existing parent so a
nested mount is not compared using the project-root volume's mode. A comparison
uses both physical parent modes and applies case-insensitive comparison if
either destination is case-insensitive, keeping the result symmetric. On Windows,
native paths are converted to UTF-8 before validation, then validated through
UTF-16 and compared ordinally; the case-sensitivity flag is queried for each
existing parent directory while a path is resolved, so per-directory
case-sensitive settings are respected. These comparisons never use the process
locale. If a required Unicode capability cannot be detected safely, the
implementation must fail safe rather than claim collision safety.

All manifest keys and generated relative paths are converted explicitly between
the UTF-8 identity representation and native filesystem paths before alias
resolution. This applies equally to writes, stale cleanup, and `clean`, so a
non-UTF-8 Windows active code page cannot create a destination spelling that
differs from manifest ownership.

Temporary output names are derived through native `std::filesystem::path`
operations from their destination filename. They must not convert the basename
through a narrow string, so paths that the active Windows code page cannot
represent remain writable atomically.

Why:

- generated trees may be moved between case-sensitive and case-insensitive
  filesystems
- ownership and collision checks must remain deterministic across hosts
- locale-dependent Unicode folding would make manifests and safety decisions
  non-deterministic

Consequence:

Case-only rename and manifest migration use the portable ASCII/Win32 identity
first, then apply the finalized Unicode capability rules from T019.

## ADR-013 — Preserve scoped string provenance in the normalized model

Decision:

Normalized include directories, compile definitions, and compile options use a
`ScopedValue<T>` entry containing the effective value plus its parser path and
optional source location. Their common and platform override collections use
`ScopedStringValues`. Existing `LinkDependency` provenance remains on the
dependency value itself because graph and interface-dependency validation
already consume that shared interface.

Why:

Interface-library scope validation must identify the exact YAML entry that is
invalid. Keeping provenance alongside each normalized value avoids recomputing
locations after normalization and preserves the association when common and
platform values are validated independently.

Compatibility:

This changes the normalized public type of string scoped entries from
`std::vector<std::string>`-like values to
`std::vector<ScopedValue<std::string>>`. Renderers and dumps deliberately
unwrap `value`, so valid generated CMake and normalized dump output remain
byte-for-byte unchanged. Dependency collection types and their direct members
are unchanged.

## ADR-014 — SafeWriter publishes multi-file output through a recoverable transaction

Decision:

SafeWriter creates a deterministic journal, pre/post manifest metadata, and
numbered backups before replacing generated outputs. It replaces the manifest
last. A replacement failure is rolled back in reverse order and the old output
set is verified before the failure is reported. If rollback or automatic
recovery cannot complete, the transaction remains explicitly uncommitted under
`.cmakegen/transaction/` for operator recovery.

Each sibling staging path is marked in the journal before it is written, so an
interrupted partial staging file can be removed as transaction-owned during
rollback. A pre-existing sibling staging path is explicitly recorded as
unclaimed and preserved if staging cannot begin. Stale journal temporary files
are discarded before the next journal write. Setup and cleanup interruptions
that leave no journal are recoverable only for the known transaction
scaffolding; the complete layout is validated before cleanup and unexpected
artifacts remain protected for operator inspection. Cleanup removes the
terminal journal before its pre/post metadata, so an interruption cannot leave
a terminal journal that requires already-removed metadata. Recovery removes a
transaction-created empty parent before restoring a file-to-directory
transition and verifies every output in the post-manifest before finalizing.
Transaction directory creation is exclusive, and a lifetime lock under
`.cmakegen/transaction.lock` makes a competing writer fail instead of
recovering an active transaction. Recovery revalidates destination and staging
parents immediately before mutation and validates retained backup type/content
before removing a destination; a competing writer never cleans another
writer's artifacts.
Generated destinations are also checked against every deterministic staging
path before a transaction starts, so one output can never overwrite another
output's staging file.

An owned output whose requested spelling is equivalent to, but differs from,
the current physical spelling is represented by an explicit rename entry in
the same transaction. The entry copies the pre-state into its numbered
backup, performs a direct rename on case-sensitive filesystems, and uses a
distinct sibling temporary spelling when the source and destination are one
case-insensitive directory entry. The manifest removes the old logical
spelling and publishes the requested spelling only after that physical rename
and any content replacement succeed; rollback restores both spelling and
content. Nested parent directory spellings migrate from the deepest component
outward, and a parent containing unowned entries is not moved. Multiple rename
entries may share that migrated parent; transaction-owned sibling outputs and
their temporaries remain part of the directory migration. Recovery adopts a
resolved transaction temporary when a crash leaves the journal one spelling
behind. Descendants are checked recursively before an owned ancestor is moved,
and replacement staging uses the existing physical parent until that migration
has occurred.

Why:

- sequential filesystem replacement can expose a partial generated tree
- the manifest must never claim ownership of an incomplete new tree
- deterministic artifacts make recovery auditable and safe to retry

Tradeoff:

Successful writes temporarily use additional disk space for backups and
metadata. Check and dry-run remain write-free; successful transactions remove
all transaction artifacts after commit verification.
