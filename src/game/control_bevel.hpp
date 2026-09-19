#pragma once

// System-7-style control bevel shared by the modal DLOG runtime and the
// hand-rendered Preferences/Settings panels.
//
// Ghidra 0x004d0a50 FUN_004d0a50 fills the rect with `fill`, draws a 1px
// `highlight` along the top/left and a 1px `shadow` along the bottom/right,
// and expects the caller to frame the rect in the window colour afterwards.
//
// The decompile emits each edge as a MoveTo/LineTo pair, but the raw pairs are
// NOT the literal segments: DrawLineTo (0x004b97e0) decrements the larger
// coordinate on each axis by one before rasterizing (see its disassembly), so
// an edge stops one pixel short of the far corner. Drawing the argument pairs
// verbatim -- e.g. bottom from (right-2, bottom-2) to (left+1, bottom-1) --
// leaves a visually wrong diagonal "inset" shadow near the bottom-right.
//
// Net geometry with a 1x1 pen (the dialog/window default):
//   highlight top    (left+1, top+1)    -> (right-2, top+1)
//   highlight left   (left+1, top+1)    -> (left+1, bottom-2)
//   shadow bottom    (left+1, bottom-2) -> (right-3, bottom-2)
//   shadow right     (right-2, top+1)   -> (right-2, bottom-3)

#include <SDL3/SDL_render.h>

namespace game {

inline void DrawControlBevel(SDL_Renderer *renderer,
                             const SDL_FRect &box,
                             const SDL_Color &fill,
                             const SDL_Color &highlight,
                             const SDL_Color &shadow) {
  const float left = box.x;
  const float top = box.y;
  const float right = box.x + box.w;
  const float bottom = box.y + box.h;
  SDL_SetRenderDrawColor(renderer, fill.r, fill.g, fill.b, SDL_ALPHA_OPAQUE);
  SDL_RenderFillRect(renderer, &box);
  SDL_SetRenderDrawColor(
      renderer, highlight.r, highlight.g, highlight.b, SDL_ALPHA_OPAQUE);
  SDL_RenderLine(renderer, left + 1.0F, top + 1.0F, right - 2.0F, top + 1.0F);
  SDL_RenderLine(renderer, left + 1.0F, top + 1.0F, left + 1.0F, bottom - 2.0F);
  SDL_SetRenderDrawColor(
      renderer, shadow.r, shadow.g, shadow.b, SDL_ALPHA_OPAQUE);
  SDL_RenderLine(
      renderer, left + 1.0F, bottom - 2.0F, right - 3.0F, bottom - 2.0F);
  SDL_RenderLine(
      renderer, right - 2.0F, top + 1.0F, right - 2.0F, bottom - 3.0F);
}

} // namespace game
