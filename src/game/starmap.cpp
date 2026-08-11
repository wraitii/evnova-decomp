#include "starmap.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../pict_image.hpp"
#include "hud_overlay.hpp"
#include "nova_font.hpp"
#include "scenario_data.hpp"
#include "services_buttons.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

namespace game {
namespace {

// ---- Fixed UI geometry (logical 640x480 playfield) ------------------------
// The starmap window is the EV Nova DLOG 0x7d0 dialog: a 601x513 frame whose
// backdrop is the PICT 0x213d "Map" resource, blitted across the whole window
// (DAT_007dc73c in NovaUi_RunStarmapWindow). DITL 0x7d0 provides the other
// window elements (UiPanel_GetEntryInfo entries are 1-based, so entry N = DITL
// item N-1):
//   item 2 (entry 3) -- the "big panel" galaxy-graph viewport, x=9..467,
//                       y=8..428;
//   item 5 (entry 6) -- the right-hand selected-system detail column,
//                       x=474..594, y=8..437;
//   item 1 (entry 2) -- the bottom status bar, x=8..594, y=436..478;
//   items 0/3/4/7/8/9 (entries 1/4/5/8/9/10) -- the bottom button row at
//                       y=483..508: Show Borders, Clear Route, Find, '-', '+',
//                       Done. Their labels come from STR# 0x96 and their
//                       actions from the clicked 1-based entry ordinal
//                       (NovaUi_StarmapWindowInnerLoop's action dispatch).
// The window is 513 tall, taller than the 640x480 field, so it is scaled down
// uniformly (480/513) to keep the whole dialog -- map, side column, status bar
// AND the bottom button row -- on screen. The starfield backdrop scales
// invisibly; all DITL item rects below are the scaled playfield rects.
constexpr int kStarmapWindowW = 601;
constexpr int kStarmapWindowH = 513;
constexpr float kStarmapFitScale =
    static_cast<float>(kLogicalUiHeight) /
    static_cast<float>(kStarmapWindowH); // ~0.936
constexpr float kStarmapWindowX =
    (640.0F - static_cast<float>(kStarmapWindowW) * kStarmapFitScale) /
    2.0F; // ~38.8
constexpr float kStarmapWindowY = 0.0F;
// DITL item 2 (the galaxy-graph viewport): window origin + item rect
// (left=9, top=8, right=467, bottom=428) scaled to the field. These are the
// defaults; the rects are re-derived from the DITL at runtime when the
// resource parses.
constexpr float kMapPanelX = kStarmapWindowX + 9.0F * kStarmapFitScale;
constexpr float kMapPanelY = 8.0F * kStarmapFitScale;
constexpr float kMapPanelW = 458.0F * kStarmapFitScale;
constexpr float kMapPanelH = 420.0F * kStarmapFitScale;
// DITL item 5: the right-hand selected-system detail column.
constexpr float kSidePanelX = kStarmapWindowX + 474.0F * kStarmapFitScale;
constexpr float kSidePanelY = 8.0F * kStarmapFitScale;
constexpr float kSidePanelW = 120.0F * kStarmapFitScale;
constexpr float kSidePanelH = 429.0F * kStarmapFitScale;
// DITL item 1: the bottom status bar.
constexpr float kBottomBarX = kStarmapWindowX + 8.0F * kStarmapFitScale;
constexpr float kBottomBarY = 436.0F * kStarmapFitScale;
constexpr float kBottomBarW = 586.0F * kStarmapFitScale;
constexpr float kBottomBarH = 42.0F * kStarmapFitScale;

// The bottom button row (DITL items 8,7,9,3,4,0 in left-to-right screen order;
// 1-based entries 9,8,10,4,5,1 -> actions Show Borders, Clear Route, Find,
// zoom out, zoom in, Done). Each entry is the item rect's (left, top, w, h)
// in the DITL authoring space, scaled to the field.
struct StarmapButtonRect {
  float x, y, w, h;
};

constexpr std::array<StarmapButtonRect, 6> kStarmapButtonRects{{
    // Show Borders / Hide Borders (item 8): x=11..141, y=483..508
    {kStarmapWindowX + 11.0F * kStarmapFitScale,
     kStarmapWindowY + 483.0F * kStarmapFitScale,
     130.0F * kStarmapFitScale,
     25.0F * kStarmapFitScale},
    // Clear Route (item 7): x=155..275
    {kStarmapWindowX + 155.0F * kStarmapFitScale,
     kStarmapWindowY + 483.0F * kStarmapFitScale,
     120.0F * kStarmapFitScale,
     25.0F * kStarmapFitScale},
    // Find (item 9): x=288..387
    {kStarmapWindowX + 288.0F * kStarmapFitScale,
     kStarmapWindowY + 483.0F * kStarmapFitScale,
     99.0F * kStarmapFitScale,
     25.0F * kStarmapFitScale},
    // Zoom out '-' (item 3): x=408..433
    {kStarmapWindowX + 408.0F * kStarmapFitScale,
     kStarmapWindowY + 483.0F * kStarmapFitScale,
     25.0F * kStarmapFitScale,
     25.0F * kStarmapFitScale},
    // Zoom in '+' (item 4): x=438..463
    {kStarmapWindowX + 438.0F * kStarmapFitScale,
     kStarmapWindowY + 483.0F * kStarmapFitScale,
     25.0F * kStarmapFitScale,
     25.0F * kStarmapFitScale},
    // Done (item 0): x=483..582
    {kStarmapWindowX + 483.0F * kStarmapFitScale,
     kStarmapWindowY + 483.0F * kStarmapFitScale,
     99.0F * kStarmapFitScale,
     25.0F * kStarmapFitScale},
}};

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
constexpr SDL_Color kTextBody{200, 214, 232, 255};
constexpr SDL_Color kTextDim{110, 132, 158, 255};
constexpr SDL_Color kSelectionBox{110, 170, 230, 255};
constexpr SDL_Color kReachableLink{120, 255, 170, 255}; // plotted-route accent

// The bottom button row, in left-to-right screen order (DITL items 8,7,9,3,4,0
// -> 1-based entries 9,8,10,4,5,1 -> NovaUi_StarmapWindowInnerLoop actions
// Show Borders, Clear Route, Find, zoom out, zoom in, Done). The labels come
// from STR# 0x96 "button labels" (NovaHud_LoadStringEntry).
enum class StarmapButton : std::size_t {
  kShowBorders = 0, // STR# 0x96 [55] / [56] (Show/Hide Borders)
  kClearRoute = 1,  // STR# 0x96 [48]
  kFind = 2,        // STR# 0x96 [59]
  kZoomOut = 3,     // STR# 0x96 [16] '-'
  kZoomIn = 4,      // STR# 0x96 [17] '+'
  kDone = 5,        // STR# 0x96 [4]
};

// The starmap geometry resolved from the DLOG/DITL dialog resources: the full
// 601x513 window frame (root of the backdrop blit and its DITL items), the
// galaxy-graph viewport (DITL item 2), the right-hand detail column (DITL item
// 5), the bottom status bar (DITL item 1) and the bottom button row (DITL
// items 8/7/9/3/4/0), all in scaled playfield coordinates.
struct StarmapGeometry {
  SDL_FRect window{};                 // full backdrop frame, playfield coords
  SDL_FRect map{};                    // galaxy-graph viewport, playfield coords
  SDL_FRect side{};                   // selected-system detail column
  SDL_FRect bar{};                    // bottom status bar
  std::array<SDL_FRect, 6> buttons{}; // bottom button row, StarmapButton order
};

// A single system marker's mapped screen position plus its index, so clicks
// and label collision checks resolve against the same projection as drawing.
struct MappedSystem {
  std::int16_t zero_based_id = -1; // scenario.systems index
  float sx = 0.0F;                 // playfield x
  float sy = 0.0F;                 // playfield y
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

// Loads the starmap backdrop PICT (0x213d "Map", the DAT_007dc73c backdrop
// NovaUi_RunStarmapWindow loads) into a texture sized to the 601x513 DLOG
// 0x7d0 window. Returns null when the resource is missing or fails to decode.
std::unique_ptr<SdlTexture> LoadStarmapBackdrop(SdlPlatform &platform) {
  const auto data = NovaResource_LoadPictData(0x213d);
  if (!data) {
    return {};
  }
  const auto img = Resource_LoadPictAsImage(*data);
  if (!img) {
    return {};
  }
  return SdlTexture::Create(
      platform.renderer(), img->width, img->height, img->rgba_pixels);
}

// Resolves the starmap window + panes + button row from the DLOG 0x7d0 /
// DITL 0x7d0 dialog resources. The 601x513 window is scaled down to fit the
// 640x480 field (kStarmapFitScale); every DITL item rect is offset by the
// window origin and scaled by the same factor. Falls back to verified
// hardcoded rects when the DITL resource can't be located.
StarmapGeometry ResolveStarmapGeometry() {
  StarmapGeometry g;
  g.window = SDL_FRect{kStarmapWindowX,
                       kStarmapWindowY,
                       static_cast<float>(kStarmapWindowW) * kStarmapFitScale,
                       static_cast<float>(kStarmapWindowH) * kStarmapFitScale};
  g.map = SDL_FRect{kMapPanelX, kMapPanelY, kMapPanelW, kMapPanelH};
  g.side = SDL_FRect{kSidePanelX, kSidePanelY, kSidePanelW, kSidePanelH};
  g.bar = SDL_FRect{kBottomBarX, kBottomBarY, kBottomBarW, kBottomBarH};
  for (std::size_t i = 0; i < kStarmapButtonRects.size(); ++i) {
    const auto &b = kStarmapButtonRects[i];
    g.buttons[i] = SDL_FRect{b.x, b.y, b.w, b.h};
  }
  const auto items = NovaResource_LoadDialogItems(0x7d0);
  if (!items) {
    return g;
  }
  // DITL items are 0-based; UiPanel_GetEntryInfo addresses them 1-based, so
  // entry 3 = item 2 (galaxy graph), entry 6 = item 5 (side column), entry 2 =
  // item 1 (bottom bar) and the button entries 9/8/10/4/5/1 = items
  // 8/7/9/3/4/0. Use each item's authored rect offset by the window origin and
  // scaled.
  const auto offset_rect = [](const NovaDialogItem &it) {
    return SDL_FRect{
        kStarmapWindowX + static_cast<float>(it.left) * kStarmapFitScale,
        kStarmapWindowY + static_cast<float>(it.top) * kStarmapFitScale,
        static_cast<float>(it.right - it.left) * kStarmapFitScale,
        static_cast<float>(it.bottom - it.top) * kStarmapFitScale};
  };
  const auto valid = [](const NovaDialogItem &it) {
    return it.right > it.left && it.bottom > it.top;
  };
  if (items->size() > 2 && valid((*items)[2])) {
    g.map = offset_rect((*items)[2]);
  }
  if (items->size() > 5 && valid((*items)[5])) {
    g.side = offset_rect((*items)[5]);
  }
  if (items->size() > 1 && valid((*items)[1])) {
    g.bar = offset_rect((*items)[1]);
  }
  // Button row, in StarmapButton screen order: item 8 -> Show Borders, item 7
  // -> Clear Route, item 9 -> Find, item 3 -> zoom out, item 4 -> zoom in,
  // item 0 -> Done.
  const std::array<std::size_t, 6> button_items{8, 7, 9, 3, 4, 0};
  for (std::size_t i = 0; i < button_items.size(); ++i) {
    const std::size_t item_idx = button_items[i];
    if (items->size() > item_idx && valid((*items)[item_idx])) {
      g.buttons[i] = offset_rect((*items)[item_idx]);
    }
  }
  return g;
}

// Draws the fixed chrome: the scrim, the 601x513 starmap window frame
// (backdrop PICT when available, else a bordered placeholder) and the inset
// panels of the remaining DITL elements -- the map viewport (item 2), the
// right-hand detail column (item 5) and the bottom status bar (item 1). No
// title bar: the window's own PICT carries its look.
void DrawChrome(SdlPlatform &platform,
                const StarmapGeometry &geometry,
                SDL_Texture *backdrop) {
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

  if (backdrop != nullptr) {
    // Blit the full 601x513 starfield backdrop across the window (the original
    // blits DAT_007dc73c to UiWindow_GetRect).
    SDL_RenderTexture(renderer, backdrop, nullptr, &geometry.window);
  } else {
    // No backdrop art: bordered placeholder so the map stays legible.
    SDL_SetRenderDrawColor(
        renderer, kPanelBg.r, kPanelBg.g, kPanelBg.b, SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(renderer, &geometry.window);
    SDL_SetRenderDrawColor(renderer,
                           kPanelBorder.r,
                           kPanelBorder.g,
                           kPanelBorder.b,
                           SDL_ALPHA_OPAQUE);
    SDL_RenderRect(renderer, &geometry.window);
  }

  // Inset panels for the map viewport and the two info regions (DITL items
  // 2/5/1): a flat dark fill keeps the text legible over the starfield, with a
  // subtle hairline edge so each region reads as its own pane.
  const SDL_FRect *const panes[3] = {
      &geometry.map, &geometry.side, &geometry.bar};
  for (const SDL_FRect *pane : panes) {
    SDL_SetRenderDrawColor(
        renderer, kPanelBg.r, kPanelBg.g, kPanelBg.b, SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(renderer, pane);
    SDL_SetRenderDrawColor(
        renderer, kPanelBorder.r, kPanelBorder.g, kPanelBorder.b, 160);
    SDL_RenderRect(renderer, pane);
  }
}

// Draws the galaxy graph: link lines first, then markers and names, clipped to
// the map panel (DITL item 2) so routes/names never paint over the window
// chrome. `show_borders` is the political-overlay toggle: when on, markers
// lean more strongly toward their owning government's colour (a lightweight
// stand-in for the original's per-cell government overlay grid).
void DrawGalaxy(SdlPlatform &platform,
                NovaFontCache &font_cache,
                const GameState &state,
                const std::vector<MappedSystem> &mapped,
                const MapView &view,
                const StarmapGeometry &geometry,
                std::int16_t selected_id,
                bool show_borders) {
  const auto &systems = state.scenario.systems;
  const std::int16_t current = state.player.current_system_id;
  SDL_Renderer *renderer = platform.renderer();
  const float panel_x = geometry.map.x;
  const float panel_y = geometry.map.y;
  // Scissor the galaxy graph to the map panel so off-field nodes/labels stay
  // inside the window frame.
  const SDL_Rect clip{static_cast<int>(geometry.map.x),
                      static_cast<int>(geometry.map.y),
                      static_cast<int>(geometry.map.w),
                      static_cast<int>(geometry.map.h)};
  SDL_SetRenderClipRect(renderer, &clip);
  const auto restore_clip = [&]() { SDL_SetRenderClipRect(renderer, nullptr); };

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
    SDL_Color c = Blend(kMarkerExplored, gov, show_borders ? 0.9F : 0.55F);
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

  restore_clip();
}

// Truncates `text` to fit within `max_width` logical pixels at the given font
// metrics, appending "..." when cut. The detail column (DITL item 5) is only
// ~120px wide, so long system/government names must be shortened to stay on
// the pane (the original draws these same fields into the narrow column).
std::string TruncateToWidth(NovaFontCache &font_cache,
                            NovaFontFamily family,
                            float point_size,
                            std::uint16_t style,
                            std::string_view text,
                            int max_width) {
  if (font_cache.TextWidth(family, point_size, style, text) <= max_width) {
    return std::string(text);
  }
  std::string cut(text);
  while (!cut.empty()) {
    cut.pop_back();
    if (font_cache.TextWidth(family, point_size, style, cut + "...") <=
        max_width) {
      return cut + "...";
    }
  }
  return "...";
}

// Draws the two info panes the DITL defines around the map graph. The right
// column (DITL item 5, UiPanel_GetEntryInfo entry 6) shows the selected
// system's details -- name, explored/current state, owning government and
// outward jump count -- mirroring the original's right-hand detail pane; the
// bottom bar (DITL item 1, entry 2) carries the "Selected System:" line plus
// the keyboard hints (a stand-in for the original's date/route status line).
void DrawSidePanels(SdlPlatform &platform,
                    NovaFontCache &font_cache,
                    const GameState &state,
                    const StarmapGeometry &geometry,
                    std::int16_t selected_id,
                    const std::string *search_query) {
  SDL_Renderer *renderer = platform.renderer();

  // ---- Right column: selected-system details. ----
  std::string sys_name = "No system selected";
  std::string status;
  SDL_Color status_color = kTextDim;
  std::string govt = "?";
  int jump_count = 0;
  if (selected_id >= 0 &&
      static_cast<std::size_t>(selected_id) < state.scenario.systems.size()) {
    const System &sys =
        state.scenario.systems[static_cast<std::size_t>(selected_id)];
    const bool explored = SystemExplored(state, selected_id);
    const bool is_current = selected_id == state.player.current_system_id;
    sys_name = sys.name;
    if (is_current) {
      status = "current system";
      status_color = kMarkerCurrent;
    } else if (explored) {
      status = "explored";
      status_color = kTextBody;
    } else {
      status = "undiscovered";
      status_color = kTextDim;
    }
    // Flag the system plotted as the next jumped-to destination (via a prior
    // starmap selection), so the landing-feedback tells the player which
    // system the plotted jump actually targets.
    if (state.travel.starmap_destination_system_id == selected_id) {
      status += " - plotted target";
      status_color = kReachableLink;
    }
    // Owning government (the political-map affiliation of this system).
    if (sys.government_id >= 0) {
      if (const Government *g = state.scenario.Government(
              static_cast<std::int16_t>(sys.government_id + 0x80))) {
        if (!g->name.empty()) {
          govt = g->name;
        }
      }
    }
    // Count reachable jumps from this system.
    for (const std::int16_t link : sys.links) {
      if (link >= 0x80) {
        ++jump_count;
      }
    }
  }

  const float col_x = geometry.side.x + 6.0F;
  const int col_w = static_cast<int>(geometry.side.w) - 12;
  const auto fit = [&](std::string_view text) {
    return TruncateToWidth(font_cache,
                           NovaFontFamily::kGeneva,
                           10.0F,
                           kNovaFontStyleRegular,
                           text,
                           col_w);
  };
  NovaText_Draw(platform,
                font_cache,
                NovaFontFamily::kGeneva,
                10.0F,
                kNovaFontStyleBold,
                kTextBody,
                col_x,
                geometry.side.y + 14.0F,
                TruncateToWidth(font_cache,
                                NovaFontFamily::kGeneva,
                                10.0F,
                                kNovaFontStyleBold,
                                sys_name,
                                col_w));
  NovaText_Draw(platform,
                font_cache,
                NovaFontFamily::kGeneva,
                10.0F,
                kNovaFontStyleRegular,
                status_color,
                col_x,
                geometry.side.y + 28.0F,
                status);
  // Divider under the column header (the original draws the same rule at the
  // top of the entry-6 pane).
  SDL_SetRenderDrawColor(
      renderer, kPanelBorder.r, kPanelBorder.g, kPanelBorder.b, 120);
  SDL_RenderLine(renderer,
                 geometry.side.x + 4.0F,
                 geometry.side.y + 36.0F,
                 geometry.side.x + geometry.side.w - 4.0F,
                 geometry.side.y + 36.0F);
  NovaText_Draw(platform,
                font_cache,
                NovaFontFamily::kGeneva,
                10.0F,
                kNovaFontStyleRegular,
                kTextBody,
                col_x,
                geometry.side.y + 46.0F,
                fit("Gov: " + govt));
  NovaText_Draw(platform,
                font_cache,
                NovaFontFamily::kGeneva,
                10.0F,
                kNovaFontStyleRegular,
                kTextBody,
                col_x,
                geometry.side.y + 60.0F,
                "Jumps: " + std::to_string(jump_count));

  // ---- Bottom bar: selected-system line + control hints. ----
  NovaText_Draw(platform,
                font_cache,
                NovaFontFamily::kGeneva,
                11.0F,
                kNovaFontStyleRegular,
                kTextBody,
                geometry.bar.x + 10.0F,
                geometry.bar.y + 24.0F,
                search_query ? "Find: " + *search_query + "_"
                             : "Selected System: " +
                                   TruncateToWidth(
                                       font_cache,
                                       NovaFontFamily::kGeneva,
                                       11.0F,
                                       kNovaFontStyleRegular,
                                       sys_name,
                                       static_cast<int>(geometry.bar.w) - 220));
  NovaText_DrawCentered(platform,
                        font_cache,
                        NovaFontFamily::kGeneva,
                        10.0F,
                        kNovaFontStyleRegular,
                        kTextDim,
                        geometry.bar.x,
                        geometry.bar.x + geometry.bar.w,
                        geometry.bar.y + 36.0F,
                        "Arrows pan   +/- zoom   h home/fit   Tab/\\ "
                        "select   Enter/Esc close");
}

// Loads a bottom-button label from STR# 0x96 ("button labels", the same pool
// the ship-comm/negotiation buttons use), with a hardcoded fallback so the row
// stays legible when the pool is missing. The Show Borders button toggles
// between the Show/Hide Borders entries with the political-overlay state.
std::string StarmapButtonLabel(StarmapButton button, bool show_borders) {
  std::uint16_t index = 0;
  switch (button) {
  case StarmapButton::kShowBorders:
    index = show_borders ? 56 : 55; // Hide Borders / Show Borders
    break;
  case StarmapButton::kClearRoute:
    index = 48;
    break;
  case StarmapButton::kFind:
    index = 59;
    break;
  case StarmapButton::kZoomOut:
    index = 16; // '-'
    break;
  case StarmapButton::kZoomIn:
    index = 17; // '+'
    break;
  case StarmapButton::kDone:
    index = 4;
    break;
  }
  if (auto s = NovaHud_LoadStringEntry(0x96, index)) {
    return *s;
  }
  switch (button) {
  case StarmapButton::kShowBorders:
    return show_borders ? "Hide Borders" : "Show Borders";
  case StarmapButton::kClearRoute:
    return "Clear Route";
  case StarmapButton::kFind:
    return "Find";
  case StarmapButton::kZoomOut:
    return "-";
  case StarmapButton::kZoomIn:
    return "+";
  case StarmapButton::kDone:
    return "Done";
  }
  return "?";
}

// Draws the bottom button row (DITL items 8/7/9/3/4/0 in screen order) with
// the house three-state button art -- the same PICT strips (0x1d4c..0x1d54)
// the ship-comm / negotiation / landed windows use -- plus the STR# 0x96
// label. The zoom buttons grey out at the zoom limits (mirroring the
// original's DAT_007dc742/743 gating), the button under the mouse uses the
// pressed ("click") art like the other dialog rows, and the Show Borders
// label reflects the political-overlay state.
void DrawButtons(SdlPlatform &platform,
                 NovaFontCache &font_cache,
                 const ServicesButtonArt &button_art,
                 const StarmapGeometry &geometry,
                 bool show_borders,
                 int zoom_level,
                 std::optional<std::size_t> hovered) {
  // Labels follow the original three-state button palette -- white on the
  // normal art, 50% grey on the pressed/hover and grey/disabled art
  // (NovaUi_InitThreeStateButtonArt DAT_007d8350) -- in the plain screen font
  // the shared renderer uses (no bold). The earlier light-blue bold labels were
  // a divergence.
  constexpr SDL_Color kButtonLabelNormal{255, 255, 255, 255};
  constexpr SDL_Color kButtonLabelGrey{128, 128, 128, 255};
  const std::array<std::pair<StarmapButton, bool>, 6> buttons{{
      {StarmapButton::kShowBorders, true},
      {StarmapButton::kClearRoute, true},
      {StarmapButton::kFind, true},
      {StarmapButton::kZoomOut, zoom_level > kZoomLevelMin},
      {StarmapButton::kZoomIn, zoom_level < kZoomLevelMax},
      {StarmapButton::kDone, true},
  }};
  for (std::size_t i = 0; i < geometry.buttons.size(); ++i) {
    const auto [button, enabled] = buttons[i];
    const bool hovered_by_mouse = enabled && hovered == i;
    const ButtonState state =
        !enabled
            ? ButtonState::kDisabled
            : (hovered_by_mouse ? ButtonState::kHover : ButtonState::kNormal);
    button_art.Draw(platform, geometry.buttons[i], state);
    const SDL_Color &label_color =
        !enabled || hovered_by_mouse ? kButtonLabelGrey : kButtonLabelNormal;
    NovaText_DrawCentered(platform,
                          font_cache,
                          kThreeStateButtonFontFamily,
                          kThreeStateButtonFontSize,
                          kNovaFontStyleRegular,
                          label_color,
                          geometry.buttons[i].x,
                          geometry.buttons[i].x + geometry.buttons[i].w,
                          ThreeStateButtonLabelBaseline(geometry.buttons[i]),
                          StarmapButtonLabel(button, show_borders));
  }
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
  // Resolve the window + map viewport from the DLOG/DITL resources and load the
  // PICT 0x213d backdrop (falling back to a clean bordered placeholder when the
  // resource can't be decoded). The map graph draws into geometry.map.
  const StarmapGeometry geometry = ResolveStarmapGeometry();
  auto backdrop = LoadStarmapBackdrop(platform);
  if (backdrop != nullptr) {
    NovaLog::Info("starmap backdrop PICT 0x213d {}x{} loaded",
                  static_cast<int>(geometry.window.w),
                  static_cast<int>(geometry.window.h));
  } else {
    NovaLog::Warn(
        "starmap backdrop PICT 0x213d unavailable; using placeholder");
  }
  const SDL_FRect panel = geometry.map;
  // The bottom button row uses the same three-state PICT strips as the other
  // dialog rows (ServicesButtonArt 0x1d4c..0x1d54). Initialize once per
  // window; a failure only costs the hover/dim styling (the art falls back to
  // a solid fill internally).
  ServicesButtonArt button_art;
  if (!button_art.Initialize(platform)) {
    NovaLog::Warn("three-state button art unavailable for the starmap buttons");
  }

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
  // Political-overlay toggle (the original's Show/Hide Borders button, action
  // 9): strengthens the per-system government tint on the map. The original
  // draws a per-cell government grid over the map; the clean-room uses a
  // marker-tint stand-in (see DrawGalaxy).
  bool show_borders = false;
  // Inline name-prefix search (the Find button, action 10). The original runs
  // a modal search dialog (NovaUi_RunStarmapSearchDialog, DLOG 0xbbd); the
  // clean-room implements the same prefix match inline: typed characters build
  // a query and the first explored system whose name starts with it is
  // selected, Esc cancels, Enter commits.
  bool search_active = false;
  std::string search_query;

  // Finds the first explored system whose name starts with `query`
  // (case-insensitive) and selects it. Mirrors the original dialog's
  // "best matching visible system name prefix" behaviour.
  const auto search_select = [&](const std::string &query) {
    if (query.empty()) {
      return;
    }
    const auto &systems = state.scenario.systems;
    std::string lower = query;
    std::transform(
        lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
          return static_cast<char>(std::tolower(c));
        });
    for (std::size_t i = 0; i < systems.size(); ++i) {
      if (!SystemExplored(state, static_cast<std::int16_t>(i))) {
        continue;
      }
      std::string name = systems[i].name;
      std::transform(
          name.begin(), name.end(), name.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
          });
      if (name.size() >= lower.size() &&
          name.compare(0, lower.size(), lower) == 0) {
        selected_id = static_cast<std::int16_t>(i);
        return;
      }
    }
  };

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
    DrawChrome(platform, geometry, backdrop ? backdrop->get() : nullptr);
    DrawGalaxy(platform,
               font_cache,
               state,
               mapped,
               view,
               geometry,
               selected_id,
               show_borders);
    DrawSidePanels(platform,
                   font_cache,
                   state,
                   geometry,
                   selected_id,
                   search_active ? &search_query : nullptr);
    // Hovered bottom-row button (pressed-art highlight, like the other dialog
    // button rows).
    std::optional<std::size_t> hovered_button;
    {
      const SDL_FPoint mp = platform.mouse_position();
      for (std::size_t i = 0; i < geometry.buttons.size(); ++i) {
        const SDL_FRect &b = geometry.buttons[i];
        if (mp.x >= b.x && mp.x < b.x + b.w && mp.y >= b.y &&
            mp.y < b.y + b.h) {
          hovered_button = i;
          break;
        }
      }
    }
    DrawButtons(platform,
                font_cache,
                button_art,
                geometry,
                show_borders,
                zoom_level,
                hovered_button);
    SDL_RenderPresent(platform.renderer());

    // Process one batch of raw editable keys / clicks.
    for (std::optional<TextInput> in; (in = platform.PollTextEvent());) {
      switch (in->key) {
      case TextKey::escape:
        if (search_active) {
          search_active = false;
          search_query.clear();
        } else {
          return close_with_selection();
        }
        break;

      case TextKey::enter:
        if (search_active) {
          // Commit the find result (the matched system stays selected).
          search_active = false;
        } else {
          // Selecting the current system's own node (or the current default)
          // is a no-op navigation-wise; Enter confirms the selection and
          // closes.
          return close_with_selection();
        }
        break;

      case TextKey::character: {
        const char ch = in->character;
        if (search_active) {
          // While searching, typed characters build the query prefix (q/x/+/-
          // are ordinary query characters, not shortcuts).
          search_query.push_back(ch);
          search_select(search_query);
          break;
        }
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
        const SDL_FPoint mp = platform.mouse_position();
        // Bottom button row (DITL items 8/7/9/3/4/0): a click inside a button
        // runs its action and consumes the click so the map doesn't also
        // select a system underneath. Actions mirror
        // NovaUi_StarmapWindowInnerLoop's dispatch (action = 1-based entry:
        // 9 borders, 8 clear route, 10 find, 4 zoom out, 5 zoom in, 1 done).
        bool button_clicked = false;
        for (std::size_t i = 0; i < geometry.buttons.size(); ++i) {
          const SDL_FRect &b = geometry.buttons[i];
          if (mp.x >= b.x && mp.x < b.x + b.w && mp.y >= b.y &&
              mp.y < b.y + b.h) {
            button_clicked = true;
            switch (static_cast<StarmapButton>(i)) {
            case StarmapButton::kShowBorders:
              show_borders = !show_borders;
              break;
            case StarmapButton::kClearRoute:
              // Clears the plotted starmap route (the original resets the
              // route array to just the current system); the clean-room clears
              // the single plotted destination and its armed travel slot.
              state.travel.starmap_destination_system_id = -1;
              state.travel.travel_slot = -1;
              selected_id = state.player.current_system_id;
              break;
            case StarmapButton::kFind:
              search_active = true;
              search_query.clear();
              break;
            case StarmapButton::kZoomOut:
              if (zoom_level > kZoomLevelMin) {
                zoom_to_level(zoom_level - 1);
              }
              break;
            case StarmapButton::kZoomIn:
              if (zoom_level < kZoomLevelMax) {
                zoom_to_level(zoom_level + 1);
              }
              break;
            case StarmapButton::kDone:
              return close_with_selection();
            }
            break;
          }
        }
        if (button_clicked) {
          break;
        }
        // Select the nearest *discovered* marker within a click radius; empty
        // click keeps the current selection. Undiscovered systems have no map
        // presence, so they cannot be picked either.
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
          // A direct map pick also exits an active Find search.
          search_active = false;
        }
        break;
      }

      case TextKey::backspace:
        if (search_active && !search_query.empty()) {
          search_query.pop_back();
          search_select(search_query);
        }
        break;

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
