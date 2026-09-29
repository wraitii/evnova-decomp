#pragma once

#include "starmap.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
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

// Hypergate destination-selection mode (Ghidra g_starmap_hypergate_mode /
// g_starmap_hypergate_source_stellar_id, armed around the map by
// Stellar_EnterHypergate 0x00456480). `linked_systems` holds the source
// gate's HyperLink1-8 targets resolved through
// System_ResolveVisibleSystemForTravel (the click gate, header, spokes and the
// exit check all compare against this). `cycle_systems` is the Tab cycle's
// list, which the original builds from System_FindSystemContainingStellar
// alone, without the visibility resolve (key filter 0x004a7710).
struct HypergateMapMode {
  std::int16_t source_stellar_id = -1;
  std::vector<std::int16_t> linked_systems;
  std::vector<std::int16_t> cycle_systems;

  [[nodiscard]] bool Links(std::int16_t zero_based_id) const {
    return zero_based_id >= 0 &&
           std::find(linked_systems.begin(),
                     linked_systems.end(),
                     zero_based_id) != linked_systems.end();
  }
};

// Builds the hypergate-mode link tables from the source gate's HyperLink1-8
// (StellarDef +0x47e). Each target's system comes from its stored system_id,
// falling back to System_FindSystemContainingStellar when that is out of
// range; `cycle_systems` uses System_FindSystemContainingStellar alone.
[[nodiscard]] HypergateMapMode
BuildHypergateMapMode(const GameState &state, std::int16_t source_stellar_id);

struct PoliticalOverlay {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> rgba;
};

[[nodiscard]] bool SystemOnMap(const GameState &state,
                               std::int16_t zero_based_id);

// Ghidra 0x004aab30 search helpers: normalize a name to lowercase [a-z0-9], and
// pick the best visible+visited name prefix match (-1 when nothing qualifies).
[[nodiscard]] std::string NormalizeSearchName(std::string_view text);

[[nodiscard]] std::int16_t FindBestSystemMatch(const GameState &state,
                                               std::string_view query);

[[nodiscard]] std::vector<MappedSystem> BuildMappedSystems(
    const GameState &state, const MapView &view, const SDL_FRect &panel);

[[nodiscard]] PoliticalOverlay BuildPoliticalOverlay(const GameState &state,
                                                     const MapView &view,
                                                     const SDL_FRect &panel);

// Ghidra 0x004aa620 NovaUi_DrawStarmapPoliticalOverlay: uploads the built
// overlay as a texture and blits it over the panel. The caller draws it
// before the nebula pass so the original compositing order is preserved.
void DrawPoliticalOverlay(SdlPlatform &platform,
                          const PoliticalOverlay &overlay,
                          const SDL_FRect &panel);

// Chooses the nebula zoom-tier image for a projected destination rect.
// `tier_w`/`tier_h` are the per-tier pixel dimensions in the original's
// ascending-size order (a zero dimension means the PICT is absent). Returns
// the selected index, or -1 when no tier image is available.
//
// Ghidra 0x004a5560 (inside NovaUi_RedrawStarmapWindow 0x004a51f0): the
// original accepts the first tier that covers the destination in *either*
// dimension. BUGFIX(original) (`safe`): require both dimensions so a
// destination whose aspect ratio differs from the (square) PICTs picks the
// slightly-oversized image and scales it down instead of stretching a
// too-small one up; falls back to the largest available tier as the original
// does when no image covers the destination.
[[nodiscard]] int ChooseNebulaTier(std::span<const float> tier_w,
                                   std::span<const float> tier_h,
                                   float dst_w,
                                   float dst_h,
                                   bool safe);

void DrawGalaxy(SdlPlatform &platform,
                NovaFontCache &font_cache,
                const GameState &state,
                const std::vector<MappedSystem> &mapped,
                const MapView &view,
                const StarmapGeometry &geometry,
                std::int16_t selected_id,
                const std::vector<std::int16_t> &mission_targets,
                const NovaStarmap_MarkerIcons &icons,
                float alpha = 1.0F,
                const HypergateMapMode *hypergate = nullptr);

} // namespace game::starmap_detail
