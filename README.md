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

Configure the Release build for normal iteration. CMake invokes vcpkg automatically and installs the dependencies declared in `vcpkg.json`.

```sh
cmake --preset release
```

Build and run the test suite:

```sh
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

To run selected tests, append `-R '<pattern>'` to the CTest command. Tests run from the repository root so they can locate local game data.

For C++ changes, also build Debug:

```sh
cmake --preset debug
cmake --build build/debug
```

Debug builds retain debug information and use `-Og` on Clang/GCC-family compilers. `ctest --preset debug` runs that build's tests.

Run the current SDL3 title/main-menu build:

```sh
./build/release/src/evnova
```

Both presets disable clang-tidy during builds because whole-codebase analysis is slow. Format changed C++ files with `clang-format -i` and lint affected translation units using the generated compilation database:

```sh
clang-tidy -p build/release --checks='clang-analyzer-*' --warnings-as-errors='*' src/path.cpp
```

Replace `src/path.cpp` with the changed source file; for header changes, choose affected source files that include it. CMake locates Homebrew LLVM for build-integrated linting, but standalone commands require `clang-tidy` on your `PATH` (or its full path).

Build products and vcpkg-installed packages are local and ignored by Git.
