#pragma once

// Small SDL render-clip RAII helper shared by the dialog renderer and its
// software-renderer tests. Narrows the renderer's current clip to `box` for
// the scope's lifetime and restores the previous clip (including its disabled
// state) on exit. A box disjoint from the previous clip yields a zero-sized
// clip, so nothing draws.

#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_render.h>

#include <algorithm>
#include <cmath>

namespace evnova::util {

class RenderClipScope {
public:
  RenderClipScope(SDL_Renderer *renderer, const SDL_FRect &box)
      : renderer_(renderer) {
    previous_enabled_ = SDL_RenderClipEnabled(renderer_);
    if (previous_enabled_) {
      // SDL_GetRenderClipRect reports success, not the enabled state, so the
      // enabled flag must be sampled separately and the rect only read when a
      // clip is actually active.
      SDL_GetRenderClipRect(renderer_, &previous_);
    }
    SDL_Rect clip{static_cast<int>(std::floor(box.x)),
                  static_cast<int>(std::floor(box.y)),
                  static_cast<int>(std::ceil(box.w)),
                  static_cast<int>(std::ceil(box.h))};
    if (previous_enabled_) {
      SDL_GetRectIntersection(&previous_, &clip, &clip);
      // A disjoint intersection yields negative extents, and SDL disables
      // clipping for negative w/h; clamp to an enabled empty clip instead so
      // nothing draws.
      clip.w = std::max(clip.w, 0);
      clip.h = std::max(clip.h, 0);
    }
    SDL_SetRenderClipRect(renderer_, &clip);
  }

  ~RenderClipScope() {
    SDL_SetRenderClipRect(renderer_, previous_enabled_ ? &previous_ : nullptr);
  }

  RenderClipScope(const RenderClipScope &) = delete;
  RenderClipScope &operator=(const RenderClipScope &) = delete;

private:
  SDL_Renderer *renderer_;
  SDL_Rect previous_{};
  bool previous_enabled_ = false;
};

} // namespace evnova::util
