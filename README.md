# Escape Velocity Nova recompilation

Clean-room C++23 recompilation project for *Escape Velocity Nova*. The original game data and reverse-engineering artefacts remain local and are intentionally not versioned.

## Prerequisites

Install CMake (3.25+), Ninja, a C++23-capable compiler, and [vcpkg](https://github.com/microsoft/vcpkg). On macOS with Homebrew:

```sh
brew install vcpkg ninja
git clone https://github.com/microsoft/vcpkg.git "$HOME/vcpkg"
```

Set `VCPKG_ROOT` to that checkout before configuring. The variable can live in your shell profile if desired:

```sh
export VCPKG_ROOT="$HOME/vcpkg"
```

## Build and test

Configure the Debug build. CMake invokes vcpkg automatically and installs the dependencies declared in `vcpkg.json` (SDL3 and Catch2).

```sh
cmake --preset debug
```

Build and run the test suite:

```sh
cmake --build --preset debug
ctest --preset debug
```

Debug builds retain debug information and use `-Og` on Clang/GCC-family compilers.

Run the current SDL3 title/main-menu build:

```sh
./build/debug/src/evnova
```

For an optimized build, replace `debug` with `release` in the configure and build commands. Build products and vcpkg-installed packages are local and ignored by Git.
