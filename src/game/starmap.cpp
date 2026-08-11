#include "starmap.hpp"

#include "../log.hpp"
#include "nova_font.hpp"
#include "scenario_data.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

namespace game {
namespace {

// ---- Fixed UI geometry (logical 640x480 playfield) ------------------------
// The map modal draws over a centred 640x480 logical field (the same space the
// other modals use). A title bar sits at the top; the galaxy graph fills the
// panel below it; an inspector footer shows the selected system's details.
constexpr int kPanelX = 8;
constexpr int kPanelY = 8;
constexpr int kPanelW = 624;
constexpr int kPanelH = 430;
constexpr float kTitleBaselineY = 22.0F;
constexpr float kFooterBaselineY = 452.0F;
constexpr float kHelpBaselineY = 470.0F;

// Galaxies can span large/unbounded coordinates; the map view transform maps
// the bounded set of *reachable* system positions (clamped to the current
// system's neighbourhood plus explored neighbours) into the panel.
constexpr float kMarkerRadius = 4.0F;
constexpr float kCurrentSystemRadius = 6.0F;

// The starmap zooms through a handful of fixed levels (like the original's
// stepped zoom, not a continuous slider) and opens on a *middle* level centred
// on the pilot's current system, rather than being pinned to the small
// discovered cluster. Each level multiplies the whole-galaxy fit scale by a
// geometric step; level 0 shows the whole galaxy and higher levels zoom in. The
// step and top level are kept modest so the closest zoom stays a useful
// regional view instead of blowing up to a handful of systems.
constexpr float kZoomStepFactor = 1.5F;
constexpr int kZoomLevelMin = 0;
constexpr int kZoomLevelMax = 5;
constexpr int kStarmapStartLevel = 3; // "middle": a local region of the galaxy

// Colours.
constexpr SDL_Color kWindowBg{0, 0, 0, 255};
constexpr SDL_Color kPanelBg{6, 14, 26, 255};
constexpr SDL_Color kScrim{0, 0, 0, 180};
constexpr SDL_Color kPanelBorder{72, 120, 168, 255};
constexpr SDL_Color kLinkLine{58, 82, 116, 200};
constexpr SDL_Color kLinkLineCurrent{96, 150, 214, 255};
constexpr SDL_Color kMarkerExplored{150, 196, 244, 255};
constexpr SDL_Color kMarkerCurrent{255, 214, 100, 255};
constexpr SDL_Color kTextTitle{202, 224, 255, 255};
constexpr SDL_Color kTextBody{200, 214, 232, 255};
constexpr SDL_Color kTextDim{110, 132, 158, 255};
constexpr SDL_Color kSelectionBox{110, 170, 230, 255};
constexpr SDL_Color kReachableLink{120, 255, 170, 255}; // plotted-route accent

// A single system marker's mapped screen position plus its index, so clicks
// and label collision checks resolve against the same projection as drawing.
struct MappedSystem {
  std::int16_t zero_based_id = -1; // scenario.systems index
  float sx = 0.0F;                 // panel-local x
  float sy = 0.0F;                 // panel-local y
};

// The map view: an SRT-ish transform that places the galaxy bounding box into
// the panel with a user-adjustable scale. Pan/zoom are trivial keyboard ops on
// this structure; clicks map mouse -> world through the inverse.
struct MapView {
  float scale = 1.0F; // panel-px per world-unit
  float offset_x = 0.0F;
  float offset_y = 0.0F;

  [[nodiscard]] float ToScreenX(float world_x) const {
    return offset_x + world_x * scale;
  }

  [[nodiscard]] float ToScreenY(float world_y) const {
    return offset_y + world_y * scale;
  }

  [[nodiscard]] float ToWorldX(float screen_x) const {
    return (screen_x - offset_x) / scale;
  }

  [[nodiscard]] float ToWorldY(float screen_y) const {
    return (screen_y - offset_y) / scale;
  }

  void PanPixels(float dx, float dy) {
    offset_x += dx;
    offset_y += dy;
  }

  // Sets an absolute scale, keeping the world point under `screen_x/y`
  // stationary. Zoom level steps change the scale without relocating the point
  // under the cursor (the panel centre by default).
  void SetScaleAt(float new_scale, float screen_x, float screen_y) {
    const float world_x = ToWorldX(screen_x);
    const float world_y = ToWorldY(screen_y);
    scale = new_scale;
    offset_x = screen_x - world_x * scale;
    offset_y = screen_y - world_y * scale;
  }
};

// Whether the pilot has explored `zero_based_id` (the GameState explored bit
// mirrored to the scenario's per-system flag). Interprets ids conservatively:
// an out-of-range id is not explored.
bool SystemExplored(const GameState &state, std::int16_t zero_based_id) {
  if (zero_based_id < 0) {
    return false;
  }
  const std::size_t idx = static_cast<std::size_t>(zero_based_id);
  if (idx < state.control.explored_systems.size()) {
    return state.control.explored_systems.test(idx);
  }
  return false;
}

// Computes the world->panel transform that fits the world bounding box of the
// *explored* systems into the panel (`only_explored`). The original only draws
// markers for systems with is_visible && has_explored_flag set (see
// NovaUi_DrawStarmapRoutesAndMarkers), so fitting to just the explored set
// both matches that fog of war and keeps the initial view sensibly close-in
// instead of spanning the whole (mostly unplotted) galaxy. When no system is
// explored yet the fit falls back to every system so there is still something
// to frame. Returns false when there is no finite extent to fit (empty /
// degenerate galaxy), in which case the map is left unchanged.
bool FitMapView(const GameState &state,
                MapView &out_view,
                float panel_w,
                float panel_h,
                bool only_explored) {
  const auto &systems = state.scenario.systems;
  float min_x = 0.0F, min_y = 0.0F, max_x = 0.0F, max_y = 0.0F;
  bool seeded = false;
  for (std::size_t i = 0; i < systems.size(); ++i) {
    if (only_explored && !SystemExplored(state, static_cast<std::int16_t>(i))) {
      continue;
    }
    const System &sys = systems[i];
    const float px = static_cast<float>(sys.pos_x);
    const float py = static_cast<float>(sys.pos_y);
    if (!seeded) {
      min_x = max_x = px;
      min_y = max_y = py;
      seeded = true;
    } else {
      min_x = std::min(min_x, px);
      max_x = std::max(max_x, px);
      min_y = std::min(min_y, py);
      max_y = std::max(max_y, py);
    }
  }
  if (!seeded) {
    return false;
  }
  const float span_x = std::max(1.0F, max_x - min_x);
  const float span_y = std::max(1.0F, max_y - min_y);
  const float margin = 24.0F;
  const float avail_w = std::max(1.0F, panel_w - 2.0F * margin);
  const float avail_h = std::max(1.0F, panel_h - 2.0F * margin);
  const float fit = std::min(avail_w / span_x, avail_h / span_y);
  out_view.scale = std::min(fit, 4.0F);
  // Centre the world bounding box in the panel.
  const float centre_x = (min_x + max_x) * 0.5F;
  const float centre_y = (min_y + max_y) * 0.5F;
  out_view.offset_x = panel_w * 0.5F - centre_x * out_view.scale;
  out_view.offset_y = panel_h * 0.5F - centre_y * out_view.scale;
  return true;
}

// Adjusts the view so the given world point lands in the centre of the panel
// at the current scale. Used when opening the map a touch closer than the pure
// fit, anchored on where the pilot currently is.
void CenterViewOn(
    MapView &view, float world_x, float world_y, float panel_w, float panel_h) {
  view.offset_x = panel_w * 0.5F - world_x * view.scale;
  view.offset_y = panel_h * 0.5F - world_y * view.scale;
}

// The owning government's theme colour for a system, or a neutral fallback
// when the system has no (or no valid) government. Governments colour their
// systems on the starmap like a political map (DrawStarmapPoliticalOverlay
// tints by g_government_defs[].theme_color_*); the clean-room uses the decoded
// Government.theme_* fields. The system stores its government as a zero-based
// id, so the resource-keyed accessor needs the +0x80 rebase.
SDL_Color GovernmentColor(const GameState &state, std::int16_t zero_based_id) {
  if (zero_based_id < 0 || static_cast<std::size_t>(zero_based_id) >=
                               state.scenario.systems.size()) {
    return SDL_Color{
        kMarkerExplored.r, kMarkerExplored.g, kMarkerExplored.b, 255};
  }
  const std::int16_t gov =
      state.scenario.systems[static_cast<std::size_t>(zero_based_id)]
          .government_id;
  if (gov < 0) {
    return SDL_Color{
        kMarkerExplored.r, kMarkerExplored.g, kMarkerExplored.b, 255};
  }
  const Government *g =
      state.scenario.Government(static_cast<std::int16_t>(gov + 0x80));
  if (!g || (g->theme_red == 0 && g->theme_green == 0 && g->theme_blue == 0)) {
    return SDL_Color{
        kMarkerExplored.r, kMarkerExplored.g, kMarkerExplored.b, 255};
  }
  return SDL_Color{g->theme_red, g->theme_green, g->theme_blue, 255};
}

// Merges `tint` into `base` by `amount` (0..1), so unexplored/explored markers
// keep their dim/readable base but lean toward the governing faction's colour.
SDL_Color Blend(SDL_Color base, SDL_Color tint, float amount) {
  const float a = std::clamp(amount, 0.0F, 0.9F);
  const auto mix = [a](std::uint8_t x, std::uint8_t y) -> std::uint8_t {
    return static_cast<std::uint8_t>(static_cast<float>(x) * (1.0F - a) +
                                     static_cast<float>(y) * a);
  };
  return SDL_Color{
      mix(base.r, tint.r), mix(base.g, tint.g), mix(base.b, tint.b), 255};
}

// Draws a line segment `thickness`px wide, rendered as a filled quad so the
// map links stay legible at any zoom. Matches the original's 2px link weight
// (FUN_004ba320(2,2) around the starmap route lines) instead of a hairline.
void DrawThickLine(SDL_Renderer *renderer,
                   SDL_FPoint a,
                   SDL_FPoint b,
                   float thickness,
                   SDL_Color tint) {
  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  const float len = std::max(0.0001F, std::sqrt(dx * dx + dy * dy));
  // Unit normal to the segment (half-thickness each side).
  const float nx = -dy / len;
  const float ny = dx / len;
  const float h = thickness * 0.5F;
  const SDL_FColor c{static_cast<float>(tint.r) / 255.0F,
                     static_cast<float>(tint.g) / 255.0F,
                     static_cast<float>(tint.b) / 255.0F,
                     static_cast<float>(tint.a) / 255.0F};
  // Two triangles sharing a diagonal, forming the thick segment quad.
  const SDL_FPoint corners[4]{{a.x + nx * h, a.y + ny * h},
                              {b.x + nx * h, b.y + ny * h},
                              {b.x - nx * h, b.y - ny * h},
                              {a.x - nx * h, a.y - ny * h}};
  SDL_Vertex verts[6];
  // Triangle 1: corners 0,1,2 ; Triangle 2: corners 0,2,3.
  const int idx[6] = {0, 1, 2, 0, 2, 3};
  for (int i = 0; i < 6; ++i) {
    const auto &p = corners[static_cast<std::size_t>(idx[i])];
    verts[i] = SDL_Vertex{SDL_FPoint{p.x, p.y}, c, SDL_FPoint{0.0F, 0.0F}};
  }
  SDL_RenderGeometry(renderer, nullptr, verts, 6, nullptr, 0);
}

// Fills a solid disc centred at (cx, cy) with radius `r`. The original draws
// its system markers as filled circles (Rect_Inset of the marker rect then a
// colour fill, rect FUN_004ba350 in NovaUi_DrawStarmapRoutesAndMarkers), so
// the clean-room renders a true filled disc rather than a hollow ring. Cast as
// a triangle fan so the geometry path (same as DrawThickLine) keeps a single
// consistent render strategy.
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
  // Triangle fan: centre + ring vertex pairs (kSegments triangles).
  std::vector<SDL_Vertex> verts;
  verts.reserve(static_cast<std::size_t>(kSegments) * 3u);
  for (int s = 0; s < kSegments; ++s) {
    const float a0 = static_cast<float>(s) *
                     static_cast<float>(2.0 * std::numbers::pi) /
                     static_cast<float>(kSegments);
    const float a1 = static_cast<float>(s + 1) *
                     static_cast<float>(2.0 * std::numbers::pi) /
                     static_cast<float>(kSegments);
    const SDL_Vertex centre{SDL_FPoint{cx, cy}, c, SDL_FPoint{0.0F, 0.0F}};
    const SDL_Vertex v0{
        SDL_FPoint{cx + r * std::cos(a0), cy + r * std::sin(a0)},
        c,
        SDL_FPoint{0.0F, 0.0F}};
    const SDL_Vertex v1{
        SDL_FPoint{cx + r * std::cos(a1), cy + r * std::sin(a1)},
        c,
        SDL_FPoint{0.0F, 0.0F}};
    verts.push_back(centre);
    verts.push_back(v0);
    verts.push_back(v1);
  }
  SDL_RenderGeometry(renderer,
                     nullptr,
                     verts.data(),
                     static_cast<int>(verts.size()),
                     nullptr,
                     0);
}

// Strokes a polyline ring (open loop, e.g. the selection ring marks) around
// (cx, cy) at radius `r`. The circle markers are filled via DrawDisc; rings are
// only ever decorative accent outlines.
void DrawRing(
    SDL_Renderer *renderer, float cx, float cy, float r, SDL_Color tint) {
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
}

// --- Zoom thresholds (mirror the original's zoom-gated labels: system names
// only render once the map is zoomed in close enough, Dat_00575a10 ; the
// current/selected nodes stay legible regardless). ---
constexpr float kLabelZoomMin = 1.4F; // scale at which labels fade in

// Draws the fixed chrome: the window background, panel frame and title bar.
void DrawChrome(SdlPlatform &platform, NovaFontCache &font_cache) {
  SDL_Renderer *renderer = platform.renderer();
  SDL_SetRenderDrawColor(
      renderer, kWindowBg.r, kWindowBg.g, kWindowBg.b, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(renderer);
  platform.SetCenteredPlayfield();

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  const SDL_FRect full{0,
                       0,
                       static_cast<float>(kLogicalUiWidth),
                       static_cast<float>(kLogicalUiHeight)};
  SDL_SetRenderDrawColor(renderer, kScrim.r, kScrim.g, kScrim.b, kScrim.a);
  SDL_RenderFillRect(renderer, &full);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

  const SDL_FRect panel{kPanelX, kPanelY, kPanelW, kPanelH};
  SDL_SetRenderDrawColor(
      renderer, kPanelBg.r, kPanelBg.g, kPanelBg.b, SDL_ALPHA_OPAQUE);
  SDL_RenderFillRect(renderer, &panel);
  SDL_SetRenderDrawColor(renderer,
                         kPanelBorder.r,
                         kPanelBorder.g,
                         kPanelBorder.b,
                         SDL_ALPHA_OPAQUE);
  SDL_RenderRect(renderer, &panel);

  NovaText_DrawCentered(platform,
                        font_cache,
                        NovaFontFamily::kChicago,
                        16.0F,
                        kNovaFontStyleBold,
                        kTextTitle,
                        0.0F,
                        640.0F,
                        kTitleBaselineY,
                        "Galaxy Map");
  NovaText_DrawCentered(platform,
                        font_cache,
                        NovaFontFamily::kGeneva,
                        11.0F,
                        kNovaFontStyleRegular,
                        kTextDim,
                        0.0F,
                        640.0F,
                        kHelpBaselineY,
                        "Arrows pan   +/- zoom   h home/fit   Tab/\\ "
                        "select   Enter/Esc close");
}

// Draws the galaxy graph: link lines first, then markers and names.
void DrawGalaxy(SdlPlatform &platform,
                NovaFontCache &font_cache,
                const GameState &state,
                const std::vector<MappedSystem> &mapped,
                const MapView &view,
                float panel_x,
                float panel_y,
                std::int16_t selected_id) {
  const auto &systems = state.scenario.systems;
  const std::int16_t current = state.player.current_system_id;
  SDL_Renderer *renderer = platform.renderer();
  // Link/route endpoints and marker positions must share the same origin.
  // Markers are stored panel-relative (m.sx/m.sy include the panel offset in
  // BuildMappedSystems); the raw world projection below does NOT, so the panel
  // origin is added explicitly here to keep lines landing on the circle
  // centres.
  const auto to_screen = [&](float wx, float wy) {
    return SDL_FPoint{panel_x + view.ToScreenX(wx),
                      panel_y + view.ToScreenY(wy)};
  };

  // Link lines, avoiding duplicates (links are symmetric in the System table:
  // system A lists B and B lists A). A link is drawn only when BOTH endpoints
  // are explored/visible; the original draws adjacency lines only for
  // is_visible && has_explored_flag systems, so undiscovered links (and their
  // far ends) stay hidden until the pilot gets close.
  const auto draw_link = [&](std::int16_t a, std::int16_t b) {
    if (a < 0 || b < 0) {
      return;
    }
    const std::size_t idx_a = static_cast<std::size_t>(a);
    const std::size_t idx_b = static_cast<std::size_t>(b);
    if (idx_a >= systems.size() || idx_b >= systems.size()) {
      return;
    }
    if (!SystemExplored(state, a) || !SystemExplored(state, b)) {
      return;
    }
    const System &sa = systems[idx_a];
    const System &sb = systems[idx_b];
    const SDL_FPoint pa =
        to_screen(static_cast<float>(sa.pos_x), static_cast<float>(sa.pos_y));
    const SDL_FPoint pb =
        to_screen(static_cast<float>(sb.pos_x), static_cast<float>(sb.pos_y));
    const bool touches_current = a == current || b == current;
    const SDL_Color line_color = touches_current ? kLinkLineCurrent : kLinkLine;
    DrawThickLine(renderer, pa, pb, touches_current ? 2.0F : 1.4F, line_color);
  };

  for (std::size_t i = 0; i < systems.size(); ++i) {
    const System &sys = systems[i];
    for (const std::int16_t link : sys.links) {
      if (link < 0) {
        continue;
      }
      const std::int16_t zero_based = static_cast<std::int16_t>(link - 0x80);
      if (zero_based >= 0 && static_cast<std::size_t>(zero_based) > i) {
        draw_link(static_cast<std::int16_t>(i), zero_based);
      }
    }
  }

  // Selection accent: whenever the highlighted system is a directly-linked
  // destination of the pilot's current system, draw a thick green line from the
  // current system to it so the planned next jump is obvious before committing.
  // (This is the planning highlight; it only appears for real jump targets, so
  // clicking an unrelated system shows no misleading green line.)
  const std::int16_t plotted = state.travel.starmap_destination_system_id;
  const bool selected_is_link =
      selected_id >= 0 && current >= 0 && selected_id != current &&
      static_cast<std::size_t>(current) < systems.size() &&
      static_cast<std::size_t>(selected_id) < systems.size();
  const bool has_selected_accent = [&] {
    if (!selected_is_link) {
      return false;
    }
    const System &cur = systems[static_cast<std::size_t>(current)];
    const std::int16_t target_res =
        static_cast<std::int16_t>(selected_id + 0x80);
    return std::find(cur.links.begin(), cur.links.end(), target_res) !=
           cur.links.end();
  }();
  if (has_selected_accent) {
    const System &dep = systems[static_cast<std::size_t>(current)];
    const System &dst = systems[static_cast<std::size_t>(selected_id)];
    const SDL_FPoint pa =
        to_screen(static_cast<float>(dep.pos_x), static_cast<float>(dep.pos_y));
    const SDL_FPoint pb =
        to_screen(static_cast<float>(dst.pos_x), static_cast<float>(dst.pos_y));
    DrawThickLine(renderer, pa, pb, 3.0F, kReachableLink);
  }

  // "Current path" accent (mirrors the original's travel_transfer_mode == 3
  // active-jump route, DAT_00733b38): when the player has COMMITTED a jump to a
  // directly-linked system, emphasise that single route with a thick green
  // line from the current system to the destination -- NOT a green fan over
  // every reachable link, which the original does not draw. (For an uncommitted
  // selection the selection accent above draws the equivalent line.) The small
  // green link marker at the destination (drawn over the marker pass below)
  // points at exactly which jump is committed.
  const bool plotted_direct =
      plotted >= 0 && current >= 0 && plotted != current &&
      static_cast<std::size_t>(current) < systems.size() &&
      static_cast<std::size_t>(plotted) < systems.size();
  if (plotted_direct) {
    const System &dep = systems[static_cast<std::size_t>(current)];
    const System &dst = systems[static_cast<std::size_t>(plotted)];
    const SDL_FPoint pa =
        to_screen(static_cast<float>(dep.pos_x), static_cast<float>(dep.pos_y));
    const SDL_FPoint pb =
        to_screen(static_cast<float>(dst.pos_x), static_cast<float>(dst.pos_y));
    DrawThickLine(renderer, pa, pb, 3.0F, kReachableLink);
  }

  // Markers + names.
  for (const auto &m : mapped) {
    const bool is_current = m.zero_based_id == current;
    const bool explored = SystemExplored(state, m.zero_based_id);
    const bool selected = m.zero_based_id == selected_id;
    // Fog of war: only draw markers for systems the pilot has discovered. The
    // original gates every marker on is_visible && has_explored_flag, so an
    // undiscovered system is simply absent from the map (no node, no label).
    if (!explored && !is_current) {
      continue;
    }
    // Colour by owning government (political-map tint) blended into the
    // explored/unexplored base so explored systems read as explorable territory
    // of their faction.
    const SDL_Color gov = GovernmentColor(state, m.zero_based_id);
    SDL_Color c = Blend(kMarkerExplored, gov, 0.55F);
    if (is_current) {
      c = kMarkerCurrent;
    }
    if (selected) {
      c = kSelectionBox;
    }
    // Marker radius scales a touch with zoom (larger close-up), mirroring the
    // original's zoom-dependent marker insets (Rect_Inset -4 vs -6 in
    // NovaUi_DrawStarmapRoutesAndMarkers).
    const float zoom_scale =
        std::clamp(0.8F + (view.scale - 1.0F) * 0.15F, 0.8F, 1.3F);
    const float r =
        (is_current ? kCurrentSystemRadius : kMarkerRadius) * zoom_scale;
    // Filled disc marker centred exactly on the system node.
    DrawDisc(renderer, m.sx, m.sy, r, c);
    // Selection ring for the highlighted system.
    if (selected) {
      DrawRing(renderer, m.sx, m.sy, r + 3.0F, kSelectionBox);
    }
    // Small green link marker on the selected/plotted-jump destination (the
    // original points a small green marker at the active jump's target stellar
    // rather than highlighting every reachable link). Draws on the selected
    // system when it's a jump target, and on the committed plotted destination.
    if ((has_selected_accent && m.zero_based_id == selected_id) ||
        (plotted_direct && m.zero_based_id == plotted)) {
      DrawRing(renderer, m.sx, m.sy, r + 1.5F, kReachableLink);
    }
    // Label the current / selected systems always; label explored systems
    // only when zoomed in enough, mirroring the original's zoom-gated labels
    // (names draw once the map is close enough, Dat_00575a10). Unexplored
    // names stay hidden entirely (matching the original's fog of war).
    const bool named =
        is_current || selected || (explored && view.scale >= kLabelZoomMin);
    if (named) {
      if (m.zero_based_id >= 0 &&
          static_cast<std::size_t>(m.zero_based_id) < systems.size()) {
        NovaText_Draw(platform,
                      font_cache,
                      NovaFontFamily::kGeneva,
                      10.0F,
                      is_current ? kNovaFontStyleBold : kNovaFontStyleRegular,
                      is_current ? kMarkerCurrent : kTextBody,
                      m.sx + 6.0F,
                      m.sy - 4.0F,
                      systems[static_cast<std::size_t>(m.zero_based_id)].name);
      }
    }
  }
}

// Draws the inspector footer for the selected system.
void DrawInspector(SdlPlatform &platform,
                   NovaFontCache &font_cache,
                   const GameState &state,
                   std::int16_t selected_id) {
  std::string line = "No system selected";
  if (selected_id >= 0 &&
      static_cast<std::size_t>(selected_id) < state.scenario.systems.size()) {
    const System &sys =
        state.scenario.systems[static_cast<std::size_t>(selected_id)];
    const bool explored = SystemExplored(state, selected_id);
    const bool is_current = selected_id == state.player.current_system_id;
    line = sys.name + "   ";
    line +=
        is_current ? "[current]" : (explored ? "[explored]" : "[undiscovered]");
    // Flag the system plotted as the next jumped-to destination (via a prior
    // starmap selection), so the landing-feedback tells the player which system
    // the plotted jump actually targets.
    if (state.travel.starmap_destination_system_id == selected_id) {
      line += "  [plotted target]";
    }
    // Owning government (the political-map affiliation of this system).
    if (sys.government_id >= 0) {
      if (const Government *g = state.scenario.Government(
              static_cast<std::int16_t>(sys.government_id + 0x80))) {
        if (!g->name.empty()) {
          line += "  " + g->name;
        }
      }
    }
    // Count reachable jumps from this system.
    int jump_count = 0;
    for (const std::int16_t link : sys.links) {
      if (link >= 0x80) {
        ++jump_count;
      }
    }
    line += "  jumps: " + std::to_string(jump_count);
  }
  NovaText_DrawCentered(platform,
                        font_cache,
                        NovaFontFamily::kGeneva,
                        12.0F,
                        kNovaFontStyleRegular,
                        kTextBody,
                        0.0F,
                        640.0F,
                        kFooterBaselineY,
                        line);
}

// Builds the projected marker list for the current view.
std::vector<MappedSystem> BuildMappedSystems(const GameState &state,
                                             const MapView &view,
                                             const SDL_FRect &panel) {
  std::vector<MappedSystem> out;
  out.reserve(state.scenario.systems.size());
  for (std::size_t i = 0; i < state.scenario.systems.size(); ++i) {
    const System &sys = state.scenario.systems[i];
    MappedSystem m;
    m.zero_based_id = static_cast<std::int16_t>(i);
    m.sx = panel.x + view.ToScreenX(static_cast<float>(sys.pos_x));
    m.sy = panel.y + view.ToScreenY(static_cast<float>(sys.pos_y));
    out.push_back(m);
  }
  return out;
}

// Distance-squared from a panel point to a system marker, in panel px.
float MarkerDistSq(const SDL_FPoint &point, const MappedSystem &m) {
  const float dx = point.x - m.sx;
  const float dy = point.y - m.sy;
  return dx * dx + dy * dy;
}

} // namespace

// ---------------------------------------------------------------------------
// NovaStarmap_RunWindow (0x004a3aa0 NovaUi_RunStarmapWindow, clean-room).
// ---------------------------------------------------------------------------
StarmapResult NovaStarmap_RunWindow(SdlPlatform &platform, GameState &state) {
  // No scenario tables -> nothing to map.
  if (state.scenario.systems.empty()) {
    NovaLog::Warn("starmap opened with no scenario system table; closing");
    return StarmapResult{};
  }

  NovaFontCache font_cache;
  const SDL_FRect panel{
      static_cast<float>(kPanelX),
      static_cast<float>(kPanelY),
      static_cast<float>(kPanelW),
      static_cast<float>(kPanelH),
  };

  // Initial view: open on a *middle* zoom level centred on the pilot's current
  // system. The zoom levels are a geometric series over the whole-galaxy fit
  // scale (level 0 = whole galaxy, higher = closer); a fixed middle level is
  // used rather than pinning the view to the small discovered cluster, so the
  // map opens with the current system's local neighbourhood in view and span a
  // good region of the galaxy. `+`/`-` step between levels and `h` snaps back
  // here ('h' also re-centres on the current system).
  MapView view;
  const float whole_fit_scale =
      FitMapView(state, view, panel.w, panel.h, /*only_explored=*/false)
          ? view.scale
          : 1.0F;
  int zoom_level = kStarmapStartLevel;
  const auto apply_zoom_level = [&]() {
    view.scale = whole_fit_scale *
                 std::pow(kZoomStepFactor, static_cast<float>(zoom_level));
  };
  apply_zoom_level();
  if (const std::int16_t cur = state.player.current_system_id;
      cur >= 0 &&
      static_cast<std::size_t>(cur) < state.scenario.systems.size()) {
    const System &cur_sys =
        state.scenario.systems[static_cast<std::size_t>(cur)];
    // Centre on the current system at the middle zoom level.
    CenterViewOn(view,
                 static_cast<float>(cur_sys.pos_x),
                 static_cast<float>(cur_sys.pos_y),
                 panel.w,
                 panel.h);
  }

  // Steps the zoom level, preserving the *current system's* on-screen position
  // so zooming pivots around it (the focus of the map). This mirrors the
  // original, which keeps its fixed pan reference stationary as the zoom scales
  // about it rather than re-scaling around an arbitrary panel corner. When the
  // current system is invalid, the panel centre is the pivot.
  const auto zoom_to_level = [&](int new_level) {
    float pivot_x = panel.w * 0.5F;
    float pivot_y = panel.h * 0.5F;
    if (const std::int16_t cur = state.player.current_system_id;
        cur >= 0 &&
        static_cast<std::size_t>(cur) < state.scenario.systems.size()) {
      const System &cur_sys =
          state.scenario.systems[static_cast<std::size_t>(cur)];
      pivot_x = view.ToScreenX(static_cast<float>(cur_sys.pos_x));
      pivot_y = view.ToScreenY(static_cast<float>(cur_sys.pos_y));
    }
    zoom_level = std::clamp(new_level, kZoomLevelMin, kZoomLevelMax);
    apply_zoom_level();
    view.SetScaleAt(view.scale, pivot_x, pivot_y);
  };

  std::int16_t selected_id = state.player.current_system_id;
  bool tab_was_held = false;
  bool backslash_was_held = false;

  // The map's Tab / Backslash cycling step, per the EV Nova manual, through
  // "all the systems that are linked to your current system." These are the
  // direct jump destinations fanning out of the player's current system
  // (System.links), the same candidate set the original's command 0x60 uses
  // for the in-flight Backslash cycle. The ring contains ONLY those destination
  // systems -- never the current system itself (jumping to where you already
  // are is meaningless) -- anchored to the player's current system (a fixed
  // ring), so a single-link system offers just that one destination rather
  // than chain-walking outward. The order is computed once per session.
  std::vector<std::int16_t> tab_order;
  {
    const auto &systems = state.scenario.systems;
    const std::size_t n = systems.size();
    const std::size_t current_ok =
        state.player.current_system_id >= 0
            ? static_cast<std::size_t>(state.player.current_system_id)
            : n;
    if (current_ok < n) {
      for (const std::int16_t link : systems[current_ok].links) {
        if (link < 0x80) {
          continue;
        }
        const std::int16_t dest = static_cast<std::int16_t>(link - 0x80);
        if (dest < 0 || static_cast<std::size_t>(dest) >= n) {
          continue; // dangling link
        }
        // Only cycle explored systems (undiscovered links have no map node to
        // select; the discovery flood keeps the current system's direct links
        // explored, so this filter is just defensive).
        if (!SystemExplored(state, dest)) {
          continue;
        }
        if (std::find(tab_order.begin(), tab_order.end(), dest) ==
            tab_order.end()) {
          tab_order.push_back(dest);
        }
      }
    }
  }

  NovaLog::Info("opening galaxy starmap ({} systems in scenario)",
                state.scenario.systems.size());

  // Builds the modal result from the currently highlighted selection. The
  // highlighted system is the plotted next-jump destination the caller may
  // act on, unless it is the player's current system (no plot).
  const auto close_with_selection = [&]() {
    StarmapResult r;
    r.exit = StarmapExit::kContinue;
    if (selected_id != state.player.current_system_id) {
      r.destination_system_id = selected_id;
    }
    return r;
  };

  while (!platform.quit_requested()) {
    const auto mapped = BuildMappedSystems(state, view, panel);
    DrawChrome(platform, font_cache);
    DrawGalaxy(platform,
               font_cache,
               state,
               mapped,
               view,
               panel.x,
               panel.y,
               selected_id);
    DrawInspector(platform, font_cache, state, selected_id);
    SDL_RenderPresent(platform.renderer());

    // Process one batch of raw editable keys / clicks.
    for (std::optional<TextInput> in; (in = platform.PollTextEvent());) {
      switch (in->key) {
      case TextKey::escape:
        return close_with_selection();

      case TextKey::enter:
        // Selecting the current system's own node (or the current default) is
        // a no-op navigation-wise; Enter confirms the selection and closes.
        return close_with_selection();

      case TextKey::character: {
        const char ch = in->character;
        if (ch == 'q' || ch == 'x') {
          return close_with_selection();
        }
        if (ch == '+' || ch == '=') {
          if (zoom_level < kZoomLevelMax) {
            zoom_to_level(zoom_level + 1);
          }
        } else if (ch == '-') {
          if (zoom_level > kZoomLevelMin) {
            zoom_to_level(zoom_level - 1);
          }
        } else if (ch == 'h' || ch == 'H') {
          // Snap back to the opening view: middle zoom level centred on the
          // pilot's current system.
          zoom_to_level(kStarmapStartLevel);
          if (const std::int16_t cur = state.player.current_system_id;
              cur >= 0 &&
              static_cast<std::size_t>(cur) < state.scenario.systems.size()) {
            const System &cur_sys =
                state.scenario.systems[static_cast<std::size_t>(cur)];
            CenterViewOn(view,
                         static_cast<float>(cur_sys.pos_x),
                         static_cast<float>(cur_sys.pos_y),
                         panel.w,
                         panel.h);
          }
        }
        break;
      }

      case TextKey::primary: {
        // Select the nearest *discovered* marker within a click radius; empty
        // click keeps the current selection. Undiscovered systems have no map
        // presence, so they cannot be picked either.
        const SDL_FPoint mp = platform.mouse_position();
        std::int16_t best = -1;
        float best_d = 18.0F * 18.0F;
        for (const auto &m : mapped) {
          if (m.zero_based_id != state.player.current_system_id &&
              !SystemExplored(state, m.zero_based_id)) {
            continue;
          }
          const float d = MarkerDistSq(mp, m);
          if (d < best_d) {
            best_d = d;
            best = m.zero_based_id;
          }
        }
        if (best >= 0) {
          selected_id = best;
        }
        break;
      }

      case TextKey::backspace:
      case TextKey::none:
        break;
      }
    }

    // Non-blocking keyboard pan/zoom polled from the live state so holding a
    // key pans continuously (mirrors the original's interactive panning).
    const bool *const keys = SDL_GetKeyboardState(nullptr);
    constexpr float kPanPxPerEvent = 10.0F;
    // Arrow keys pan the view/camera: content scrolls opposite the key
    // direction (Right shows what's to the right of the current view, i.e. the
    // map content shifts left).
    if (keys[SDL_SCANCODE_LEFT]) {
      view.PanPixels(kPanPxPerEvent, 0.0F);
    }
    if (keys[SDL_SCANCODE_RIGHT]) {
      view.PanPixels(-kPanPxPerEvent, 0.0F);
    }
    if (keys[SDL_SCANCODE_UP]) {
      view.PanPixels(0.0F, kPanPxPerEvent);
    }
    if (keys[SDL_SCANCODE_DOWN]) {
      view.PanPixels(0.0F, -kPanPxPerEvent);
    }
    // Tab / Backslash step the selection through the destination ring computed
    // when the window opened (the current system's directly-linked destination
    // systems, per the manual: "cycle through all the systems that are linked
    // to your current system"). Tab and Backslash move forward; Shift+Tab and
    // Shift+Backslash move backward. The initial selection (the current system)
    // is not itself in the ring, so the first forward step lands on the first
    // destination and the first backward step on the last. Held keys repeat via
    // the scan state; a one-shot requires a press edge (PollTextEvent does not
    // deliver Tab, so poll the key state here).
    const auto cycle_selection = [&](bool forward) {
      if (tab_order.empty()) {
        return;
      }
      auto it = std::find(tab_order.begin(), tab_order.end(), selected_id);
      if (it == tab_order.end()) {
        // Selection not in the ring (e.g. the current system itself): start at
        // the appropriate end of the destination ring rather than skipping the
        // first/last entry.
        selected_id = tab_order[forward ? 0 : tab_order.size() - 1];
        return;
      }
      std::size_t index = static_cast<std::size_t>(it - tab_order.begin());
      if (forward ? (index + 1 >= tab_order.size()) : (index == 0)) {
        selected_id = tab_order[forward ? 0 : tab_order.size() - 1];
      } else {
        selected_id = tab_order[index + (forward ? 1 : -1)];
      }
    };
    const bool shift = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
    const bool tab_pressed = keys[SDL_SCANCODE_TAB];
    const bool backslash_pressed = keys[SDL_SCANCODE_BACKSLASH];
    const bool cycle_pressed = (tab_pressed || backslash_pressed) &&
                               (tab_was_held || backslash_was_held) == false;
    if (cycle_pressed) {
      cycle_selection(!shift);
    }
    tab_was_held = tab_pressed;
    backslash_was_held = backslash_pressed;
  }

  StarmapResult quit;
  quit.exit = StarmapExit::kQuit;
  quit.destination_system_id = selected_id;
  return quit;
}

} // namespace game
