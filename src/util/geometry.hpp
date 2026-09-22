#pragma once

// Shared rectangle helpers for the dialog/UI code. These used to be copied
// into each translation unit with slightly different edge semantics. Two
// original hit tests exist and must not be conflated:
//   - Rect_ContainsPoint (0x004b8e90): half-open, inclusive top/left and
//     exclusive bottom/right -- every dialog control hit-test.
//   - Sprite_TestOpaquePixelAtPoint (0x00475e20): inclusive bounds, then a
//     mask/pixel test -- the menu sprite hover.
//
// The integer QuickDraw rect algebra (InsetRect/IntersectRect) is the pair the
// original uses for the radar blip clip and the mission observe-on-screen test.

#include <SDL3/SDL.h>

#include <algorithm>
#include <optional>

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

// Ghidra 0x004b8dc0 Rect_Inset. QuickDraw insets a rect symmetrically: `dy`
// shrinks top/bottom and `dx` shrinks left/right (the original mutates in
// place; this returns the result).
[[nodiscard]] inline SDL_Rect InsetRect(SDL_Rect rect, int dx, int dy) {
  rect.x += dx;
  rect.y += dy;
  rect.w -= 2 * dx;
  rect.h -= 2 * dy;
  return rect;
}

// Ghidra 0x004b8df0 Rect_Intersect. QuickDraw SectRect: the overlap of two
// rects -- max of lefts/tops, min of rights/bottoms. Edges that merely touch
// give an empty intersection, reported as nullopt.
[[nodiscard]] inline std::optional<SDL_Rect> IntersectRect(const SDL_Rect &a,
                                                           const SDL_Rect &b) {
  const int left = std::max(a.x, b.x);
  const int top = std::max(a.y, b.y);
  const int right = std::min(a.x + a.w, b.x + b.w);
  const int bottom = std::min(a.y + a.h, b.y + b.h);
  if (left >= right || top >= bottom) {
    return std::nullopt;
  }
  return SDL_Rect{left, top, right - left, bottom - top};
}

} // namespace evnova::util
