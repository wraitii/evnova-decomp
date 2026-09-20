# Vendored third-party code

## dr_mp3.h

Single-header MP3 decoder by David Reid (mackron), **v0.7.4**, dual-licensed
public domain / MIT-0 (see the license statement at the end of the file).

- Upstream: <https://github.com/mackron/dr_libs>
- Copied verbatim from SDL_mixer 3.2.4 (`src/dr_libs/dr_mp3.h`) as shipped by
  vcpkg's `sdl3-mixer` port, so the vendored decoder matches the one SDL_mixer
  uses.
- SHA-256: `f0919e1652ffd9469e3440b5c759d26e862a50e6dc93cb4b51cb7e7e90b00f77`

`SdlMusic` defines `DR_MP3_IMPLEMENTATION` in `src/sdl_music.cpp` and decodes
the one shipped background track (`Nova Music.mp3`). Using dr_mp3 keeps the
build free of the copyleft LGPL-2.1 runtime dependency that an SDL3_mixer +
mpg123 stack would pull in.
