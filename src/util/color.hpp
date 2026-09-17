#pragma once

// c\x9alr colors arrive as 8-bit RGB triples (NovaRgbColor); SDL draws want
// SDL_Color. Shared by every dialog that consumes a color table.

#include <SDL3/SDL.h>

#include "../brgr_archive.hpp"

namespace evnova::util {

[[nodiscard]] constexpr SDL_Color ToSdlColor(NovaRgbColor color) {
  return SDL_Color{color.red, color.green, color.blue, SDL_ALPHA_OPAQUE};
}

} // namespace evnova::util
