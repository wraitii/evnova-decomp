#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace evnova::util {

// A screen's authored coordinate space and its rectangle in window points.
// The transform is deliberately independent of SDL's backing-pixel density.
struct Placement {
  enum class Rule { explicit_rect, contained, window, centered };

  SDL_FRect dst{};
  float scale = 1.0F;
  // The scale the caller asked for, before the window/container fit clamp.
  // Preserved so a resize can rebuild the placement at the same request.
  float requested_scale = 1.0F;

  // These fields retain enough information to rebuild the placement after a
  // window resize. Callers normally use one of the builders below.
  SDL_FPoint authored_size{};
  SDL_FPoint containing_size{};
  SDL_FRect anchor_bounds{};
  SDL_FPoint anchor_authored_size{};
  SDL_FPoint anchor_containing_size{};
  Rule anchor_rule = Rule::explicit_rect;
  SDL_FPoint anchor_center_normalized{0.5F, 0.5F};
  bool anchor_reflows_with_window = false;
  // The requested scale of the background placement a modal was centred in,
  // so a resize rebuilds the whole ancestor chain at its own request.
  float anchor_requested_scale = 1.0F;
  Rule rule = Rule::explicit_rect;

  [[nodiscard]] SDL_FPoint ToAuthored(SDL_FPoint window) const {
    if (scale <= 0.0F) {
      return {};
    }
    return {(window.x - dst.x) / scale, (window.y - dst.y) / scale};
  }

  [[nodiscard]] SDL_FPoint ToWindow(SDL_FPoint authored) const {
    return {dst.x + authored.x * scale, dst.y + authored.y * scale};
  }

  [[nodiscard]] SDL_FRect ToWindowRect(SDL_FRect authored) const {
    const SDL_FPoint top_left = ToWindow(SDL_FPoint{authored.x, authored.y});
    return {top_left.x, top_left.y, authored.w * scale, authored.h * scale};
  }

  // SDL applies the render scale to the viewport. Convert the placement's
  // window-point destination back to pre-scale viewport coordinates.
  [[nodiscard]] SDL_Rect ToRenderViewport() const {
    const float inv_scale = scale > 0.0F ? 1.0F / scale : 1.0F;
    return {static_cast<int>(std::lround(dst.x * inv_scale)),
            static_cast<int>(std::lround(dst.y * inv_scale)),
            static_cast<int>(std::lround(dst.w * inv_scale)),
            static_cast<int>(std::lround(dst.h * inv_scale))};
  }

  [[nodiscard]] Placement Canonicalized() const {
    Placement result = *this;
    const SDL_Rect viewport = ToRenderViewport();
    result.dst = {viewport.x * scale,
                  viewport.y * scale,
                  viewport.w * scale,
                  viewport.h * scale};
    return result;
  }

  // Rebuild a placement whose containing window changed size. Explicit
  // placements have no rule to rebuild and are returned unchanged.
  [[nodiscard]] Placement Reflow(SDL_FPoint window) const;
};

// `requested_scale` composes the caller's scale factor with the window fit:
// the result is the single clamp min(requested, window/authored). 1.0
// reproduces the historical min(1, ...) cap.
[[nodiscard]] inline Placement PlaceContained(SDL_FPoint authored,
                                              SDL_FPoint window,
                                              float requested_scale = 1.0F) {
  if (authored.x <= 0.0F || authored.y <= 0.0F || window.x <= 0.0F ||
      window.y <= 0.0F) {
    Placement placement;
    placement.requested_scale = requested_scale;
    placement.authored_size = authored;
    placement.containing_size = window;
    placement.rule = Placement::Rule::contained;
    return placement;
  }
  const float scale = std::min(
      requested_scale, std::min(window.x / authored.x, window.y / authored.y));
  const SDL_FPoint size{authored.x * scale, authored.y * scale};
  Placement placement;
  placement.dst = {
      (window.x - size.x) * 0.5F, (window.y - size.y) * 0.5F, size.x, size.y};
  placement.scale = scale;
  placement.requested_scale = requested_scale;
  placement.authored_size = authored;
  placement.containing_size = window;
  placement.rule = Placement::Rule::contained;
  return placement;
}

[[nodiscard]] inline Placement PlaceCenteredIn(SDL_FRect bounds,
                                               SDL_FPoint authored,
                                               float requested_scale = 1.0F) {
  if (authored.x <= 0.0F || authored.y <= 0.0F || bounds.w <= 0.0F ||
      bounds.h <= 0.0F) {
    Placement placement;
    placement.dst = {bounds.x, bounds.y, 0.0F, 0.0F};
    placement.requested_scale = requested_scale;
    placement.authored_size = authored;
    placement.containing_size = {bounds.w, bounds.h};
    placement.anchor_bounds = bounds;
    placement.rule = Placement::Rule::centered;
    return placement;
  }
  const float scale = std::min(
      requested_scale, std::min(bounds.w / authored.x, bounds.h / authored.y));
  const SDL_FPoint size{authored.x * scale, authored.y * scale};
  Placement placement;
  placement.dst = {bounds.x + (bounds.w - size.x) * 0.5F,
                   bounds.y + (bounds.h - size.y) * 0.5F,
                   size.x,
                   size.y};
  placement.scale = scale;
  placement.requested_scale = requested_scale;
  placement.authored_size = authored;
  placement.containing_size = {bounds.w, bounds.h};
  placement.anchor_bounds = bounds;
  placement.rule = Placement::Rule::centered;
  return placement;
}

// A window-rule placement fills the whole window at the requested scale; the
// authored extent is the window divided by that scale (the scene uses this
// for the flight-world transform). 1.0 reproduces the native 1:1 window.
[[nodiscard]] inline Placement PlaceWindow(SDL_FPoint window,
                                           float requested_scale = 1.0F) {
  Placement placement;
  placement.dst = {0.0F, 0.0F, window.x, window.y};
  placement.scale = requested_scale;
  placement.requested_scale = requested_scale;
  placement.authored_size =
      requested_scale > 0.0F
          ? SDL_FPoint{window.x / requested_scale, window.y / requested_scale}
          : window;
  placement.containing_size = window;
  placement.rule = Placement::Rule::window;
  return placement;
}

[[nodiscard]] inline Placement PlaceCenteredIn(const Placement &background,
                                               SDL_FPoint authored,
                                               float requested_scale = 1.0F) {
  // A modal is centred on the background's centre, but its native size is
  // constrained by the whole window. This keeps a 640-space dialog native
  // when a smaller fixed panel is behind it.
  SDL_FPoint available = background.containing_size;
  if (available.x <= 0.0F || available.y <= 0.0F) {
    available = {background.dst.w, background.dst.h};
  }
  const SDL_FPoint center{background.dst.x + background.dst.w * 0.5F,
                          background.dst.y + background.dst.h * 0.5F};
  const float scale = (authored.x > 0.0F && authored.y > 0.0F &&
                       available.x > 0.0F && available.y > 0.0F)
                          ? std::min(requested_scale,
                                     std::min(available.x / authored.x,
                                              available.y / authored.y))
                          : requested_scale;
  const SDL_FPoint size{authored.x * scale, authored.y * scale};
  const float x = std::clamp(
      center.x - size.x * 0.5F, 0.0F, std::max(0.0F, available.x - size.x));
  const float y = std::clamp(
      center.y - size.y * 0.5F, 0.0F, std::max(0.0F, available.y - size.y));
  Placement placement =
      PlaceCenteredIn(SDL_FRect{0.0F, 0.0F, available.x, available.y},
                      authored,
                      requested_scale);
  placement.dst = {x, y, size.x, size.y};
  placement.anchor_bounds = background.rule == Placement::Rule::centered
                                ? background.anchor_bounds
                                : background.dst;
  placement.anchor_authored_size = background.authored_size;
  placement.anchor_containing_size = background.containing_size;
  placement.anchor_rule = background.rule;
  placement.anchor_requested_scale = background.requested_scale;
  const SDL_FPoint available_norm = placement.containing_size;
  if (available_norm.x > 0.0F && available_norm.y > 0.0F) {
    placement.anchor_center_normalized = {
        (background.dst.x + background.dst.w * 0.5F) / available_norm.x,
        (background.dst.y + background.dst.h * 0.5F) / available_norm.y};
  }
  placement.anchor_reflows_with_window =
      background.rule != Placement::Rule::explicit_rect;
  return placement;
}

inline Placement Placement::Reflow(SDL_FPoint window) const {
  switch (rule) {
  case Rule::contained:
    return PlaceContained(authored_size, window, requested_scale);
  case Rule::window:
    return PlaceWindow(window, requested_scale);
  case Rule::explicit_rect:
    return *this;
  case Rule::centered:
    if (anchor_rule == Rule::contained) {
      const Placement background =
          PlaceContained(anchor_authored_size, window, anchor_requested_scale);
      return PlaceCenteredIn(background, authored_size, requested_scale);
    }
    if (anchor_rule == Rule::window) {
      const Placement background = PlaceWindow(window, anchor_requested_scale);
      return PlaceCenteredIn(background, authored_size, requested_scale);
    }
    if (anchor_rule == Rule::centered) {
      if (anchor_reflows_with_window) {
        const float resized_scale = std::min(
            requested_scale,
            std::min(window.x / authored_size.x, window.y / authored_size.y));
        const SDL_FPoint size{authored_size.x * resized_scale,
                              authored_size.y * resized_scale};
        const SDL_FPoint center{window.x * anchor_center_normalized.x,
                                window.y * anchor_center_normalized.y};
        Placement result = *this;
        result.dst = {std::clamp(center.x - size.x * 0.5F,
                                 0.0F,
                                 std::max(0.0F, window.x - size.x)),
                      std::clamp(center.y - size.y * 0.5F,
                                 0.0F,
                                 std::max(0.0F, window.y - size.y)),
                      size.x,
                      size.y};
        result.scale = resized_scale;
        result.authored_size = authored_size;
        result.containing_size = window;
        result.rule = Rule::centered;
        return result;
      }
      const Placement background = PlaceCenteredIn(
          anchor_bounds, anchor_authored_size, anchor_requested_scale);
      return PlaceCenteredIn(background, authored_size, requested_scale);
    }
    return PlaceCenteredIn(anchor_bounds, authored_size, requested_scale);
  }
  return *this;
}

} // namespace evnova::util

// Keep the short names available to game code while the implementation lives
// in the shared utility namespace.
using evnova::util::PlaceCenteredIn;
using evnova::util::PlaceContained;
using evnova::util::Placement;
using evnova::util::PlaceWindow;
