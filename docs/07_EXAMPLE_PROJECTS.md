# Example Projects

## Example A — Minimal executable

```yaml
schemaVersion: 1
name: hello

targets:
  hello:
    type: executable
    sources:
      - src
```

Expected tree:

```text
hello/
  project.yml
  src/
    main.cpp
```

## Example B — App + internal library

```yaml
schemaVersion: 1
name: Calculator

cmake:
  cxxStandard: 23

targets:
  CalculatorCore:
    type: staticLibrary
    sources:
      - Sources/Core
    includeDirectories:
      public:
        - include

  calculator:
    type: executable
    sources:
      - Sources/App
    dependencies:
      - CalculatorCore
```

## Example C — External package

```yaml
schemaVersion: 1
name: Formatter

packages:
  fmt:
    name: fmt
    version: "11"

targets:
  formatter:
    type: executable
    sources:
      - src
    dependencies:
      - fmt::fmt
```

## Example D — Platform overrides

```yaml
schemaVersion: 1
name: CrossPlatformApp

targets:
  platform:
    type: staticLibrary
    sources:
      - src/common

    platform:
      macos:
        sources:
          - src/macos
        definitions:
          - PLATFORM_MACOS

      linux:
        sources:
          - src/linux
        definitions:
          - PLATFORM_LINUX

      windows:
        sources:
          - src/windows
        definitions:
          - PLATFORM_WINDOWS

  app:
    type: executable
    sources:
      - app
    dependencies:
      - platform
```

## Example E — Exclusions

```yaml
schemaVersion: 1
name: Engine

targets:
  Engine:
    type: staticLibrary
    sources:
      - path: src
        exclude:
          - "**/*_test.cpp"
          - "**/bench/**"
```
