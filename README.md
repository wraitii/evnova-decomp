# Escape Velocity Nova recompilation

C++23 reimplementation project for the *Escape Velocity Nova* engine. The original game data and reverse-engineering artefacts remain local and are intentionally not versioned.

The reimplementation is based on the [Windows Community Edition](https://escape-velocity.games), since that's the "latest patch" for Nova.  
One exception: the hyperjump animation mimics the original Mac behaviour with fade-in/out.

## Scope

I want to get as close as possible to 100% feature parity, including original quirks. This will probably not be achieved anytime soon, as RNG & exact timing is quite tricky to get right. However, in Nova's case I don't think this matters enormously to get the feel 100% right, so that's what I'm going for.

Relation to:
- Cosmic Frontier: an Override remake. I suppose Cosmic Frontier could be finished using this engine.
- NovaSwift: this decompilation is much closer in fidelity to the original Nova behaviour.
- Singularity Horizon: unclear

## Current status

Untested, but original nova should be 100% playable with essentially no noticeable differences to a regular player.
Remaining missing pieces are small, e.g. ship paints, shield bubbles, stellar domination, and a few highly specific behaviours that stock nova didn't test but plug-ins might.
However, many parts are not deterministically reproducible, so there will be minor differences in AI tick rate & the like that lead to a technically different gameplay experience.

## Extra features / divergences

The reimplementation exposes "extra preferences" in the prefs dialog. You can set scaling independently for all UI/HUD, the in-game flight scene, and the mission dialog specifically because 9pt geneva will damage your eyesight.
The port also fixes a number of known bugs, some of which can be toggled on/off. I maintain a full list of known bugs/quirks [here](docs/known_original_bugs.md).
Finally, it's rendering on Hi-DPI properly by default. For now, the simulation speed is capped like the original Nova (mostly 30/48fps depending), though I plan to unlock that later.

## Game data

The reimplementation runs on an installed copy of the Windows CE data. That data is not versioned here.
The game asks for the Community Edition .exe when starting so it can locate the data, unles it can find it automatically. It checks:
- the directory next to the executable,
- `EV Nova/` relative to the working directory,
- `../../../EV Nova` relative to the working directory (for tests).

User files are stored at `SDL_GetPrefPath("Ambrosia Software", "EV Nova")`: on macOS `~/Library/Application Support/Ambrosia Software/EV Nova/`.

### Plug-ins

The reimplementation loads windows & mac-os plug-ins, so there's technically no need to convert. (Note: macos dcmp-compressed data is unsupported but I really doubt this affects any plug-in). You can put them in the regular CE plugin folder or in your user-specific folder.

Total conversions are currently not particularly supported (you'd have to replace files manually), I plan to implement some sort of mod manager at some point here.


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
- **Linux (Ubuntu 24.04):** install the compiler, CMake, Ninja, and SDL build dependencies:

  ```sh
  sudo apt-get update
  sudo apt-get install --no-install-recommends -y \
    build-essential cmake ninja-build pkg-config \
    autoconf autoconf-archive automake libtool libltdl-dev \
    libegl1-mesa-dev libibus-1.0-dev libwayland-dev libx11-dev \
    libxext-dev libxft-dev libxkbcommon-dev
  ```

- **Windows:** install Visual Studio 2022 with the *Desktop development with C++* workload plus CMake and Ninja (`winget install Kitware.CMake Ninja-build.Ninja`). Open an **x64 Developer PowerShell for VS 2022** (so the MSVC environment is initialized), then bootstrap vcpkg in that shell:

  ```powershell
  git clone https://github.com/microsoft/vcpkg.git "$env:USERPROFILE\vcpkg"
  & "$env:USERPROFILE\vcpkg\bootstrap-vcpkg.bat"
  $env:VCPKG_ROOT = "$env:USERPROFILE\vcpkg"
  ```

  Set `VCPKG_ROOT` after opening the developer shell; Visual Studio environment initialization can overwrite variables set earlier. Run the build from that same shell so Ninja can find `cl.exe`.

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
