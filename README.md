# Escape Velocity Nova recompilation

C++23 reimplementation project for the *Escape Velocity Nova* engine. The original game data and reverse-engineering artefacts remain local and are intentionally not versioned.

The reimplementation is based on the Windows Community Edition, since that's the latest patch for Nova, in essence.

## Scope

I want to get as close as possible to 100% feature parity, including original quirks. This will probably not be achieved anytime soon, as RNG & exact timing is quite tricky to get right. However, in Nova's case I don't think this matters enormously to get the feel 100% right, so that's what I'm going for.

Relation to:
- Cosmic Frontier: that was intended as more of an Override remake. This project substitutes the 'open source engine' part.
- NovaSwift: so far, this project is much higher fidelity, e.g. AI routines are closely reimplemented. Plus this is multiplatform.
- Singularity Horizon: unclear

## Current status

Maybe 80% of the way there? As I'm writing this, the big things that are missing are buying a ship/capturing, most escort-related stuff, a bunch of mission-adjacent functionality. Preferences & keymapping that work. Plugin support. The x2 speed branch.

## Quick legal disclaimer + License

Everything I have copyright over in this repository is licenced under MIT (see LICENSE.txt).
This repo was mostly written using 'driven' AI, so that muddies the water. Further muddying the water is that this is a reimplementation of the original engine via decompilation, which is in a kinda grey area all of its own.
In Nova's case, as far as I can tell, the original copyright holder here for the engine is now Matt Burch, the original programmer at Ambrosia Software. So if you intend to do something with any of the code here, I'd ask him. I haven't yet contacted him about this project, either, so dragons be here.
I choose to release the repo because I believe this is legally defensible regardless, and genuinely useful. Plus the game is definitely abandonware, even if that isn't really a thing.
As for the assets, that's probably under ATMOS copyright, and I think Cosmic Frontier is the closet to an answer you'll get. This repository sidesteps that problem by not providing them.

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

Configure, build, and test the Release preset (CMake installs the `vcpkg.json` dependencies automatically):

```sh
cmake --preset release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

Run the game with:

```sh
./build/release/src/evnova
```
