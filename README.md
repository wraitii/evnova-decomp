# Escape Velocity Nova recompilation

C++23 reimplementation project for the *Escape Velocity Nova* engine. The original game data and reverse-engineering artefacts remain local and are intentionally not versioned.

The reimplementation is based on the [Windows Community Edition](https://escape-velocity.games), since that's the latest patch for Nova.  
One exception: the hyperjump animation mimics the original Mac behaviour with fade-in/out.

## Scope

I want to get as close as possible to 100% feature parity, including original quirks. This will probably not be achieved anytime soon, as RNG & exact timing is quite tricky to get right. However, in Nova's case I don't think this matters enormously to get the feel 100% right, so that's what I'm going for.

Relation to:
- Cosmic Frontier: an Override remake. I suppose Cosmic Frontier could be finished using this engine.
- NovaSwift: this decompilation is much closer in fidelity to the original Nova behaviour.
- Singularity Horizon: unclear

## Current status

90% of the way there on vibes. There are missing pieces in a number of places like ship buying, escort management, some mission functionalities. Ship paints. Shield bubbles. Probably a number of other things.

## AI slop disclaimer

Most of this repo's code was written by AI agents, mostly various versions of GPT 5.2 to 5.6. Bit of GLM 5.3 flash, deepseek flash. I've probably directed a lot of the actual naming or things. But I haven't really checked the code, mostly tested behaviour.

## Quick legal disclaimer + License

Everything I have copyright over in this repository is licenced under MIT (see LICENSE.txt).  
This repo was mostly written using 'driven' AI, so that muddies the water. Further muddying the water is that this is a reimplementation of the original engine via decompilation, which is in a kinda grey area all of its own.  
In Nova's case, as far as I can tell, the original copyright holder here for the engine is now Matt Burch, the original programmer at Ambrosia Software. So if you intend to do something with any of the code here, I'd ask him. I haven't yet contacted him about this project, either, so dragons be here.  
I choose to release the repo because I believe this is legally defensible regardless, and genuinely useful. Plus the game is definitely abandonware, even if that isn't really a thing.  
As for the assets, that's probably under ATMOS copyright, and I think Cosmic Frontier is the closet to an answer you'll get. This repository sidesteps that problem by not providing them.

## Prerequisites

You need CMake (3.25+), [Ninja](https://ninja-build.org/), a C++23-capable compiler (AppleClang/Clang, GCC 13+, or MSVC 19.3x), and [vcpkg](https://github.com/microsoft/vcpkg). Ninja is used on every platform, including Windows with MSVC.

Clone and bootstrap vcpkg once; this is where CMake fetches the SDL3, fmt and Catch2 dependencies from:

```sh
git clone https://github.com/microsoft/vcpkg.git "$HOME/vcpkg"
"$HOME/vcpkg/bootstrap-vcpkg.sh"
```

Then point `VCPKG_ROOT` at that checkout (add it to your shell profile to keep it across sessions):

```sh
export VCPKG_ROOT="$HOME/vcpkg"
```

Platform notes:

- **macOS:** `brew install cmake ninja` (the Xcode command line tools provide the compiler).
- **Linux:** install `cmake`, `ninja-build` and a C++23 compiler from your distro (`g++` 13+ or `clang++` 17+).
- **Windows:** install Visual Studio 2022 with the *Desktop development with C++* workload plus CMake and Ninja (`winget install Kitware.CMake Ninja-build.Ninja`), then bootstrap vcpkg from PowerShell:

  ```powershell
  git clone https://github.com/microsoft/vcpkg.git "$env:USERPROFILE\vcpkg" & "$env:USERPROFILE\vcpkg\bootstrap-vcpkg.bat"
  $env:VCPKG_ROOT = "$env:USERPROFILE\vcpkg"
  ```

  Run the build from an *x64 Native Tools Command Prompt for VS 2022* (or after running `vcvars64.bat`) so Ninja can find `cl.exe`.

## Build and test

Configure, build, and test the Release preset (CMake installs the `vcpkg.json` dependencies automatically the first time):

```sh
cmake --preset release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

Run the game with:

```sh
./build/release/src/evnova          # macOS / Linux
build\release\src\evnova.exe        # Windows
```

The optional probe/control harness ([docs/probe_harness.md](docs/probe_harness.md)) is built by default on macOS/Linux and off on Windows, since its transport is a POSIX socket server. Toggle it with `-DEVNOVA_ENABLE_PROBE=ON|OFF` when configuring.

The reimplementation loads the installed CE data (`Nova.rez`, `Nova Files/`, and the bundled `Charcoal.ttf`/`Geneva.ttf`), which this repo does not ship.
Either run the binary from the install folder or from the repo root (which contains the `EV Nova/` folder); the resolver looks next to the executable first, then for `EV Nova` relative to the working directory.
