# Escape Velocity Nova recompilation

Clean-room C++23 recompilation project for *Escape Velocity Nova*. The original game data and reverse-engineering artefacts remain local and are intentionally not versioned.

## Prerequisites

Install CMake (3.25+), Ninja, a C++23-capable compiler, and [vcpkg](https://github.com/microsoft/vcpkg). Set `VCPKG_ROOT` to the vcpkg checkout root.

## Build and test

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

The `evnova` executable is currently an SDL3 window/event-loop smoke application. Dependencies are declared in `vcpkg.json`; CMake installs them automatically through the vcpkg toolchain.
