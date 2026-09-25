#include "starmap_internal.hpp"

#include "../log.hpp"
#include "nova_font.hpp"
#include "targeting.hpp"
#include "travel.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

namespace game::starmap_detail {
namespace {

constexpr SDL_Color kColorBlack{0, 0, 0, 255};
constexpr SDL_Color kColorWhite{255, 255, 255, 255};
constexpr SDL_Color kColorCyan{0, 255, 255, 255};
constexpr SDL_Color kLinkGrey{78, 78, 78, 255};
constexpr SDL_Color kRouteGreen{0, 255, 0, 255};
constexpr SDL_Color kJumpGreen{0, 153, 0, 255};
constexpr SDL_Color kDestBlue{0, 0, 255, 255};
constexpr SDL_Color kDestOrange{255, 102, 0, 255};
constexpr SDL_Color kDestRed{255, 0, 0, 255};
constexpr SDL_Color kHeaderGrey{192, 192, 192, 255};
constexpr SDL_Color kTextGrey{64, 64, 64, 255};

constexpr double kZoomInLimit = 0.5;
constexpr double kLabelZoomThreshold = 1.1;
constexpr float kMarkerInset = 4.0F;
constexpr float kMarkerInsetZoomedIn = 6.0F;
constexpr float kCurrentDotInset = 2.0F;
constexpr float kCurrentDotInsetZoomedIn = 3.0F;
constexpr float kReticleInset = 6.0F;
constexpr float kReticleInsetZoomedIn = 8.0F;
constexpr float kLabelOffsetX = 7.0F;
constexpr float kLabelOffsetY = 4.0F;
constexpr double kOverlayRadiusSmall = 11.0;
constexpr double kOverlayRadiusLarge = 22.0;
constexpr double kOverlayFadeSmall = 0.4;
constexpr double kOverlayFadeLarge = 0.2;

} // namespace

namespace {

// Whether the pilot has VISITED `zero_based_id` (SystemDef.discovery_state,
// the persisted fog record). Matches the original's map gates, which also
// require the system to pass its Visibility NCB (is_visible) -- an invisible
// story twin never shows even if its fog slot holds a stale visit.
bool SystemVisited(const GameState &state, std::int16_t zero_based_id) {
  if (zero_based_id < 0 || static_cast<std::size_t>(zero_based_id) >=
                               state.scenario.systems.size()) {
    return false;
  }
  const System &sys =
      state.scenario.systems[static_cast<std::size_t>(zero_based_id)];
  return sys.is_visible && sys.has_explored_flag && sys.discovery_state > 0;
}

} // namespace

// Whether the system appears on the map at all: the marker pass's outer gate
// is is_visible && has_explored_flag (0x004a8100), then visited, or latched by
// the last discovery rebuild (the original's marker latch also accepts
// discovered_this_rebuild -- one jump ahead shows as a marker).
bool SystemOnMap(const GameState &state, std::int16_t zero_based_id) {
  if (zero_based_id < 0 || static_cast<std::size_t>(zero_based_id) >=
                               state.scenario.systems.size()) {
    return false;
  }
  const System &sys =
      state.scenario.systems[static_cast<std::size_t>(zero_based_id)];
  if (!sys.is_visible || !sys.has_explored_flag) {
    return false;
  }
  return sys.discovery_state > 0 || sys.discovered_this_rebuild;
}

// Ghidra 0x004aab30 NovaUi_RunStarmapSearchDialog, normalization pass:
// MWRuntime_FUN_004d6230 lower-cases each character and the search keeps only
// the survivors in [a-z0-9], so "New Boston" normalizes to "newboston".
std::string NormalizeSearchName(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const unsigned char c : text) {
    const unsigned char lower = static_cast<unsigned char>(std::tolower(c));
    if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9')) {
      out.push_back(static_cast<char>(lower));
    }
  }
  return out;
}

// Ghidra 0x004aab30 scoring pass. Only visible, already-visited systems latched
// by the last discovery rebuild are candidates (is_visible &&
// discovery_state > 0 && discovered_this_rebuild). The winner shares the
// longest normalized leading prefix with the query; a tie goes to the shorter
// normalized name. A one-character match is accepted only when it is the sole
// candidate. Returns -1 when nothing qualifies.
std::int16_t FindBestSystemMatch(const GameState &state,
                                 std::string_view query) {
  const std::string wanted = NormalizeSearchName(query);
  if (wanted.empty()) {
    return -1;
  }
  std::int16_t best_id = -1;
  std::size_t best_match = 0;
  std::size_t best_name_len = 0;
  std::size_t candidate_count = 0;
  for (std::size_t i = 0; i < state.scenario.systems.size(); ++i) {
    const System &sys = state.scenario.systems[i];
    if (!sys.is_visible || sys.discovery_state <= 0 ||
        !sys.discovered_this_rebuild) {
      continue;
    }
    const std::string name = NormalizeSearchName(sys.name);
    const std::size_t limit = std::min(wanted.size(), name.size());
    std::size_t matched = 0;
    while (matched < limit && wanted[matched] == name[matched]) {
      ++matched;
    }
    if (matched == 0) {
      continue;
    }
    ++candidate_count;
    if (best_id < 0 || matched > best_match ||
        (matched == best_match && name.size() < best_name_len)) {
      best_id = static_cast<std::int16_t>(i);
      best_match = matched;
      best_name_len = name.size();
    }
  }
  if (best_id < 0 || (best_match < 2 && candidate_count != 1)) {
    return -1;
  }
  return best_id;
}

namespace {

// @port 0x00466260 85% rendering
// Ghidra 0x00466260 Stellar_ComputeStellarDisplayColor: the starmap marker
// ring colour for a system, graded from its destinations: unvisited -> grey
// 0xc000-header? no -- grey 0x4000; no usable destination -> grey 0xc000; any
// hazardous destination -> green; any government-policy or reputation-gated
// destination -> blue; any plain destination with non-negative system
// reputation -> orange (0xffff,0x6666,0); only negative-reputation ones ->
// red. The reputation gate reproduces the binary's branch polarities exactly
// (0x004663f4: threshold 0x7fff or rep < threshold with threshold > -0x7fff
// classifies by reputation sign, otherwise the destination counts blocked).
// NOTE: this polarity is the map's own; Stellar_GetStellarRadarColor
// (0x00466030) shares the comparison but paints the gate-passed case yellow.
SDL_Color StellarDisplayColor(const GameState &state,
                              std::int16_t zero_based_id) {
  if (zero_based_id < 0 || static_cast<std::size_t>(zero_based_id) >=
                               state.scenario.systems.size()) {
    return kHeaderGrey;
  }
  const System &sys =
      state.scenario.systems[static_cast<std::size_t>(zero_based_id)];
  if (sys.discovery_state < 1) {
    return kTextGrey;
  }
  if (!NovaSystem_HasUsableTravelDestination(state, zero_based_id)) {
    return kHeaderGrey;
  }
  const std::int16_t rep =
      zero_based_id < static_cast<std::int16_t>(state.system_reputation.size())
          ? state.system_reputation[static_cast<std::size_t>(zero_based_id)]
          : 0;
  int blocked = 0;
  int hazard = 0;
  int friendly = 0;
  int hostile = 0;
  for (const std::int16_t nav : sys.nav_defs) {
    if (nav < 0x80) {
      continue;
    }
    const Stellar *st = state.scenario.Stellar(nav);
    if (st == nullptr || !st->is_available ||
        (st->availability_flags & 0x3000U) != 0U || (st->flags & 0x20U) != 0U ||
        !NovaTargeting_StellarTargetsSpriteSetActive(*st)) {
      continue;
    }
    if (st->dominated) {
      ++hazard;
      continue;
    }
    const bool policy_blocked =
        st->government_id >= 0 && st->government_id < 0x100 &&
        state.scenario.governments[static_cast<std::size_t>(st->government_id)]
                .policy_flags[1] != 0;
    const std::int16_t threshold = st->min_status;
    bool classify_by_rep =
        threshold == 0x7fff || (rep < threshold && threshold > -0x7fff);
    if (!policy_blocked && classify_by_rep) {
      if (rep < 0) {
        ++hostile;
      } else {
        ++friendly;
      }
    } else {
      ++blocked;
    }
  }
  if (hazard >= 1) {
    return kRouteGreen;
  }
  if (blocked >= 1) {
    return kDestBlue;
  }
  if (friendly >= 1) {
    return kDestOrange;
  }
  if (hostile >= 1) {
    return kDestRed;
  }
  return kHeaderGrey;
}

// ---- Draw primitives ------------------------------------------------------

void DrawThickLine(SDL_Renderer *renderer,
                   SDL_FPoint a,
                   SDL_FPoint b,
                   float thickness,
                   SDL_Color tint) {
  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  const float len = std::max(0.0001F, std::sqrt(dx * dx + dy * dy));
  const float nx = -dy / len;
  const float ny = dx / len;
  const float h = thickness * 0.5F;
  const SDL_FColor c{static_cast<float>(tint.r) / 255.0F,
                     static_cast<float>(tint.g) / 255.0F,
                     static_cast<float>(tint.b) / 255.0F,
                     static_cast<float>(tint.a) / 255.0F};
  const SDL_FPoint corners[4]{{a.x + nx * h, a.y + ny * h},
                              {b.x + nx * h, b.y + ny * h},
                              {b.x - nx * h, b.y - ny * h},
                              {a.x - nx * h, a.y - ny * h}};
  SDL_Vertex verts[6];
  const int idx[6] = {0, 1, 2, 0, 2, 3};
  for (int i = 0; i < 6; ++i) {
    const auto &p = corners[static_cast<std::size_t>(idx[i])];
    verts[i] = SDL_Vertex{SDL_FPoint{p.x, p.y}, c, SDL_FPoint{0.0F, 0.0F}};
  }
  SDL_RenderGeometry(renderer, nullptr, verts, 6, nullptr, 0);
}

// Filled disc (FUN_004bb260's midpoint-circle fill rasterizer).
void DrawDisc(
    SDL_Renderer *renderer, float cx, float cy, float r, SDL_Color tint) {
  if (r <= 0.0F) {
    return;
  }
  constexpr int kSegments = 20;
  const SDL_FColor c{static_cast<float>(tint.r) / 255.0F,
                     static_cast<float>(tint.g) / 255.0F,
                     static_cast<float>(tint.b) / 255.0F,
                     static_cast<float>(tint.a) / 255.0F};
  std::vector<SDL_Vertex> verts;
  verts.reserve(static_cast<std::size_t>(kSegments) * 3u);
  for (int s = 0; s < kSegments; ++s) {
    const float a0 = static_cast<float>(s) *
                     static_cast<float>(2.0 * std::numbers::pi) /
                     static_cast<float>(kSegments);
    const float a1 = static_cast<float>(s + 1) *
                     static_cast<float>(2.0 * std::numbers::pi) /
                     static_cast<float>(kSegments);
    verts.push_back(SDL_Vertex{SDL_FPoint{cx, cy}, c, SDL_FPoint{0.0F, 0.0F}});
    verts.push_back(
        SDL_Vertex{SDL_FPoint{cx + r * std::cos(a0), cy + r * std::sin(a0)},
                   c,
                   SDL_FPoint{0.0F, 0.0F}});
    verts.push_back(
        SDL_Vertex{SDL_FPoint{cx + r * std::cos(a1), cy + r * std::sin(a1)},
                   c,
                   SDL_FPoint{0.0F, 0.0F}});
  }
  SDL_RenderGeometry(renderer,
                     nullptr,
                     verts.data(),
                     static_cast<int>(verts.size()),
                     nullptr,
                     0);
}

// 1px ring (DrawContext_DrawCircleInRect 0x004ba350's midpoint outline).
// The 1px/2px ring around a marker: the original sets the pen to (2,2) for
// the normal tier and (1,1) at the innermost zoom step before
// DrawCircleInRect (0x004a8f8b / 0x004a8fa1), so the ring is 2px thick except
// when zoomed fully in. Drawn as two offset rings for the thick variant.
void DrawRing(SDL_Renderer *renderer,
              float cx,
              float cy,
              float r,
              SDL_Color tint,
              float thickness = 1.0F) {
  constexpr int kSegments = 24;
  std::array<SDL_FPoint, kSegments + 1> pts{};
  for (int s = 0; s <= kSegments; ++s) {
    const float ang = static_cast<float>(s) *
                      static_cast<float>(2.0 * std::numbers::pi) /
                      static_cast<float>(kSegments);
    pts[static_cast<std::size_t>(s)] =
        SDL_FPoint{cx + r * std::cos(ang), cy + r * std::sin(ang)};
  }
  SDL_SetRenderDrawColor(renderer, tint.r, tint.g, tint.b, SDL_ALPHA_OPAQUE);
  SDL_RenderLines(renderer, pts.data(), kSegments + 1);
  if (thickness > 1.5F) {
    for (int s = 0; s <= kSegments; ++s) {
      const float ang = static_cast<float>(s) *
                        static_cast<float>(2.0 * std::numbers::pi) /
                        static_cast<float>(kSegments);
      pts[static_cast<std::size_t>(s)] = SDL_FPoint{
          cx + (r - 1.0F) * std::cos(ang), cy + (r - 1.0F) * std::sin(ang)};
    }
    SDL_RenderLines(renderer, pts.data(), kSegments + 1);
  }
}

// The selected-system reticle: 8 disjoint green 4px corner ticks inset 1px
// from the corners of the reticle rect (NovaUi_RedrawStarmapWindow 0x004a5a40
// block, colour SHORT_ARRAY_00733b32). The ticks are NOT joined: the original
// draws eight independent SetCursorPos/DrawLineTo pairs, so the result is a
// partial square (corner brackets with gaps at the edge midpoints), never a
// cross inside.
void DrawReticle(SDL_Renderer *renderer,
                 float cx,
                 float cy,
                 float half,
                 float alpha = 1.0F) {
  SDL_SetRenderDrawColor(
      renderer,
      kRouteGreen.r,
      kRouteGreen.g,
      kRouteGreen.b,
      static_cast<std::uint8_t>(static_cast<float>(SDL_ALPHA_OPAQUE) * alpha));
  const float l = cx - half;
  const float r = cx + half;
  const float t = cy - half;
  const float b = cy + half;
  const SDL_FPoint segs[16]{
      {l + 1, t},
      {l + 4, t},
      {r - 4, t},
      {r - 1, t}, // top edge
      {l + 1, b},
      {l + 4, b},
      {r - 4, b},
      {r - 1, b}, // bottom edge
      {l, t + 1},
      {l, t + 4},
      {l, b - 4},
      {l, b - 1}, // left edge
      {r, t + 1},
      {r, t + 4},
      {r, b - 4},
      {r, b - 1}, // right edge
  };
  for (int i = 0; i < 16; i += 2) {
    SDL_RenderLine(
        renderer, segs[i].x, segs[i].y, segs[i + 1].x, segs[i + 1].y);
  }
}
} // namespace

// ---- Political overlay -----------------------------------------------------
// Ghidra NovaUi_BuildStarmapPoliticalOverlay 0x004a9d50 /
// NovaUi_PaintStarmapGovDisc 0x004aa070 / NovaUi_BlendStarmapOverlayCell
// 0x004aa2f3 / NovaUi_DrawStarmapPoliticalOverlay 0x004aa620.
//
// Eligibility per system: visited, latched this rebuild, valid government with
// scan_mask bit 2 (0x4) clear, and a usable travel destination. Disc radius is
// round(22/zoom)+12 half-pixel cells (round(11/zoom)+9 for scan_mask bit-1
// governments); per-cell strength is clamp((r^2-d^2) * fade * zoom, 1, 255)
// with fade 0.2 (0.4 small tier); the final 16-bit colour component is the
// government theme byte * strength * 2.0 (0x004aa850) drawn opaque -- i.e. a
// fade to black over the black map, which equals alpha = strength*2 here.
//
// BUGFIX(original): the executable clears and paints a fixed 512-wide,
// 0x35c-tall half-pixel buffer (0x004a9d50), so a galaxy layout larger than
// that map area has its government territory clipped. Confirmed engine bug
// (docs/known_original_bugs.md). The port allocates the buffer to the actual
// starmap panel instead, so custom layouts are not clipped. Not routed
// through kApplyOriginalBugFixes: the fixed buffer is a software-surface
// artifact of the original renderer, not reproducible gameplay state.
PoliticalOverlay BuildPoliticalOverlay(const GameState &state,
                                       const MapView &view,
                                       const SDL_FRect &panel) {
  PoliticalOverlay out;
  out.width = std::max(1, static_cast<int>(panel.w));
  out.height = std::max(1, static_cast<int>(panel.h));
  out.rgba.assign(static_cast<std::size_t>(out.width) *
                      static_cast<std::size_t>(out.height) * 4u,
                  0U);
  std::vector<std::uint16_t> strength(static_cast<std::size_t>(out.width) *
                                          static_cast<std::size_t>(out.height),
                                      0);
  std::vector<std::int16_t> gov(static_cast<std::size_t>(out.width) *
                                    static_cast<std::size_t>(out.height),
                                -1);

  for (std::size_t i = 0; i < state.scenario.systems.size(); ++i) {
    const auto id = static_cast<std::int16_t>(i);
    const System &sys = state.scenario.systems[i];
    // Ghidra 0x004aa070 eligibility gate.
    if (sys.discovery_state <= 0 || !sys.discovered_this_rebuild ||
        sys.government_id < 0) {
      continue;
    }
    const Government *gv = state.scenario.Government(
        static_cast<std::int16_t>(sys.government_id + 0x80));
    if (gv == nullptr || (gv->flags_secondary & 0x0004U) != 0U) {
      continue;
    }
    if (!NovaSystem_HasUsableTravelDestination(state, id)) {
      continue;
    }
    const bool small_tier = (gv->flags_secondary & 0x0002U) != 0U;
    // Radius in half-pixel grid UNITS (1 unit = 2 screen px; a cell of 8
    // units is the original's 16px paint block): round(22/zoom)+12 units, or
    // round(11/zoom)+9 for the small tier. At the default zoom the large tier
    // spans ~102px, so neighbouring discs merge into territory blobs
    // (0x004aa070 / 0x004aa2f3).
    const double radius_units =
        std::round((small_tier ? kOverlayRadiusSmall : kOverlayRadiusLarge) /
                   static_cast<double>(view.zoom)) +
        (small_tier ? 9.0 : 12.0);
    const double r_px = radius_units * 2.0;
    if (r_px < 0.5) {
      continue;
    }
    // Ghidra 0x004A9BF0 NovaUi_ProjectSystemToOverlayGrid runs inline here.
    const SDL_FPoint centre = view.Project(panel.w,
                                           panel.h,
                                           static_cast<float>(sys.pos_x),
                                           static_cast<float>(sys.pos_y));
    // Project() is panel-relative and the overlay texture is blitted at the
    // panel origin, so the disc centre needs no further offset.
    const double cx = centre.x;
    const double cy = centre.y;
    const double fade = small_tier ? kOverlayFadeSmall : kOverlayFadeLarge;
    const double r2 = radius_units * radius_units;
    const int bx0 = std::max(0, static_cast<int>(std::floor(cx - r_px)));
    const int by0 = std::max(0, static_cast<int>(std::floor(cy - r_px)));
    const int bx1 =
        std::min(out.width, static_cast<int>(std::ceil(cx + r_px)) + 1);
    const int by1 =
        std::min(out.height, static_cast<int>(std::ceil(cy + r_px)) + 1);
    // Ghidra 0x004AA25E NovaUi_PaintStarmapDiscRowLoop and 0x004AA2DA
    // NovaUi_PaintStarmapDiscCellStep run inline in these two loops.
    for (int py = by0; py < by1; ++py) {
      for (int px = bx0; px < bx1; ++px) {
        // Distance in grid units (1 unit = 2 screen px).
        const double d_units = 0.5 * std::sqrt(std::pow(px + 0.5 - cx, 2.0) +
                                               std::pow(py + 0.5 - cy, 2.0));
        const double raw =
            (r2 - d_units * d_units) * fade * static_cast<double>(view.zoom);
        if (raw < 1.0) {
          continue;
        }
        const auto s = static_cast<std::uint16_t>(std::clamp(raw, 1.0, 255.0));
        const std::size_t idx =
            static_cast<std::size_t>(py) * static_cast<std::size_t>(out.width) +
            static_cast<std::size_t>(px);
        if (s > strength[idx]) {
          strength[idx] = s;
          gov[idx] = sys.government_id;
        }
      }
    }
  }

  for (int py = 0; py < out.height; ++py) {
    for (int px = 0; px < out.width; ++px) {
      const std::size_t idx =
          static_cast<std::size_t>(py) * static_cast<std::size_t>(out.width) +
          static_cast<std::size_t>(px);
      if (strength[idx] == 0U) {
        continue;
      }
      const Government *gv =
          state.scenario.Government(static_cast<std::int16_t>(gov[idx] + 0x80));
      if (gv == nullptr) {
        continue;
      }
      // 0x004aa620 computes 16-bit components theme * strength * 0.5
      // (_DAT_00575a00 = 0.5) and passes them to DrawContext_SetRgbColor,
      // which converts the classic 16-bit RGBColor to 8-bit by >>8 - so the
      // painted colour is theme * strength / 512 (peaking at HALF the theme
      // brightness). Zero-strength pixels stay transparent.
      const double k = static_cast<double>(strength[idx]) / 512.0;
      const std::size_t o = idx * 4u;
      out.rgba[o + 0] = static_cast<std::uint8_t>(
          std::min(255.0, static_cast<double>(gv->theme_red) * k));
      out.rgba[o + 1] = static_cast<std::uint8_t>(
          std::min(255.0, static_cast<double>(gv->theme_green) * k));
      out.rgba[o + 2] = static_cast<std::uint8_t>(
          std::min(255.0, static_cast<double>(gv->theme_blue) * k));
      out.rgba[o + 3] = 255;
    }
  }
  return out;
}

namespace {

void DrawPoliticalOverlay(SdlPlatform &platform,
                          const PoliticalOverlay &overlay,
                          const SDL_FRect &panel) {
  SDL_Renderer *renderer = platform.renderer();
  auto texture =
      SdlTexture::Create(renderer, overlay.width, overlay.height, overlay.rgba);
  if (!texture) {
    NovaLog::Warn("political overlay texture upload failed: {}",
                  SDL_GetError());
    return;
  }
  const SDL_FRect dst{panel.x,
                      panel.y,
                      static_cast<float>(overlay.width),
                      static_cast<float>(overlay.height)};
  SDL_RenderTexture(renderer, texture->get(), nullptr, &dst);
}

} // namespace

// ---- Galaxy graph ----------------------------------------------------------

// Builds the projected marker list for the current view.
std::vector<MappedSystem> BuildMappedSystems(const GameState &state,
                                             const MapView &view,
                                             const SDL_FRect &panel) {
  std::vector<MappedSystem> out;
  out.reserve(state.scenario.systems.size());
  for (std::size_t i = 0; i < state.scenario.systems.size(); ++i) {
    const System &sys = state.scenario.systems[i];
    const SDL_FPoint p = view.Project(panel.w,
                                      panel.h,
                                      static_cast<float>(sys.pos_x),
                                      static_cast<float>(sys.pos_y));
    out.push_back(MappedSystem{
        static_cast<std::int16_t>(i), panel.x + p.x, panel.y + p.y});
  }
  return out;
}

// Ghidra 0x004a8100 NovaUi_DrawStarmapRoutesAndMarkers (+ the nebula and
// overlay passes of 0x004a51f0 that precede it). Draw order: political
// overlay, plotted-route chain (green), adjacency links from visited systems
// (grey; links radiate into unrevealed space because the target needs no
// discovery gate), the committed-jump accent (dark green), then markers
// (background disc + status ring), the mission-target arrows and the selected
// marker icon, the current-system cyan dot, the selection reticle and finally
// the white system labels.
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
                float alpha) {
  const auto &systems = state.scenario.systems;
  const std::int16_t current = state.player.current_system_id;
  SDL_Renderer *renderer = platform.renderer();
  // Route-map overlay fade (Ghidra 0x00439bd0 blit tint): multiplies every
  // element colour; 1.0 = the opaque starmap-window rendering.
  const auto tint = [alpha](SDL_Color c) {
    if (alpha < 1.0F) {
      c.a = static_cast<std::uint8_t>(static_cast<float>(c.a) * alpha);
    }
    return c;
  };

  const SDL_Rect clip{static_cast<int>(geometry.map.x),
                      static_cast<int>(geometry.map.y),
                      static_cast<int>(geometry.map.w),
                      static_cast<int>(geometry.map.h)};
  SDL_SetRenderClipRect(renderer, &clip);
  const auto restore_clip = [&]() { SDL_SetRenderClipRect(renderer, nullptr); };

  if (overlay != nullptr) {
    DrawPoliticalOverlay(platform, *overlay, geometry.map);
  }

  const auto find_mapped = [&](std::int16_t id) -> const MappedSystem * {
    if (id < 0 || static_cast<std::size_t>(id) >= mapped.size()) {
      return nullptr;
    }
    return &mapped[static_cast<std::size_t>(id)];
  };

  // Plotted-route chain (route pass of 0x004a8100; green, 2px).
  if (NovaStarmap_RouteHasHops(state)) {
    const auto &route = state.travel.starmap_route;
    for (std::size_t i = 0; i + 1 < route.size(); ++i) {
      const std::int16_t a = route[i];
      const std::int16_t b = route[i + 1];
      if (a == -1 || b == -1) {
        break;
      }
      const MappedSystem *ma = find_mapped(a);
      const MappedSystem *mb = find_mapped(b);
      if (ma == nullptr || mb == nullptr) {
        continue;
      }
      DrawThickLine(renderer,
                    SDL_FPoint{ma->sx, ma->sy},
                    SDL_FPoint{mb->sx, mb->sy},
                    2.0F,
                    tint(kRouteGreen));
    }
  }

  // Adjacency links (link pass of 0x004a8100): only systems that are VISITED
  // (is_visible + discovery_state > 0 + discovered_this_rebuild latch; the
  // rebuild always latches visited systems) radiate lines, deduplicated by
  // marking each drawn source (g_starmap_system_highlight_flags). Links go to
  // every travel-resolvable neighbour -- no discovery gate on the target, so
  // lines radiate one hop into unrevealed space from each visited system.
  std::vector<std::uint8_t> drawn(systems.size(), 0);
  const auto draw_link = [&](std::int16_t a, std::int16_t b, SDL_Color color) {
    const MappedSystem *ma = find_mapped(a);
    const MappedSystem *mb = find_mapped(b);
    if (ma == nullptr || mb == nullptr) {
      return;
    }
    DrawThickLine(renderer,
                  SDL_FPoint{ma->sx, ma->sy},
                  SDL_FPoint{mb->sx, mb->sy},
                  1.0F,
                  tint(color));
  };
  for (std::size_t i = 0; i < systems.size(); ++i) {
    const System &sys = systems[i];
    if (drawn[i] != 0 || !SystemVisited(state, static_cast<std::int16_t>(i))) {
      continue;
    }
    for (const std::int16_t link : sys.links) {
      if (link < 0x80) {
        continue;
      }
      // The original resolves the far end with System_ResolveVisibleSystemFor-
      // Travel (0x0046b920): the line only draws to a system whose Visibility
      // NCB currently holds (an invisible twin group ends the line), and the
      // loader's link normalization (0x004bd3c0) already points every Con at
      // the target's visibility root. Visitation does NOT gate the target, so
      // lines still reach unvisited latched neighbours.
      const std::int16_t target = NovaSystem_ResolveVisibleForTravel(
          state, static_cast<std::int16_t>(link - 0x80));
      if (target < 0) {
        continue;
      }
      // The committed-jump slot of the current system draws dark green
      // (DAT_00733b38) while a plotted jump is armed (travel_transfer_mode
      // == 3, 0x004a8100 line-225 gate); everything else grey
      // (DAT_00733b62).
      const bool active_jump =
          state.player.travel_transfer_mode == 3 &&
          static_cast<std::int16_t>(i) == current &&
          state.travel.travel_slot >= 0 &&
          state.travel.travel_slot <
              static_cast<std::int16_t>(sys.links.size()) &&
          sys.links[static_cast<std::size_t>(state.travel.travel_slot)] == link;
      draw_link(static_cast<std::int16_t>(i),
                target,
                active_jump ? kJumpGreen : kLinkGrey);
    }
    drawn[i] = 1;
  }

  // Plotted-jump accent line (0x004a8100 line-290 pass): while a jump is
  // armed (travel_transfer_mode == 3), one thick dark-green line runs from
  // the current system to the travel-resolvable destination of the armed
  // slot. Slot-driven, NOT selection-driven, and absent whenever the mode
  // latch is not 3 (cleared implicitly by every disarm path).
  if (state.player.travel_transfer_mode == 3 && current >= 0 &&
      static_cast<std::size_t>(current) < systems.size()) {
    const std::int16_t slot = state.travel.travel_slot;
    const System &cur_sys = systems[static_cast<std::size_t>(current)];
    if (slot >= 0 && slot < static_cast<std::int16_t>(cur_sys.links.size()) &&
        cur_sys.links[static_cast<std::size_t>(slot)] >= 0x80) {
      const std::int16_t dest = NovaSystem_ResolveVisibleForTravel(
          state,
          static_cast<std::int16_t>(
              cur_sys.links[static_cast<std::size_t>(slot)] - 0x80));
      if (dest >= 0) {
        draw_link(current, dest, kJumpGreen);
      }
    }
  }

  // Markers (marker pass of 0x004a8100): the disc + ring draws ONLY for
  // systems the marker latch accepts -- the current system, any visited
  // (discovery_state > 0) system, or one latched by the last discovery
  // rebuild (visited systems plus their one-hop neighbours; the latch alone
  // covers the visited case). Mission targets and the selected system draw
  // their marker regardless (the original's bVar4 / auStack_14[2] overrides).
  // Ring colour grades per StellarDisplayColor, so merely-revealed neighbours
  // (discovery_state < 1) read as dim grey rings while visited ones are
  // blue/orange/green/red.
  const float marker_inset = static_cast<double>(view.zoom) < kZoomInLimit
                                 ? kMarkerInsetZoomedIn
                                 : kMarkerInset;
  const float dot_inset = static_cast<double>(view.zoom) < kZoomInLimit
                              ? kCurrentDotInsetZoomedIn
                              : kCurrentDotInset;
  for (const MappedSystem &m : mapped) {
    // Outer marker-pass gate (0x004a8100): is_visible && has_explored_flag.
    // Invisible story twins never draw a marker, ring or arrow.
    const System &sys = systems[static_cast<std::size_t>(m.zero_based_id)];
    if (!sys.is_visible || !sys.has_explored_flag) {
      continue;
    }
    // Mission targets compare through the discovery slot (0x004a8ec2 resolves
    // both ends with System_ResolveSystemDiscoverySlot), so a twin group
    // matches whichever member holds the mission locator. Selection compares
    // through the discovery slots the same way (0x004a8f0a).
    const std::int16_t slot =
        NovaSystem_ResolveDiscoverySlot(state, m.zero_based_id);
    const bool is_mission_target =
        std::any_of(mission_targets.begin(),
                    mission_targets.end(),
                    [&](std::int16_t target) {
                      return target != -1 && NovaSystem_ResolveDiscoverySlot(
                                                 state, target) == slot;
                    });
    const bool is_selected =
        selected_id >= 0 &&
        slot == NovaSystem_ResolveDiscoverySlot(state, selected_id);
    if (!SystemOnMap(state, m.zero_based_id) && !is_selected &&
        !is_mission_target) {
      continue;
    }
    const float r = marker_inset;
    DrawDisc(renderer, m.sx, m.sy, r, tint(kColorBlack));
    DrawRing(renderer,
             m.sx,
             m.sy,
             r,
             tint(StellarDisplayColor(state, m.zero_based_id)),
             static_cast<double>(view.zoom) < kZoomInLimit ? 2.0F : 1.0F);

    // Mission-target arrow (CICN 0x3a98): a 16px box up-left of the marker
    // (0x004a8e5c), pointing down-right at the node. The original suppresses
    // it on the selected system (override cleared when the selected slot
    // matches and the briefing latch DAT_007dc745 is clear, 0x004a8f0a).
    // Selected arrow (CICN 0x3a99, DAT_007dc3b4): 16px box up-right of the
    // marker (0x004a9040), drawn for ANY selected system — so selecting a
    // mission target swaps the red arrow for the green one rather than
    // dropping the marker entirely.
    if (is_mission_target && !is_selected && icons.mission_target) {
      const SDL_FRect dst{m.sx - r - 16.0F, m.sy - r - 16.0F, 16.0F, 16.0F};
      SDL_SetTextureAlphaMod(icons.mission_target->get(),
                             static_cast<std::uint8_t>(255.0F * alpha));
      SDL_RenderTexture(renderer, icons.mission_target->get(), nullptr, &dst);
    }
    if (is_selected && icons.selected_arrow) {
      const SDL_FRect dst{m.sx + r, m.sy - r - 16.0F, 16.0F, 16.0F};
      SDL_SetTextureAlphaMod(icons.selected_arrow->get(),
                             static_cast<std::uint8_t>(255.0F * alpha));
      SDL_RenderTexture(renderer, icons.selected_arrow->get(), nullptr, &dst);
    }
  }

  // The current system's small cyan dot (drawn after the whole marker loop).
  if (const MappedSystem *cur = find_mapped(current);
      cur != nullptr && current >= 0) {
    DrawDisc(renderer, cur->sx, cur->sy, dot_inset, tint(kColorCyan));
  }

  // Selection reticle (green corner ticks around the -8 rect).
  if (const MappedSystem *sel = find_mapped(selected_id); sel != nullptr) {
    const float half = static_cast<double>(view.zoom) < kZoomInLimit
                           ? kReticleInsetZoomedIn
                           : kReticleInset;
    DrawReticle(renderer, sel->sx, sel->sy, half, alpha);
  }

  // Labels (label pass of 0x004a8100): white, offset (+7, +4) from the marker
  // centre (DAT_00575a18/a20). Only VISITED systems (discovery_state > 0; the
  // pass's is_visible + discovery_state gate -- the reveal latch is NOT
  // accepted here) are labelled, and while zoom <= 1.1 all of those are;
  // zoomed in past that, only the selected system keeps its label (0x004a9819).
  const bool labels_all = static_cast<double>(view.zoom) <= kLabelZoomThreshold;
  for (const MappedSystem &m : mapped) {
    if (!SystemVisited(state, m.zero_based_id)) {
      continue;
    }
    if (!labels_all && m.zero_based_id != selected_id) {
      continue;
    }
    NovaText_Draw(platform,
                  font_cache,
                  NovaFontFamily::kGeneva,
                  10.0F,
                  kNovaFontStyleRegular,
                  tint(kColorWhite),
                  m.sx + kLabelOffsetX,
                  m.sy + kLabelOffsetY,
                  systems[static_cast<std::size_t>(m.zero_based_id)].name);
  }

  restore_clip();
}

} // namespace game::starmap_detail
