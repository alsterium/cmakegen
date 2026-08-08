# T002 — Bootstrap project foundation

## Purpose

Create a buildable C++23 skeleton that supports incremental implementation and
the future self-host bootstrap path.

## Scope

- Add the documented `include/`, `src/`, `tests/`, and `bootstrap/` structure.
- Define `cmakegen_core`, `cmakegen`, and the unit-test target.
- Integrate the dependencies selected by T001.
- Keep `main.cpp` thin and provide working help/version placeholders.
- Establish test discovery through explicitly transitional bootstrap wiring so
  implementation tasks can run tests before self-hosting. T014 must remove that
  wiring from the final minimal bootstrap once generated normal CMake owns it.

## Dependencies

T001.

## Acceptance criteria

- The bootstrap project configures and builds with C++23.
- `cmakegen_core` is reusable by tests and the executable.
- `cmakegen --help` succeeds.
- One smoke unit test is discovered and passes.
- No production implementation language other than C++ is introduced.
- Transitional bootstrap-only test wiring is clearly marked and does not claim
  to be the canonical normal project definition.

## Verification

```bash
cmake -S bootstrap -B build/bootstrap
cmake --build build/bootstrap
ctest --test-dir build/bootstrap --output-on-failure
build/bootstrap/cmakegen --help
```

Account for configuration-specific executable paths on multi-config generators.
