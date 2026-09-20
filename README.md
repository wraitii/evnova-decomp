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

80-90% of the way there on vibes. There are missing pieces in a number of places like ship buying, escort management, some mission functionalities. Ship paints. Shield bubbles. Probably a number of other things. The fidelity is a little spiky, so large parts are completed with odd warts here and there too.

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

Logging always goes to stderr. Launchers that detach the console (for example a Windows bottle under Wine/CrossOver) drop that output, so set `EVNOVA_LOG_FILE=<path>` to additionally mirror the log to that file (truncated each run).

### Game data

The reimplementation runs on an installed copy of the Windows CE data. That
data (the `.rez` archives, `Nova Files/`, `Nova Plug-ins/` and the
`Charcoal.ttf`/`Geneva.ttf` fonts) is kept locally and is not versioned here.

It resolves two directories:

- **Install root** — the read-only shipped data. The first candidate that
  contains `Nova.rez` or a `Nova Files/` directory wins:
  1. the directory next to the executable,
  2. `EV Nova/` relative to the working directory,
  3. `../../../EV Nova` relative to the working directory.
- **Support folder** — per-user writable state (`Pilots/`, user plug-ins),
  from `SDL_GetPrefPath("Ambrosia Software", "EV Nova")`: on macOS
  `~/Library/Application Support/Ambrosia Software/EV Nova/`.

`.rez` archives load from weakest to strongest, so later ones override earlier
ones: `Nova.rez` → `Nova Files/*.rez` → `Nova Plug-ins/*.rez` → support-folder
`Nova Plug-ins/*.rez`. (A bare `Plug-ins/` is used when the CE-style
`Nova Plug-ins/` folder is absent.)

If none of that finds the data, the game shows a **Locate EV Nova.exe** screen
on a black background. Pick `EV Nova.exe` from your Community Edition folder;
the chosen folder is validated and remembered as `install_root` in
`EV Nova Extra Prefs.ini` under the support folder, and is used on later runs.
The usual way to avoid seeing this screen is to run from inside the install
folder, or from the repo root (which holds the gitignored `EV Nova/` folder).
