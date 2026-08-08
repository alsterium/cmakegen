# Test Strategy

## 1. Test layers

cmakegen requires:

- C++ unit tests
- parser/normalizer tests
- source resolver tests
- golden renderer tests
- filesystem safety tests
- real-CMake integration tests
- self-host tests

Recommended framework:

- Catch2 or GoogleTest

Prefer one framework for the whole repository.

## 2. Unit tests

Focus on:

- YAML parsing
- normalization
- path handling
- graph validation
- formatting primitives
- diagnostics

## 3. Golden tests

Fixture:

```text
tests/fixtures/basic/
  project.yml
  tree/
  expected/
    CMakeLists.txt
    CMakePresets.json
    cmakegen.generated.cmake
```

Renderer output is compared byte-for-byte.

## 4. Source resolver tests

Cover:

- recursion
- ordering
- exclusions
- missing paths
- explicit nonstandard extensions
- symlinked directories
- Windows path normalization

## 5. CMake integration tests

For generated fixture projects:

```bash
cmake -S <fixture-output> -B <build-dir>
cmake --build <build-dir>
```

Do not require network access.

## 6. Filesystem safety

Test:

- unowned CMakeLists is not overwritten
- `--force` works
- `clean` removes only owned files
- `--check` performs no writes
- failed validation performs no writes
- manifest updates after successful output writes

## 7. Determinism

Test:

```text
generate
hash outputs
generate again
hash outputs
```

Hashes must match.

No generated timestamps.

## 8. Self-host test

This is mandatory before MVP completion.

### Stage A: bootstrap

```bash
cmake -S bootstrap -B build/bootstrap
cmake --build build/bootstrap
```

### Stage B: generation

Run bootstrap executable:

```bash
cmakegen generate
```

### Stage C: normal build

```bash
cmake --preset debug
cmake --build --preset debug
```

### Stage D: reproduce

Run normal executable:

```bash
cmakegen generate --check
```

Expected result:

```text
No changes.
```

## 9. Two-compiler generation equivalence

Where practical, CI should verify that the generator built by different C++ compilers produces identical output for the same fixture.

This catches accidental dependence on:

- unordered container iteration
- locale
- native path separators
- implementation-defined behavior

## 10. Cross-platform fixture principle

Generated CMake text should normally be identical on Linux/macOS/Windows for the same logical fixture.

Use normalized `/` paths in generated CMake.

Platform-specific source discovery fixture inputs may differ only where filesystem semantics force it.

## 11. Bootstrap drift test

Bootstrap source lists can become stale before cmakegen exists.

CI or a test helper should compare source files needed by bootstrap against the canonical cmakegen source set after generation.

At minimum, document bootstrap update steps clearly.

A future helper command may generate a bootstrap source include file, but bootstrap must never require that generated file to exist on a fresh clone.

## 12. Acceptance checklist

- [ ] cmakegen compiles as C++23
- [ ] parser tests pass
- [ ] normalizer tests pass
- [ ] renderer golden tests pass
- [ ] source resolver tests pass
- [ ] filesystem safety tests pass
- [ ] root `project.yml` generates current checked-in CMake
- [ ] bootstrap build succeeds
- [ ] self-host normal build succeeds
- [ ] normal binary reports `generate --check` clean
- [ ] macOS CI passes
- [ ] Linux CI passes
- [ ] Windows CI passes
