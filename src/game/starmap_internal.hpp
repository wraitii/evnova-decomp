#pragma once

#include "starmap.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace game::starmap_detail {

struct StarmapGeometry {
  SDL_FRect window{};
  SDL_FRect map{};
  SDL_FRect side{};
  SDL_FRect bar{};
  std::array<SDL_FRect, 6> buttons{};
};

// The original projects world -> panel with
//   screen = panel_centre + (world - pan_origin) / zoom
// (projection in NovaUi_DrawStarmapRoutesAndMarkers / the click hit-test).
// pan_origin is the world point under the panel centre; zoom changes never
// touch it, so zooming pivots about the panel centre.
inline constexpr float kZoomInitial = 0.5625F;

struct MapView {
  float zoom = kZoomInitial;
  float pan_x = 0.0F;
  float pan_y = 0.0F;

  [[nodiscard]] SDL_FPoint
  Project(float panel_w, float panel_h, float world_x, float world_y) const {
    return SDL_FPoint{std::round(panel_w * 0.5F + (world_x - pan_x) / zoom),
                      std::round(panel_h * 0.5F + (world_y - pan_y) / zoom)};
  }

  [[nodiscard]] float UnprojectX(float panel_w, float screen_x) const {
    return (screen_x - panel_w * 0.5F) * zoom + pan_x;
  }

  [[nodiscard]] float UnprojectY(float panel_h, float screen_y) const {
    return (screen_y - panel_h * 0.5F) * zoom + pan_y;
  }
};

struct MappedSystem {
  std::int16_t zero_based_id = -1;
  float sx = 0.0F;
  float sy = 0.0F;
};

struct PoliticalOverlay {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> rgba;
};

[[nodiscard]] bool SystemOnMap(const GameState &state,
                               std::int16_t zero_based_id);

[[nodiscard]] std::vector<MappedSystem> BuildMappedSystems(
    const GameState &state, const MapView &view, const SDL_FRect &panel);

[[nodiscard]] PoliticalOverlay BuildPoliticalOverlay(const GameState &state,
                                                     const MapView &view,
                                                     const SDL_FRect &panel);

void DrawGalaxy(SdlPlatform &platform,
                NovaFontCache &font_cache,
                const GameState &state,
                const std::vector<MappedSystem> &mapped,
                const MapView &view,
                const StarmapGeometry &geometry,
                std::int16_t selected_id,
                const PoliticalOverlay *overlay,
                const std::vector<std::int16_t> &mission_targets,
                const NovaStarmap_MarkerIcons &icons,
                float alpha = 1.0F);

} // namespace game::starmap_detail
