#pragma once

// Shared rectangle helpers for the dialog/UI code. These used to be copied
// into each translation unit with slightly different edge semantics. Two
// original hit tests exist and must not be conflated:
//   - Rect_ContainsPoint (0x004b8e90): half-open, inclusive top/left and
//     exclusive bottom/right -- every dialog control hit-test.
//   - Sprite_TestOpaquePixelAtPoint (0x00475e20): inclusive bounds, then a
//     mask/pixel test -- the menu sprite hover.

#include <SDL3/SDL.h>

namespace evnova::util {

// Ghidra 0x004b8e90 Rect_ContainsPoint. Toolbox PtInRect convention.
[[nodiscard]] inline bool Contains(const SDL_FRect &rect, float x, float y) {
  return x >= rect.x && x < rect.x + rect.w && y >= rect.y &&
         y < rect.y + rect.h;
}

[[nodiscard]] inline bool Contains(const SDL_FRect &rect, SDL_FPoint point) {
  return Contains(rect, point.x, point.y);
}

// Ghidra 0x00475e20 Sprite_TestOpaquePixelAtPoint's bounds pre-check: unlike
// Rect_ContainsPoint this includes the bottom/right edge.
[[nodiscard]] inline bool
ContainsInclusive(const SDL_FRect &rect, float x, float y) {
  return x >= rect.x && x <= rect.x + rect.w && y >= rect.y &&
         y <= rect.y + rect.h;
}

[[nodiscard]] inline bool ContainsInclusive(const SDL_FRect &rect,
                                            SDL_FPoint point) {
  return ContainsInclusive(rect, point.x, point.y);
}

[[nodiscard]] inline SDL_FRect OffsetRect(SDL_FRect rect, SDL_FPoint origin) {
  rect.x += origin.x;
  rect.y += origin.y;
  return rect;
}

} // namespace evnova::util
