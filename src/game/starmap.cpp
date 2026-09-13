#include "starmap.hpp"

#include "../brgr_archive.hpp"
#include "../cicn_image.hpp"
#include "../log.hpp"
#include "../pict_image.hpp"
#include "hud_overlay.hpp"
#include "hud_renderer.hpp"
#include "mission.hpp"
#include "nova_font.hpp"
#include "scenario_data.hpp"
#include "services_buttons.hpp"
#include "spaceflight_view.hpp"
#include "targeting.hpp"
#include "travel.hpp"

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

// ---- Window geometry (logical 640x480 playfield) --------------------------
// The starmap window is the EV Nova DLOG 0x7d0 dialog: a 601x513 frame whose
// backdrop is the PICT 0x213d "Map" starfield, blitted across the whole window
// (DAT_007dc73c in NovaUi_RunStarmapWindow 0x004a3aa0). Only the galaxy-graph
// viewport (DITL item 2 / UiPanel_GetEntryInfo entry 3) is filled opaque with
// the system-space background colour; the side column (item 5 / entry 6) and
// the bottom status bar (item 1 / entry 2) render their text straight onto the
// starfield, separated from the graph by thin rules. The 513-tall window is
// scaled down uniformly (480/513) to fit the field, like the other dialogs.
constexpr int kStarmapWindowW = 601;
constexpr int kStarmapWindowH = 513;
constexpr float kStarmapFitScale =
    static_cast<float>(kLogicalUiHeight) /
    static_cast<float>(kStarmapWindowH); // ~0.936
constexpr float kStarmapWindowX =
    (640.0F - static_cast<float>(kStarmapWindowW) * kStarmapFitScale) / 2.0F;
constexpr float kStarmapWindowY = 0.0F;
constexpr float kMapPanelX = kStarmapWindowX + 9.0F * kStarmapFitScale;
constexpr float kMapPanelY = 8.0F * kStarmapFitScale;
constexpr float kMapPanelW = 458.0F * kStarmapFitScale;
constexpr float kMapPanelH = 420.0F * kStarmapFitScale;
constexpr float kSidePanelX = kStarmapWindowX + 474.0F * kStarmapFitScale;
constexpr float kSidePanelY = 8.0F * kStarmapFitScale;
constexpr float kSidePanelW = 120.0F * kStarmapFitScale;
constexpr float kSidePanelH = 429.0F * kStarmapFitScale;
constexpr float kBottomBarX = kStarmapWindowX + 8.0F * kStarmapFitScale;
constexpr float kBottomBarY = 436.0F * kStarmapFitScale;
constexpr float kBottomBarW = 586.0F * kStarmapFitScale;
constexpr float kBottomBarH = 42.0F * kStarmapFitScale;

// The bottom button row (DITL items 8,7,9,3,4,0 in left-to-right screen
// order; 1-based entries 9,8,10,4,5,1). Each is the item rect in the DITL
// authoring space; the rects are re-derived from the resource at runtime.
struct StarmapButtonRect {
  float x, y, w, h;
};

constexpr std::array<StarmapButtonRect, 6> kStarmapButtonRects{{
    {kStarmapWindowX + 11.0F * kStarmapFitScale,
     kStarmapWindowY + 483.0F * kStarmapFitScale,
     130.0F * kStarmapFitScale,
     25.0F * kStarmapFitScale},
    {kStarmapWindowX + 155.0F * kStarmapFitScale,
     kStarmapWindowY + 483.0F * kStarmapFitScale,
     120.0F * kStarmapFitScale,
     25.0F * kStarmapFitScale},
    {kStarmapWindowX + 288.0F * kStarmapFitScale,
     kStarmapWindowY + 483.0F * kStarmapFitScale,
     99.0F * kStarmapFitScale,
     25.0F * kStarmapFitScale},
    {kStarmapWindowX + 408.0F * kStarmapFitScale,
     kStarmapWindowY + 483.0F * kStarmapFitScale,
     25.0F * kStarmapFitScale,
     25.0F * kStarmapFitScale},
    {kStarmapWindowX + 438.0F * kStarmapFitScale,
     kStarmapWindowY + 483.0F * kStarmapFitScale,
     25.0F * kStarmapFitScale,
     25.0F * kStarmapFitScale},
    {kStarmapWindowX + 483.0F * kStarmapFitScale,
     kStarmapWindowY + 483.0F * kStarmapFitScale,
     99.0F * kStarmapFitScale,
     25.0F * kStarmapFitScale},
}};

// ---- Palette (Ghidra Settings_InitTimingPresets 0x004ad7c0 fills the fixed
// RGBColor triples at DAT_00733b2c..DAT_00733b78; 16-bit components render as
// >>8 in the 8-bit draw paths). PTR_DAT_00575acc targets the black system-
// space colour (map background + marker disc fill); PTR_DAT_00575ad8 and
// PTR_DAT_00575af0 are the static white and cyan triples at 0x575ad0/0x575ae8.
constexpr SDL_Color kColorBlack{0, 0, 0, 255};
constexpr SDL_Color kColorWhite{255, 255, 255, 255};
constexpr SDL_Color kColorCyan{0, 255, 255, 255}; // RGBColor @0x575ae8
constexpr SDL_Color kLinkGrey{78, 78, 78, 255};   // 20000 (DAT_00733b62)
// 4000 grey (DAT_00733b68) -- the link tint in the destination-window route
// mode, which is not reconstructed; kept for reference.
[[maybe_unused]] constexpr SDL_Color kLinkGreyDim{15, 15, 15, 255};
constexpr SDL_Color kRouteGreen{0, 255, 0, 255};   // SHORT_ARRAY_00733b32
constexpr SDL_Color kJumpGreen{0, 153, 0, 255};    // _DAT_00733b3a (0x9900)
constexpr SDL_Color kDestBlue{0, 0, 255, 255};     // StellarDisplayColor
constexpr SDL_Color kDestOrange{255, 102, 0, 255}; // (0xffff, 0x6666, 0)
constexpr SDL_Color kDestRed{255, 0, 0, 255};
constexpr SDL_Color kHeaderGrey{192, 192, 192, 255}; // 0xc000 (0x733b50)
constexpr SDL_Color kTextGrey{64, 64, 64, 255};      // 0x4000 (0x733b5c)

// Zoom model (g_starmap_zoom 0x005759d8, factors at 0x005759f0/f8, enable
// limits at 0x00575a00/08). Zoom is a DIVISOR: screen = world / zoom, so the
// zoom-out key multiplies and zoom-in divides. Both factors multiply the
// stored value (0x004a3fae / 0x004a4035): zoom-out x4/3 while zoom < 2.0
// (0.5625 -> 0.75 -> 1.0 -> 1.333 -> 1.778 -> 2.370), zoom-in x0.75 while
// zoom > 0.5 (one step to 0.4219). Opening clamps a non-positive stored zoom
// back to 1.0 (0x004a3cf1).
constexpr double kZoomInitial = 0.5625;
constexpr double kZoomOutStep = 4.0 / 3.0; // '-' / zoom-out button
constexpr double kZoomInStep = 0.75;       // '+' / zoom-in button
constexpr double kZoomOutLimit = 2.0;      // zoom-out allowed while below
constexpr double kZoomInLimit = 0.5;       // zoom-in allowed while above
// Label threshold (g_starmap_label_zoom_threshold 0x00575a10 = 1.1): names
// draw for every revealed system while zoom <= 1.1; zoomed in past that only
// the selected system keeps its label (0x004a9819).
constexpr double kLabelZoomThreshold = 1.1;

// Marker geometry (NovaUi_DrawStarmapRoutesAndMarkers 0x004a8100). Markers are
// background-coloured filled discs with a 1px status ring inside a rect inset
// by -4 (radius 4) while zoom >= 0.5, or -6 (radius 6) with a 2px ring at the
// innermost zoom step (zoom < 0.5; the tier switch at 0x004a8f63 compares
// g_starmap_zoom against DAT_00575a00 = 0.5 -- zoom is a DIVISOR, so a smaller
// value is zoomed IN, and the markers GROW on the final step in). The current
// system draws a small solid cyan dot afterwards (inset -2 normally, -3 at the
// innermost zoom step), and the selected system gets the green mid-edge tick
// reticle around the -6 rect (-8 at the innermost zoom step, 0x004a51f0 /
// 0x004a5a44).
constexpr float kMarkerInset = 4.0F;
constexpr float kMarkerInsetZoomedIn = 6.0F;
constexpr float kCurrentDotInset = 2.0F;
constexpr float kCurrentDotInsetZoomedIn = 3.0F;
constexpr float kReticleInset = 6.0F;
constexpr float kReticleInsetZoomedIn = 8.0F;
constexpr float kClickInset = 10.0F; // click hit-test rect (Rect_Inset -10)

// Label offsets from the marker centre (DAT_00575a18 / DAT_00575a20).
constexpr float kLabelOffsetX = 7.0F;
constexpr float kLabelOffsetY = 4.0F;

// Political overlay radii (g_starmap_overlay_radius_small/large 0x00575a38/40
// = 11.0 / 22.0 world units) and strength fades (0.4 small / 0.2 large tier).
// The original works in HALF-PIXEL grid cells: radius = round(22/zoom) + 12
// cells, strength = clamp((r^2 - d^2) * fade * zoom, 1, 255).
constexpr double kOverlayRadiusSmall = 11.0;
constexpr double kOverlayRadiusLarge = 22.0;
constexpr double kOverlayFadeSmall = 0.4;
constexpr double kOverlayFadeLarge = 0.2;

// Bottom button order/actions (DITL items 8,7,9,3,4,0 -> entries 9,8,10,4,5,1
// -> NovaUi_StarmapWindowInnerLoop's action dispatch).
enum class StarmapButton : std::size_t {
  kShowBorders = 0, // STR# 0x96 0x38 / 0x39 (Show/Hide Borders)
  kClearRoute = 1,  // STR# 0x96 0x31
  kFind = 2,        // STR# 0x96 0x3c
  kZoomOut = 3,     // STR# 0x96 0x11 '-'
  kZoomIn = 4,      // STR# 0x96 0x12 '+'
  kDone = 5,        // STR# 0x96 0x5
};

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
struct MapView {
  float zoom = static_cast<float>(kZoomInitial);
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

// A projected marker plus its zero-based scenario id, shared by the draw and
// hit-test passes.
struct MappedSystem {
  std::int16_t zero_based_id = -1;
  float sx = 0.0F;
  float sy = 0.0F;
};

// Ghidra 0x0046b9b0 System_ResolveSystemDiscoverySlot: a system's fog record
// lives on its visibility-group root (the loader's twin grouping, 0x004bd3c0).
[[nodiscard]] std::int16_t DiscoverySlot(const GameState &state,
                                         std::int16_t system_id) {
  if (system_id < 0 ||
      static_cast<std::size_t>(system_id) >= state.scenario.systems.size()) {
    return -1;
  }
  const std::int16_t root =
      state.scenario.systems[static_cast<std::size_t>(system_id)]
          .visibility_root_system_id;
  return root != -1 ? root : system_id;
}

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

// STR# 0x7d2 / 0x86 / 0xfa0 strings the map draws, cached at window open.
// Ghidra NovaData_LoadDisplayNamePstringTables 0x004c7040 fills the Pascal-
// string tables at startup from these pools; entry numbers are 1-based,
// exactly as the original passes them to Resource_LoadStringEntry.
struct StarmapStrings {
  std::string ports;                        // 0x7d2 0x135 "Ports:"
  std::string unknown;                      // 0x136 "<Unknown>"
  std::string nav_hazards;                  // 0x137 "Navigation Hazards:"
  std::string gravity_shear;                // 0x138
  std::array<std::string, 3> asteroid;      // 0x139-0x13b
  std::string asteroid_noun;                // 0x13c "asteroid field"
  std::array<std::string, 3> interference;  // 0x13d-0x13f
  std::string interference_noun;            // 0x140
  std::array<std::string, 3> visibility;    // 0x141-0x143
  std::string visibility_noun;              // 0x144
  std::string government;                   // 0x145 "Government:"
  std::string legal_status;                 // 0x146 "Legal Status:"
  std::string goods_traded;                 // 0x147 "Goods Traded:"
  std::string services;                     // 0x148 "Services:"
  std::array<std::string, 3> service_names; // 0x149-0x14b
  std::string none_available;               // 0x14c "None Available"
  std::string independent;                  // 0x14d "Independent"
  std::string uninhabited;                  // 0x14e "Uninhabited System"
  std::string none;                         // 0x14f "None"
  std::string current_system;               // 0x151 "Current System:"
  std::string selected_system;              // 0x152 "Selected System:"
  std::string destination_system;           // 0x153 "Destination System:"
  std::string hypergate_destination;        // 0x154 "Hypergate Destination:"
  std::string na;                           // 0x18c "N/A"
  std::array<std::string, 18> legal_table;  // STR# 0x86 entries 1..18
  std::array<std::string, 6> goods_classes; // STR# 0xfa0 entries 1..6
};

std::string LoadStringOr(std::uint16_t resource_id,
                         std::uint16_t entry,
                         std::string_view fallback) {
  if (auto s = NovaHud_LoadStringEntry(resource_id, entry)) {
    return std::move(*s);
  }
  return std::string(fallback);
}

StarmapStrings LoadStarmapStrings() {
  StarmapStrings s;
  s.ports = LoadStringOr(0x7d2, 0x135, "Ports:");
  s.unknown = LoadStringOr(0x7d2, 0x136, "<Unknown>");
  s.nav_hazards = LoadStringOr(0x7d2, 0x137, "Navigation Hazards:");
  s.gravity_shear = LoadStringOr(0x7d2, 0x138, "gravity shear");
  s.asteroid = {LoadStringOr(0x7d2, 0x139, "sparse"),
                LoadStringOr(0x7d2, 0x13a, "moderate"),
                LoadStringOr(0x7d2, 0x13b, "dense")};
  s.asteroid_noun = LoadStringOr(0x7d2, 0x13c, "asteroid field");
  s.interference = {LoadStringOr(0x7d2, 0x13d, "light"),
                    LoadStringOr(0x7d2, 0x13e, "moderate"),
                    LoadStringOr(0x7d2, 0x13f, "heavy")};
  s.interference_noun = LoadStringOr(0x7d2, 0x140, "interference");
  s.visibility = {LoadStringOr(0x7d2, 0x141, "reduced"),
                  LoadStringOr(0x7d2, 0x142, "severely reduced"),
                  LoadStringOr(0x7d2, 0x143, "greatly reduced")};
  s.visibility_noun = LoadStringOr(0x7d2, 0x144, "visibility");
  s.government = LoadStringOr(0x7d2, 0x145, "Government:");
  s.legal_status = LoadStringOr(0x7d2, 0x146, "Legal Status:");
  s.goods_traded = LoadStringOr(0x7d2, 0x147, "Goods Traded:");
  s.services = LoadStringOr(0x7d2, 0x148, "Services:");
  s.service_names = {LoadStringOr(0x7d2, 0x149, "Trading"),
                     LoadStringOr(0x7d2, 0x14a, "Outfitting"),
                     LoadStringOr(0x7d2, 0x14b, "Shipyard")};
  s.none_available = LoadStringOr(0x7d2, 0x14c, "None Available");
  s.independent = LoadStringOr(0x7d2, 0x14d, "Independent");
  s.uninhabited = LoadStringOr(0x7d2, 0x14e, "Uninhabited System");
  s.none = LoadStringOr(0x7d2, 0x14f, "None");
  s.current_system = LoadStringOr(0x7d2, 0x151, "Current System:");
  s.selected_system = LoadStringOr(0x7d2, 0x152, "Selected System:");
  s.destination_system = LoadStringOr(0x7d2, 0x153, "Destination System:");
  s.hypergate_destination =
      LoadStringOr(0x7d2, 0x154, "Hypergate Destination:");
  s.na = LoadStringOr(0x7d2, 0x18c, "N/A");
  // Legal-status ladder (DAT_005dafcc <- STR# 0x86) and the six trade-good
  // class names (DAT_0067cccc <- STR# 0xfa0).
  static constexpr std::string_view kLegalFallback[18]{"No Record",
                                                       "No Record",
                                                       "No Convictions",
                                                       "Minor Offender",
                                                       "Offender",
                                                       "Criminal",
                                                       "Wanted Criminal",
                                                       "Fugitive",
                                                       "Hunted Fugitive",
                                                       "Public Enemy",
                                                       "Citizen",
                                                       "Good Citizen",
                                                       "Upstanding Citizen",
                                                       "Leading Citizen",
                                                       "Model Citizen",
                                                       "Virtuous Citizen",
                                                       "Military Dictator",
                                                       "Military Governor"};
  for (std::size_t i = 0; i < s.legal_table.size(); ++i) {
    s.legal_table[i] = LoadStringOr(
        0x86, static_cast<std::uint16_t>(i + 1), kLegalFallback[i]);
  }
  static constexpr std::string_view kGoodsFallback[6]{"Food",
                                                      "Industrial",
                                                      "Medical Supplies",
                                                      "Luxury Goods",
                                                      "Metal",
                                                      "Equipment"};
  for (std::size_t i = 0; i < s.goods_classes.size(); ++i) {
    s.goods_classes[i] = LoadStringOr(
        0xfa0, static_cast<std::uint16_t>(i + 1), kGoodsFallback[i]);
  }
  return s;
}

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
    if (st->hazard_marker) {
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

// Loads the starmap backdrop PICT (0x213d "Map", DAT_007dc73c).
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

// One nebula's 7 zoom-tier images. PICT 0x251c + nebula*7 + tier (loaded by
// NovaUi_RunStarmapWindow for every populated trigger slot), ascending size.
struct NebulaTier {
  std::unique_ptr<SdlTexture> texture;
  int width = 0;
  int height = 0;
};

using NebulaTiers = std::array<NebulaTier, 7>;

// The mission-target marker icons are color icons loaded together by the
// starmap asset loader (0x004aea40: Resource_LoadCicn 0x3a98 -> DAT_007dc3b0,
// 0x3a99 -> DAT_007dc3b4). In the marker pass (0x004a8100) the red 0x3a98
// arrow draws up-left of mission-target systems, suppressed on the selected
// system; the green 0x3a99 arrow draws up-right of the selected system (the
// DAT_007dc745 briefing-map latch only controls that selection clears the
// mission-target override — it does not gate the green arrow, which draws in
// the plain flight map too).
// (NovaStarmap_MarkerIcons lives in starmap.hpp so the route-map overlay can
// share the loaded CICN textures.)
using MarkerIcons = NovaStarmap_MarkerIcons;

std::unique_ptr<SdlTexture> LoadCicnTexture(SdlPlatform &platform,
                                            std::uint16_t resource_id) {
  const auto data = NovaResource_Load(kResourceTypeCicn, resource_id);
  if (!data) {
    return {};
  }
  const auto img = Resource_LoadCicnAsImage(*data);
  if (!img) {
    return {};
  }
  return SdlTexture::Create(
      platform.renderer(), img->width, img->height, img->rgba_pixels);
}

MarkerIcons LoadMarkerIcons(SdlPlatform &platform) {
  return NovaStarmap_LoadMarkerIcons(platform);
}

// Resolves the starmap window + panes + button row from the DLOG 0x7d0 /
// DITL 0x7d0 dialog resources. The hardcoded rects above are the verified
// fallback when the resource cannot be parsed.
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
  const std::array<std::size_t, 6> button_items{8, 7, 9, 3, 4, 0};
  for (std::size_t i = 0; i < button_items.size(); ++i) {
    const std::size_t item_idx = button_items[i];
    if (items->size() > item_idx && valid((*items)[item_idx])) {
      g.buttons[i] = offset_rect((*items)[item_idx]);
    }
  }
  return g;
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

void DrawRule(SDL_Renderer *renderer, float x0, float x1, float y) {
  SDL_SetRenderDrawColor(
      renderer, kColorBlack.r, kColorBlack.g, kColorBlack.b, SDL_ALPHA_OPAQUE);
  SDL_RenderLine(renderer, x0, y, x1, y);
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
struct PoliticalOverlay {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> rgba;
};

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
    if (gv == nullptr || (gv->scan_mask_short & 0x0004U) != 0U) {
      continue;
    }
    if (!NovaSystem_HasUsableTravelDestination(state, id)) {
      continue;
    }
    const bool small_tier = (gv->scan_mask_short & 0x0002U) != 0U;
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

// ---- Nebula backdrop images ------------------------------------------------

// Sets up the clip rect for the galaxy viewport (defined with the fixed
// chrome below).
void ClipToMapPanel(SDL_Renderer *renderer, const SDL_FRect &panel);

NebulaTiers LoadNebulaTiers(SdlPlatform &platform, std::size_t nebula_index) {
  NebulaTiers tiers;
  for (std::size_t tier = 0; tier < tiers.size(); ++tier) {
    const auto resource_id =
        static_cast<std::uint16_t>(0x251c + nebula_index * 7 + tier);
    const auto data = NovaResource_LoadPictData(resource_id);
    if (!data) {
      continue;
    }
    const auto img = Resource_LoadPictAsImage(*data);
    if (!img) {
      continue;
    }
    tiers[tier].texture = SdlTexture::Create(
        platform.renderer(), img->width, img->height, img->rgba_pixels);
    tiers[tier].width = img->width;
    tiers[tier].height = img->height;
  }
  return tiers;
}

// Picks the nebula image for the projected rect the way the redraw does
// (0x004a5560 loop): the first tier (ascending size) tall enough, else the
// first one wide enough, else the largest available; then blits it stretched
// into the projected rect (clipped by the map scissor).
void DrawNebulae(SdlPlatform &platform,
                 const GameState &state,
                 const std::vector<NebulaTiers> &nebula_images,
                 const MapView &view,
                 const SDL_FRect &panel) {
  SDL_Renderer *renderer = platform.renderer();
  ClipToMapPanel(renderer, panel);
  const auto restore_clip = [&]() { SDL_SetRenderClipRect(renderer, nullptr); };
  for (std::size_t i = 0;
       i < state.scenario.nebulae.size() && i < nebula_images.size();
       ++i) {
    const Nebula &neb = state.scenario.nebulae[i];
    // Redraw gate (0x004a5400): populated rect, ActiveOn satisfied and the
    // explored latch set (a visited system fell inside).
    if (neb.width == 0 || neb.height == 0 || !neb.active_on || !neb.explored) {
      continue;
    }
    const NebulaTiers &tiers = nebula_images[i];
    const SDL_FPoint top_left = view.Project(
        panel.w, panel.h, static_cast<float>(neb.x), static_cast<float>(neb.y));
    const SDL_FPoint bottom_right =
        view.Project(panel.w,
                     panel.h,
                     static_cast<float>(neb.x + neb.width),
                     static_cast<float>(neb.y + neb.height));
    const SDL_FRect dst{panel.x + top_left.x,
                        panel.y + top_left.y,
                        bottom_right.x - top_left.x,
                        bottom_right.y - top_left.y};
    if (dst.w < 1.0F || dst.h < 1.0F) {
      continue;
    }
    std::size_t chosen = tiers.size();
    std::size_t fallback = tiers.size();
    for (std::size_t t = 0; t < tiers.size(); ++t) {
      if (!tiers[t].texture) {
        continue;
      }
      const float iw = static_cast<float>(tiers[t].width);
      const float ih = static_cast<float>(tiers[t].height);
      if (dst.h <= ih) {
        chosen = t;
        break;
      }
      fallback = t;
      if (dst.w <= iw) {
        chosen = t;
        break;
      }
    }
    if (chosen >= tiers.size()) {
      chosen = fallback;
    }
    if (chosen >= tiers.size() || !tiers[chosen].texture) {
      continue;
    }
    SDL_RenderTexture(renderer, tiers[chosen].texture->get(), nullptr, &dst);
  }
  restore_clip();
}

// ---- Fixed chrome ----------------------------------------------------------

void ClipToMapPanel(SDL_Renderer *renderer, const SDL_FRect &panel) {
  const SDL_Rect clip{static_cast<int>(panel.x),
                      static_cast<int>(panel.y),
                      static_cast<int>(panel.w),
                      static_cast<int>(panel.h)};
  SDL_SetRenderClipRect(renderer, &clip);
}

// Backdrop used when no live flight view is available (map opened from the
// docked menu / text reader): opaque background + the PICT 0x213d window.
void DrawChrome(SdlPlatform &platform,
                const StarmapGeometry &geometry,
                SDL_Texture *backdrop) {
  SDL_Renderer *renderer = platform.renderer();
  SDL_SetRenderDrawColor(
      renderer, kColorBlack.r, kColorBlack.g, kColorBlack.b, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(renderer);
  platform.SetCenteredPlayfield();

  // The original fills the window with the background colour, then blits the
  // starfield PICT across the whole frame (0x004a5240).
  if (backdrop != nullptr) {
    SDL_RenderTexture(renderer, backdrop, nullptr, &geometry.window);
  } else {
    SDL_SetRenderDrawColor(renderer, 8, 12, 24, SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(renderer, &geometry.window);
  }

  // Deliberate divergence for this fallback path only: the viewport is filled
  // opaque so the map reads over a plain backdrop. When the map opens from
  // spaceflight, DrawLiveChrome keeps the flight view visible instead (the
  // original's modal window composites over the live frame).
  SDL_SetRenderDrawColor(
      renderer, kColorBlack.r, kColorBlack.g, kColorBlack.b, SDL_ALPHA_OPAQUE);
  SDL_RenderFillRect(renderer, &geometry.map);
  DrawRule(renderer,
           geometry.bar.x + 1.0F,
           geometry.bar.x + geometry.bar.w - 2.0F,
           geometry.bar.y);
  DrawRule(renderer,
           geometry.side.x + 1.0F,
           geometry.side.x + geometry.side.w - 2.0F,
           geometry.side.y - 1.0F);
}

// Live-flight backdrop (spaceflight caller): the full game frame (world +
// HUD) keeps rendering behind the modal window, the PICT 0x213d window chrome
// blits over it, and the galaxy viewport itself is filled opaque black - the
// map area does NOT show the flight view through.
void DrawLiveChrome(SdlPlatform &platform,
                    const GameState &state,
                    const StarmapGeometry &geometry,
                    SDL_Texture *backdrop,
                    SpaceflightView &view,
                    HudRenderer &hud) {
  SDL_Renderer *renderer = platform.renderer();
  view.DrawGameFrame(platform, state, hud);
  platform.SetCenteredPlayfield();
  if (backdrop != nullptr) {
    SDL_RenderTexture(renderer, backdrop, nullptr, &geometry.window);
  }
  SDL_SetRenderDrawColor(
      renderer, kColorBlack.r, kColorBlack.g, kColorBlack.b, SDL_ALPHA_OPAQUE);
  SDL_RenderFillRect(renderer, &geometry.map);
  DrawRule(renderer,
           geometry.bar.x + 1.0F,
           geometry.bar.x + geometry.bar.w - 2.0F,
           geometry.bar.y);
  DrawRule(renderer,
           geometry.side.x + 1.0F,
           geometry.side.x + geometry.side.w - 2.0F,
           geometry.side.y - 1.0F);
}

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
                const MarkerIcons &icons,
                float alpha = 1.0F) {
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
    const std::int16_t slot = DiscoverySlot(state, m.zero_based_id);
    const bool is_mission_target = std::any_of(
        mission_targets.begin(),
        mission_targets.end(),
        [&](std::int16_t target) {
          return target != -1 && DiscoverySlot(state, target) == slot;
        });
    const bool is_selected =
        selected_id >= 0 && slot == DiscoverySlot(state, selected_id);
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

// ---- Info panes ------------------------------------------------------------

// Ghidra 0x00468d90 NovaUi_DrawSystemFactionConflictStatus: the selected
// system's Legal Status text, graded from the system's reputation against the
// owning government's flee threshold (t). The ladder (levels 1..15) is the
// binary's exact threshold chain; a hazard-marked destination among the first
// three nav stellars overrides to 0x10/0x11 (the STR# 0x86 pool's trailing
// "Military Dictator"/"Military Governor" entries -- odd but verbatim), and
// governments with flags_primary bit 0 suppress the status to "N/A".
[[nodiscard]] std::string LegalStatusText(const GameState &state,
                                          std::int16_t zero_based_id,
                                          const StarmapStrings &strings) {
  const System &sys =
      state.scenario.systems[static_cast<std::size_t>(zero_based_id)];
  const std::int16_t rep =
      zero_based_id < static_cast<std::int16_t>(state.system_reputation.size())
          ? state.system_reputation[static_cast<std::size_t>(zero_based_id)]
          : 0;
  const Government *gov = nullptr;
  if (sys.government_id >= 0 && sys.government_id < 0x100) {
    gov = state.scenario.Government(
        static_cast<std::int16_t>(sys.government_id + 0x80));
  }
  // The original falls back to the first government table entry's CrimeTol
  // (GovtDef 0x46, payload +0x08) when the system has no government.
  const std::int16_t t =
      gov != nullptr ? gov->crime_tol
                     : (!state.scenario.governments.empty()
                            ? state.scenario.governments.front().crime_tol
                            : 0);
  const std::int32_t ti = t;
  int level = 0;
  const auto rep32 = static_cast<std::int32_t>(rep);
  if (rep32 < 0) {
    level = 2;
  }
  if (rep32 < -ti) {
    level = 3;
  }
  if (rep32 < -ti * 4) {
    level = 4;
  }
  if (rep32 < -ti * 16) {
    level = 5;
  }
  if (rep32 < -ti * 64) {
    level = 6;
  }
  if (rep32 < -ti * 256) {
    level = 7;
  }
  if (rep32 < -ti * 1024) {
    level = 8;
  }
  if (rep32 < -ti * 4096) {
    level = 9;
  }
  if (rep32 == 0) {
    level = 1;
  }
  if (rep32 > 0) {
    level = 10;
  }
  if (rep32 != ti * 4 && rep32 > ti * 4) {
    level = 11;
  }
  if (rep32 > ti * 16) {
    level = 12;
  }
  if (rep32 > ti * 64) {
    level = 13;
  }
  if (rep32 > ti * 256) {
    level = 14;
  }
  if (rep32 > ti * 1024) {
    level = 15;
  }
  // Hazard override: any hazard-marked usable destination among the first
  // three nav stellars (0x00468e9d count loop).
  int non_hazard = 0;
  int hazard = 0;
  for (std::size_t i = 0; i < 3 && i < sys.nav_defs.size(); ++i) {
    const std::int16_t nav = sys.nav_defs[i];
    if (nav < 0x80) {
      continue;
    }
    const Stellar *st = state.scenario.Stellar(nav);
    if (st == nullptr || !st->is_available || (st->flags & 0x20U) != 0U ||
        !NovaTargeting_IsStellarUsableForTravel(*st)) {
      continue;
    }
    if (st->hazard_marker) {
      ++hazard;
    } else {
      ++non_hazard;
    }
  }
  if (hazard > 0) {
    level = non_hazard < 1 ? 0x11 : 0x10;
  }
  if (gov != nullptr && (gov->flags_primary & 0x0001U) != 0U) {
    level = 0;
  }
  if (level == 0) {
    return strings.na;
  }
  return strings.legal_table[static_cast<std::size_t>(level)];
}

// Ghidra 0x00469d30 Stellar_ExtractTravelFlagConnectiveValue: the six
// trade-class lanes packed into the stellar flags word, class i at bits
// (28 - 4*i) .. (30 - 4*i); any nonzero lane means the class is traded.
[[nodiscard]] bool StellarTradesClass(std::uint32_t flags, int class_index) {
  const std::uint32_t lane = 0xE0000000UL >> (4 * class_index);
  return (flags & lane) != 0U;
}

// Ghidra 0x00468210 Stellar_HasAdjacentAccessibleStellarMatchingTravelFlags:
// any usable nav stellar that trades `class_index` (flags bit 1 set + lane).
[[nodiscard]] bool
SystemTradesClass(const GameState &state, const System &sys, int class_index) {
  for (const std::int16_t nav : sys.nav_defs) {
    if (nav < 0x80) {
      continue;
    }
    const Stellar *st = state.scenario.Stellar(nav);
    if (st == nullptr || !st->is_available || (st->flags & 0x20U) != 0U ||
        !NovaTargeting_IsStellarUsableForTravel(*st)) {
      continue;
    }
    if ((st->flags & 0x2U) != 0U &&
        StellarTradesClass(st->flags, class_index)) {
      return true;
    }
  }
  return false;
}

// Ghidra 0x004682d0 Stellar_HasAdjacentAccessibleStellarForTravelMode:
// any usable nav stellar with the mode's service bit (2 Trading, 4
// Outfitting, 8 Shipyard).
[[nodiscard]] bool
SystemHasService(const GameState &state, const System &sys, int mode) {
  const std::uint32_t bit = 2U << mode;
  for (const std::int16_t nav : sys.nav_defs) {
    if (nav < 0x80) {
      continue;
    }
    const Stellar *st = state.scenario.Stellar(nav);
    if (st == nullptr || !st->is_available || (st->flags & 0x20U) != 0U ||
        !NovaTargeting_IsStellarUsableForTravel(*st)) {
      continue;
    }
    if ((st->flags & bit) != 0U) {
      return true;
    }
  }
  return false;
}

namespace {

void DrawTextAt(SdlPlatform &platform,
                NovaFontCache &font_cache,
                float x,
                float y,
                float point_size,
                SDL_Color color,
                const std::string &text) {
  NovaText_Draw(platform,
                font_cache,
                NovaFontFamily::kGeneva,
                point_size,
                kNovaFontStyleRegular,
                color,
                x,
                y,
                text);
}

} // namespace

// The right-hand detail column (DITL item 5 / entry 6) and the bottom status
// bar (DITL item 1 / entry 2), reproducing NovaUi_RedrawStarmapWindow's
// 0x004a6219..0x004a6d40 text layout (offsets are DITL authoring pixels,
// scaled by the window fit). The original right-aligns the game date in the
// status bar (NovaText_FormatDateString; now drawn).
void DrawSidePanels(SdlPlatform &platform,
                    NovaFontCache &font_cache,
                    const GameState &state,
                    const StarmapGeometry &geometry,
                    const StarmapStrings &strings,
                    std::int16_t selected_id,
                    const std::string *search_query) {
  const float s = kStarmapFitScale;
  const SDL_FRect &side = geometry.side;
  const SDL_FRect &bar = geometry.bar;
  const bool valid_id =
      selected_id >= 0 &&
      static_cast<std::size_t>(selected_id) < state.scenario.systems.size();
  const System &sys =
      valid_id ? state.scenario.systems[static_cast<std::size_t>(selected_id)]
               : System{};
  const bool visited = valid_id && sys.discovery_state >= 1;
  const std::int16_t current = state.player.current_system_id;

  // ---- Side column ----
  // Travel-status header (0x004a6460): the plotted-jump case reads
  // "Destination System:", the current system "Current System:", everything
  // else "Selected System:". The original also has a "Hypergate Destination:"
  // case for the destination-window sub-flow (0x004a6580), which is not
  // reconstructed here.
  std::string header = strings.selected_system;
  if (state.travel.travel_slot >= 0 &&
      state.travel.starmap_destination_system_id != -1) {
    header = strings.destination_system;
  } else if (selected_id == current) {
    header = strings.current_system;
  }
  DrawTextAt(platform,
             font_cache,
             side.x + 10.0F * s,
             side.y + 12.0F * s,
             10.0F,
             kHeaderGrey,
             header);
  // System name (or "<Unknown>" while unvisited; debug builds print the id).
  DrawTextAt(platform,
             font_cache,
             side.x + 15.0F * s,
             side.y + 26.0F * s,
             10.0F,
             kColorWhite,
             visited ? sys.name : strings.unknown);
  if (!valid_id) {
    return;
  }
  if (!NovaSystem_HasUsableTravelDestination(state, selected_id)) {
    DrawTextAt(platform,
               font_cache,
               side.x + 10.0F * s,
               side.y + 75.0F * s,
               10.0F,
               kColorWhite,
               strings.uninhabited);
    return;
  }
  // Government (0x004a6600).
  DrawTextAt(platform,
             font_cache,
             side.x + 10.0F * s,
             side.y + 75.0F * s,
             10.0F,
             kHeaderGrey,
             strings.government);
  std::string gov_name = strings.independent;
  if (sys.government_id >= 0) {
    if (const Government *g = state.scenario.Government(
            static_cast<std::int16_t>(sys.government_id + 0x80))) {
      if (!g->name.empty()) {
        gov_name = g->name;
      }
    }
  }
  DrawTextAt(platform,
             font_cache,
             side.x + 15.0F * s,
             side.y + 88.0F * s,
             10.0F,
             kColorWhite,
             gov_name);
  // Legal status (0x004a66f0).
  DrawTextAt(platform,
             font_cache,
             side.x + 10.0F * s,
             side.y + 119.0F * s,
             10.0F,
             kHeaderGrey,
             strings.legal_status);
  DrawTextAt(platform,
             font_cache,
             side.x + 15.0F * s,
             side.y + 132.0F * s,
             10.0F,
             kColorWhite,
             LegalStatusText(state, selected_id, strings));
  // Goods traded (0x004a67a0): class names for the lanes present among the
  // usable nav stellars; "<Unknown>" until discovery_state reaches 2.
  DrawTextAt(platform,
             font_cache,
             side.x + 10.0F * s,
             side.y + 155.0F * s,
             10.0F,
             kHeaderGrey,
             strings.goods_traded);
  float goods_y = side.y + 168.0F * s;
  if (sys.discovery_state < 2) {
    DrawTextAt(platform,
               font_cache,
               side.x + 15.0F * s,
               goods_y,
               10.0F,
               kColorWhite,
               strings.unknown);
  } else {
    int drawn_goods = 0;
    for (int cls = 0; cls < 6; ++cls) {
      if (!SystemTradesClass(state, sys, cls)) {
        continue;
      }
      DrawTextAt(platform,
                 font_cache,
                 side.x + 15.0F * s,
                 goods_y + static_cast<float>(drawn_goods) * 12.0F * s,
                 10.0F,
                 kColorWhite,
                 strings.goods_classes[static_cast<std::size_t>(cls)]);
      ++drawn_goods;
    }
    if (drawn_goods == 0) {
      DrawTextAt(platform,
                 font_cache,
                 side.x + 15.0F * s,
                 goods_y,
                 10.0F,
                 kColorWhite,
                 strings.none);
    }
  }
  // Services (0x004a68b0).
  DrawTextAt(platform,
             font_cache,
             side.x + 10.0F * s,
             side.y + 250.0F * s,
             10.0F,
             kHeaderGrey,
             strings.services);
  int service_count = 0;
  for (int mode = 0; mode < 3; ++mode) {
    if (SystemHasService(state, sys, mode)) {
      ++service_count;
    }
  }
  float services_y = side.y + 263.0F * s;
  if (service_count < 1) {
    if (sys.discovery_state == 1) {
      DrawTextAt(platform,
                 font_cache,
                 side.x + 15.0F * s,
                 services_y,
                 10.0F,
                 kColorWhite,
                 strings.unknown);
    } else if (sys.discovery_state >= 2) {
      DrawTextAt(platform,
                 font_cache,
                 side.x + 15.0F * s,
                 services_y,
                 10.0F,
                 kColorWhite,
                 strings.none_available);
    }
  } else if (sys.discovery_state < 2) {
    DrawTextAt(platform,
               font_cache,
               side.x + 15.0F * s,
               services_y,
               10.0F,
               kColorWhite,
               strings.unknown);
  } else {
    int drawn_services = 0;
    for (int mode = 0; mode < 3; ++mode) {
      if (!SystemHasService(state, sys, mode)) {
        continue;
      }
      DrawTextAt(platform,
                 font_cache,
                 side.x + 15.0F * s,
                 services_y + static_cast<float>(drawn_services) * 12.0F * s,
                 10.0F,
                 kColorWhite,
                 strings.service_names[static_cast<std::size_t>(mode)]);
      ++drawn_services;
    }
  }

  // ---- Bottom bar ----
  // An active inline Find shows the typed query (the original runs a modal
  // search dialog, DLOG 0xbbd; the clean-room keeps an inline stand-in).
  if (search_query != nullptr) {
    DrawTextAt(platform,
               font_cache,
               bar.x + 10.0F * s,
               bar.y + 24.0F * s,
               10.0F,
               kColorWhite,
               "Find: " + *search_query + "_");
  }
  // "Ports:" + the usable nav stellar display names, comma-separated,
  // wrapping once (0x004a6140). Until visited the value reads "<Unknown>".
  DrawTextAt(platform,
             font_cache,
             bar.x + 10.0F * s,
             bar.y + 12.0F * s,
             10.0F,
             kHeaderGrey,
             strings.ports);
  const float value_x = bar.x + 45.0F * s;
  if (!visited) {
    DrawTextAt(platform,
               font_cache,
               value_x,
               bar.y + 12.0F * s,
               10.0F,
               kColorWhite,
               strings.unknown);
  } else {
    std::string ports_line;
    float cursor_x = value_x;
    float line_y = bar.y + 12.0F * s;
    int drawn_ports = 0;
    int usable_ports = 0;
    for (const std::int16_t nav : sys.nav_defs) {
      if (nav < 0x80) {
        continue;
      }
      const Stellar *st = state.scenario.Stellar(nav);
      if (st == nullptr || !st->is_available || (st->flags & 0x20U) != 0U ||
          !NovaTargeting_StellarTargetsSpriteSetActive(*st)) {
        continue;
      }
      ++usable_ports;
      const std::string &name = st->name;
      const float name_w = static_cast<float>(font_cache.TextWidth(
          NovaFontFamily::kGeneva, 10.0F, kNovaFontStyleRegular, name));
      const float wrap_limit =
          bar.x + bar.w - 20.0F * s; // the original's right - 0x14
      if (cursor_x + name_w > wrap_limit && drawn_ports > 0) {
        cursor_x = value_x;
        line_y = bar.y + 24.0F * s;
      }
      if (drawn_ports > 0) {
        const std::string sep = ", ";
        NovaText_Draw(platform,
                      font_cache,
                      NovaFontFamily::kGeneva,
                      10.0F,
                      kNovaFontStyleRegular,
                      kColorWhite,
                      cursor_x,
                      line_y,
                      sep);
        cursor_x += static_cast<float>(font_cache.TextWidth(
            NovaFontFamily::kGeneva, 10.0F, kNovaFontStyleRegular, sep));
      }
      NovaText_Draw(platform,
                    font_cache,
                    NovaFontFamily::kGeneva,
                    10.0F,
                    kNovaFontStyleRegular,
                    kColorWhite,
                    cursor_x,
                    line_y,
                    name);
      cursor_x += name_w;
      ++drawn_ports;
    }
    if (usable_ports == 0) {
      DrawTextAt(platform,
                 font_cache,
                 value_x,
                 bar.y + 12.0F * s,
                 10.0F,
                 kColorWhite,
                 strings.none);
    }
  }
  // "Navigation Hazards:" + the composed asteroid / interference / visibility
  // description (0x004a6219 second half). "<Unknown>" until visited.
  // The game date right-aligned in the status bar (0x004a6219's date arm).
  {
    const std::string date_text = NovaText_FormatDateString(state.date, true);
    const float date_w = static_cast<float>(font_cache.TextWidth(
        NovaFontFamily::kGeneva, 10.0F, kNovaFontStyleRegular, date_text));
    NovaText_Draw(platform,
                  font_cache,
                  NovaFontFamily::kGeneva,
                  10.0F,
                  kNovaFontStyleRegular,
                  kColorWhite,
                  bar.x + bar.w - 10.0F * s - date_w,
                  bar.y + 12.0F * s,
                  date_text);
  }
  DrawTextAt(platform,
             font_cache,
             bar.x + 10.0F * s,
             bar.y + 36.0F * s,
             10.0F,
             kHeaderGrey,
             strings.nav_hazards);
  // The original places the value at +0x6e (110px), but the clean-room's
  // 10px font renders "Navigation Hazards:" wider than the original's Geneva 9,
  // so the value shifts right to clear the header (logged divergence).
  const float hazard_x = bar.x + 124.0F * s;
  if (!visited) {
    DrawTextAt(platform,
               font_cache,
               hazard_x,
               bar.y + 36.0F * s,
               10.0F,
               kColorWhite,
               strings.unknown);
    return;
  }
  std::string hazards;
  bool has_gravity_shear = false;
  for (const std::int16_t nav : sys.nav_defs) {
    if (nav < 0x80) {
      continue;
    }
    const Stellar *st = state.scenario.Stellar(nav);
    if (st != nullptr && st->is_available && (st->flags & 0x20U) == 0U &&
        NovaTargeting_StellarTargetsSpriteSetActive(*st) &&
        st->gravity_shear != 0) {
      has_gravity_shear = true;
      break;
    }
  }
  if (has_gravity_shear) {
    hazards = strings.gravity_shear;
  }
  const auto append = [&](const std::string &text) {
    if (!hazards.empty()) {
      hazards += ", ";
    }
    hazards += text;
  };
  if (sys.asteroid_count > 0) {
    const int tier =
        sys.asteroid_count < 4 ? 0 : (sys.asteroid_count < 7 ? 1 : 2);
    append(strings.asteroid[static_cast<std::size_t>(tier)] + " " +
           strings.asteroid_noun);
  }
  if (sys.interference > 0) {
    const int tier =
        sys.interference < 0x22 ? 0 : (sys.interference < 0x43 ? 1 : 2);
    append(strings.interference[static_cast<std::size_t>(tier)] + " " +
           strings.interference_noun);
  }
  if (sys.murk > 0) {
    const int tier = sys.murk < 0x1f ? 0 : (sys.murk < 0x3d ? 1 : 2);
    append(strings.visibility[static_cast<std::size_t>(tier)] + " " +
           strings.visibility_noun);
  }
  DrawTextAt(platform,
             font_cache,
             hazard_x,
             bar.y + 36.0F * s,
             10.0F,
             kColorWhite,
             hazards.empty() ? strings.none : hazards);
}

// ---- Bottom button row -----------------------------------------------------

// Loads a button label from STR# 0x96 ("button labels"), with hardcoded
// fallbacks so the row stays legible when the pool is missing.
std::string StarmapButtonLabel(StarmapButton button, bool show_borders) {
  std::uint16_t index = 0;
  switch (button) {
  case StarmapButton::kShowBorders:
    index = show_borders ? 0x39 : 0x38; // Hide Borders / Show Borders
    break;
  case StarmapButton::kClearRoute:
    index = 0x31;
    break;
  case StarmapButton::kFind:
    index = 0x3c;
    break;
  case StarmapButton::kZoomOut:
    index = 0x11; // '-'
    break;
  case StarmapButton::kZoomIn:
    index = 0x12; // '+'
    break;
  case StarmapButton::kDone:
    index = 0x5;
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

void DrawButtons(SdlPlatform &platform,
                 NovaFontCache &font_cache,
                 const ServicesButtonArt &button_art,
                 const StarmapGeometry &geometry,
                 bool show_borders,
                 double zoom,
                 std::optional<std::size_t> hovered) {
  constexpr SDL_Color kButtonLabelNormal{255, 255, 255, 255};
  constexpr SDL_Color kButtonLabelGrey{128, 128, 128, 255};
  // The zoom buttons grey out exactly on the original's enable gates
  // (DAT_007dc742 = zoom > 2.0, DAT_007dc743 = zoom < 1.1).
  const std::array<std::pair<StarmapButton, bool>, 6> buttons{{
      {StarmapButton::kShowBorders, true},
      {StarmapButton::kClearRoute, true},
      {StarmapButton::kFind, true},
      {StarmapButton::kZoomOut, zoom < kZoomOutLimit},
      {StarmapButton::kZoomIn, zoom > kZoomInLimit},
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

// Ghidra 0x004aa980 Mission_RebuildMissionTargetSystemList.
// Builds the map's mission-target system list, filling
// g_starmap_mission_target_system_ids): the travel and return systems of
// every active mission, deduplicated.
std::vector<std::int16_t> BuildMissionTargetSystems(const GameState &state) {
  std::vector<std::int16_t> out;
  for (std::size_t slot = 0; slot < state.active_missions.size(); ++slot) {
    if (!state.active_mission_runtime_flags[slot].is_active) {
      continue;
    }
    const ActiveMission &mission = state.active_missions[slot];
    for (const std::int16_t stellar_id :
         {mission.travel_stellar_id, mission.return_stellar_id}) {
      // MisnActive +0x00/+0x04 hold 0-based stellar indices (the original's
      // g_stellar_defs convention); ScenarioData::Stellar takes the 0x80-
      // based resource id.
      if (stellar_id < 0) {
        continue;
      }
      const Stellar *st =
          state.scenario.Stellar(static_cast<std::int16_t>(stellar_id + 0x80));
      if (st == nullptr || st->system_id < 0) {
        continue;
      }
      if (std::find(out.begin(), out.end(), st->system_id) == out.end()) {
        out.push_back(st->system_id);
      }
    }
  }
  return out;
}

} // namespace

// Ghidra 0x004a99f0 Ui_DrawSystemRouteMap draw pass (see starmap.hpp). The
// original re-renders NovaUi_DrawStarmapRoutesAndMarkers into a dedicated
// square surface with g_starmap_pan_origin swapped to the current system and
// g_starmap_zoom swapped to g_route_map_zoom_scale; the port projects
// directly with a temporary MapView. Background fill + border stand in for
// the unresolved PTR_DAT_00575acc / DAT_00735658 colours (TODO(decomp)).
void NovaStarmap_DrawRouteMapChart(SdlPlatform &platform,
                                   NovaFontCache &font_cache,
                                   const GameState &state,
                                   const SDL_FRect &rect,
                                   float zoom,
                                   std::int16_t selected_id,
                                   float alpha,
                                   const NovaStarmap_MarkerIcons &icons) {
  if (state.scenario.systems.empty() || rect.w <= 0.0F || rect.h <= 0.0F) {
    return;
  }
  SDL_Renderer *renderer = platform.renderer();
  const std::int16_t current_id = state.player.current_system_id;
  if (current_id < 0 ||
      static_cast<std::size_t>(current_id) >= state.scenario.systems.size()) {
    return;
  }
  const System &current =
      state.scenario.systems[static_cast<std::size_t>(current_id)];
  const MapView view{zoom,
                     static_cast<float>(current.pos_x),
                     static_cast<float>(current.pos_y)};
  StarmapGeometry geometry{};
  geometry.map = rect;

  // Fill (DrawContext_FillRect16 at 0x004a9a2a): the same per-system space
  // background colour the gameplay surface is cleared with
  // (NovaRender_SetSystemSpaceBackgroundColor, PTR_DAT_00575acc -> the
  // surface at 0x0085f6e8), so the opaque overlay blends with space while
  // hiding the world behind it. Border DAT_00735658 is a scenario-derived UI
  // colour (loader byte at def+0x8a, 0x004c686e); grey stand-in (TODO(decomp)).
  const auto *cur_def =
      state.scenario.System(static_cast<std::int16_t>(current_id + 0x80));
  const std::uint32_t bg = cur_def ? cur_def->bkgnd_color : 0;
  SDL_SetRenderDrawColor(renderer,
                         static_cast<std::uint8_t>((bg >> 16) & 0xff),
                         static_cast<std::uint8_t>((bg >> 8) & 0xff),
                         static_cast<std::uint8_t>(bg & 0xff),
                         SDL_ALPHA_OPAQUE);
  SDL_RenderFillRect(renderer, &rect);

  const auto mapped = BuildMappedSystems(state, view, rect);
  const std::vector<std::int16_t> mission_targets =
      BuildMissionTargetSystems(state);
  DrawGalaxy(platform,
             font_cache,
             state,
             mapped,
             view,
             geometry,
             selected_id,
             /*overlay=*/nullptr,
             mission_targets,
             icons,
             alpha);

  SDL_SetRenderDrawColor(renderer, 96, 96, 96, 255);
  SDL_RenderRect(renderer, &rect);
}

NovaStarmap_MarkerIcons NovaStarmap_LoadMarkerIcons(SdlPlatform &platform) {
  NovaStarmap_MarkerIcons icons;
  icons.mission_target = LoadCicnTexture(platform, 0x3a98);
  icons.selected_arrow = LoadCicnTexture(platform, 0x3a99);
  return icons;
}

// ---------------------------------------------------------------------------
// Ghidra 0x004a3aa0 NovaUi_RunStarmapWindow (clean-room modal loop).
// ---------------------------------------------------------------------------
StarmapResult NovaStarmap_RunWindow(SdlPlatform &platform,
                                    GameState &state,
                                    std::int16_t preselected_system_id,
                                    SpaceflightView *flight_view,
                                    HudRenderer *hud) {
  if (state.scenario.systems.empty()) {
    NovaLog::Warn("starmap opened with no scenario system table; closing");
    return StarmapResult{};
  }

  NovaFontCache font_cache;
  // Ghidra 0x00448090: refresh the Visibility NCB / discovery state before
  // anything reads it (the original re-evaluates per frame).
  NovaResources_EvaluateAvailability(state);
  const StarmapGeometry geometry = ResolveStarmapGeometry();
  const StarmapStrings strings = LoadStarmapStrings();
  auto backdrop = LoadStarmapBackdrop(platform);
  if (backdrop == nullptr) {
    NovaLog::Warn(
        "starmap backdrop PICT 0x213d unavailable; using placeholder");
  }
  ServicesButtonArt button_art;
  if (!button_art.Initialize(platform)) {
    NovaLog::Warn("three-state button art unavailable for the starmap buttons");
  }
  const MarkerIcons icons = LoadMarkerIcons(platform);

  // Nebula tier images (PICT 0x251c + 7*i ..), one set per populated nebula.
  std::vector<NebulaTiers> nebula_images;
  nebula_images.reserve(state.scenario.nebulae.size());
  for (std::size_t i = 0; i < state.scenario.nebulae.size(); ++i) {
    nebula_images.push_back(LoadNebulaTiers(platform, i));
  }

  // Nebula ActiveOn evaluation (the original caches it in
  // NovaResources_EvaluateAvailability; controls cannot change mid-map, so
  // evaluating at open matches).
  {
    ControlExpressionState expression;
    expression.get_control_bit = [&state](std::uint32_t bit) {
      return state.control.ControlBit(bit);
    };
    expression.is_registered = [&state](std::uint32_t) {
      return state.control.registered;
    };
    expression.is_male = [&state] { return state.control.male; };
    expression.owns_outfit = [&state](std::int16_t id) {
      return id >= 0 &&
             id < static_cast<std::int16_t>(
                      state.inventory.outfit_owned_count.size()) &&
             state.inventory.outfit_owned_count[static_cast<std::size_t>(id)] >
                 0;
    };
    expression.has_explored = [&state](std::int16_t id) {
      return id >= 0 && id < 0x800 &&
             state.control.explored_systems.test(static_cast<std::size_t>(id));
    };
    for (Nebula &neb : state.scenario.nebulae) {
      neb.active_on =
          NovaControlExpression_Evaluate(neb.active_on_expression, expression);
    }
  }

  // View state persists across map sessions (g_starmap_zoom /
  // g_starmap_pan_origin_x/y); the arrival path keeps the pan on the current
  // system, so the map opens centred there at the default zoom.
  MapView view;
  view.zoom = state.starmap_zoom;
  view.pan_x = state.starmap_pan_x;
  view.pan_y = state.starmap_pan_y;
  if (view.zoom <= 0.0F) {
    view.zoom = 1.0F; // 0x004a3cf1 open-time clamp
  }
  const SDL_FRect &panel = geometry.map;

  // Selected system (0x004a3aa0 open-init): while a plotted jump is armed
  // (travel_transfer_mode == 3 with a valid slot) the map opens preselected
  // on that jump's travel-resolvable destination; otherwise on the current
  // system.
  std::int16_t selected_id = state.player.current_system_id;
  if (state.player.travel_transfer_mode == 3 && state.travel.travel_slot >= 0 &&
      static_cast<std::size_t>(state.player.current_system_id) <
          state.scenario.systems.size()) {
    const System &cur_sys =
        state.scenario
            .systems[static_cast<std::size_t>(state.player.current_system_id)];
    if (state.travel.travel_slot <
        static_cast<std::int16_t>(cur_sys.links.size())) {
      const std::int16_t link =
          cur_sys.links[static_cast<std::size_t>(state.travel.travel_slot)];
      if (link >= 0x80) {
        const std::int16_t dest = NovaSystem_ResolveVisibleForTravel(
            state, static_cast<std::int16_t>(link - 0x80));
        if (dest >= 0) {
          selected_id = dest;
        }
      }
    }
  }
  if (preselected_system_id >= 0 &&
      static_cast<std::size_t>(preselected_system_id) <
          state.scenario.systems.size()) {
    selected_id = preselected_system_id;
  }

  const std::vector<std::int16_t> mission_targets =
      BuildMissionTargetSystems(state);

  // The mission-info window's destination-window sub-flow (route editing from
  // a selected stellar, DAT_007354a6) is not reconstructed; the flight map
  // always runs in the plain mode (logged divergence).
  bool show_borders = true; // original pref default is OFF; no prefs store yet
  bool search_active = false;
  std::string search_query;

  const auto search_select = [&](const std::string &query) {
    if (query.empty()) {
      return;
    }
    std::string lower = query;
    std::transform(
        lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
          return static_cast<char>(std::tolower(c));
        });
    for (std::size_t i = 0; i < state.scenario.systems.size(); ++i) {
      if (!SystemOnMap(state, static_cast<std::int16_t>(i))) {
        continue;
      }
      std::string name = state.scenario.systems[i].name;
      std::transform(
          name.begin(), name.end(), name.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
          });
      if (name.compare(0, lower.size(), lower) == 0) {
        selected_id = static_cast<std::int16_t>(i);
        return;
      }
    }
  };

  // Tab / Backslash have no map function in the original: the route-editing
  // trigger is Shift+click (pane-action commands 0x2a/0x36 = LShift/RShift,
  // verified against 0x004a4353's action-3 branch), so the clean-room keeps no
  // keyboard cycle.

  NovaLog::Info("opening galaxy starmap ({} systems, {} nebulae)",
                state.scenario.systems.size(),
                state.scenario.nebulae.size());

  PoliticalOverlay overlay;
  bool overlay_needs_rebuild = true;
  bool dragging = false;
  SDL_FPoint drag_last{};

  const auto close_with_selection = [&]() {
    // Ghidra 0x004a8f50 exit path: re-arm the travel slot from the plotted
    // route's first hop, then report the highlighted selection unless a route
    // re-arm took over.
    NovaStarmap_SyncTravelSelectionFromRoute(state);
    StarmapResult r;
    r.exit = StarmapExit::kContinue;
    if (!NovaStarmap_RouteHasHops(state) &&
        selected_id != state.player.current_system_id) {
      r.destination_system_id = selected_id;
    }
    return r;
  };

  const auto apply_zoom_out = [&]() {
    if (static_cast<double>(view.zoom) < kZoomOutLimit) {
      view.zoom =
          static_cast<float>(static_cast<double>(view.zoom) * kZoomOutStep);
      state.starmap_zoom = view.zoom;
      overlay_needs_rebuild = true;
    }
  };
  const auto apply_zoom_in = [&]() {
    if (static_cast<double>(view.zoom) > kZoomInLimit) {
      view.zoom =
          static_cast<float>(static_cast<double>(view.zoom) * kZoomInStep);
      state.starmap_zoom = view.zoom;
      overlay_needs_rebuild = true;
    }
  };

  while (!platform.quit_requested()) {
    const auto mapped = BuildMappedSystems(state, view, panel);
    if (flight_view != nullptr && hud != nullptr) {
      DrawLiveChrome(platform,
                     state,
                     geometry,
                     backdrop ? backdrop->get() : nullptr,
                     *flight_view,
                     *hud);
    } else {
      DrawChrome(platform, geometry, backdrop ? backdrop->get() : nullptr);
    }
    DrawNebulae(platform, state, nebula_images, view, panel);
    if (show_borders) {
      if (overlay_needs_rebuild) {
        overlay = BuildPoliticalOverlay(state, view, panel);
        overlay_needs_rebuild = false;
      }
      DrawGalaxy(platform,
                 font_cache,
                 state,
                 mapped,
                 view,
                 geometry,
                 selected_id,
                 &overlay,
                 mission_targets,
                 icons);
    } else {
      DrawGalaxy(platform,
                 font_cache,
                 state,
                 mapped,
                 view,
                 geometry,
                 selected_id,
                 nullptr,
                 mission_targets,
                 icons);
    }
    DrawSidePanels(platform,
                   font_cache,
                   state,
                   geometry,
                   strings,
                   selected_id,
                   search_active ? &search_query : nullptr);

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
                static_cast<double>(view.zoom),
                hovered_button);
    platform.Present();

    // One batch of raw editable keys / clicks.
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
          search_active = false;
        } else {
          return close_with_selection();
        }
        break;

      case TextKey::character: {
        const char ch = in->character;
        if (search_active) {
          search_query.push_back(ch);
          search_select(search_query);
          break;
        }
        if (ch == 'q' || ch == 'x') {
          return close_with_selection();
        }
        if (ch == '+' || ch == '=') {
          apply_zoom_in();
        } else if (ch == '-') {
          apply_zoom_out();
        }
        break;
      }

      case TextKey::primary: {
        const SDL_FPoint mp = platform.mouse_position();
        bool button_clicked = false;
        for (std::size_t i = 0; i < geometry.buttons.size(); ++i) {
          const SDL_FRect &b = geometry.buttons[i];
          if (mp.x >= b.x && mp.x < b.x + b.w && mp.y >= b.y &&
              mp.y < b.y + b.h) {
            button_clicked = true;
            switch (static_cast<StarmapButton>(i)) {
            case StarmapButton::kShowBorders:
              // Ghidra 0x004A9D10 NovaUi_EnableStarmapPoliticalOverlay.
              show_borders = !show_borders;
              overlay_needs_rebuild = true;
              break;
            case StarmapButton::kClearRoute:
              NovaStarmap_ClearRoute(state);
              selected_id = state.player.current_system_id;
              break;
            case StarmapButton::kFind:
              search_active = true;
              search_query.clear();
              break;
            case StarmapButton::kZoomOut:
              apply_zoom_out();
              break;
            case StarmapButton::kZoomIn:
              apply_zoom_in();
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
        if (mp.x < panel.x || mp.y < panel.y || mp.x >= panel.x + panel.w ||
            mp.y >= panel.y + panel.h) {
          break;
        }
        // Map-panel click (0x004a3cdd action 3): the first VISIBLE system in
        // id order whose marker rect (inset -10) contains the point AND that
        // passes the acceptance gate wins; the hit is then resolved through
        // the visibility chain (System_ResolveVisibleSystemForTravel
        // 0x0046b920) so a twin group always selects its currently visible
        // member. The gate (0x004a4773): a click is accepted on a system that
        // is latched by the last discovery rebuild, is a mission target (raw
        // id compare, 0x004a4659), or is already selected -- otherwise only
        // when it is a travel-resolvable adjacency of the current system
        // (plain click) or of the plotted route's tail (shift-click, which
        // additionally accepts positional twins of the tail, 0x004a4899;
        // with no hops plotted the shift source is the current system and
        // the twin check is skipped). Everything else reads as empty space
        // and begins a drag-pan.
        const bool shift =
            (SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_LSHIFT] != 0) ||
            (SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_RSHIFT] != 0);
        std::int16_t hit = -1;
        std::int16_t shift_tail = -1;
        if (shift) {
          for (const std::int16_t hop : state.travel.starmap_route) {
            if (hop != -1) {
              shift_tail = hop;
            }
          }
          if (shift_tail == state.player.current_system_id) {
            // Route[0] anchors the current system, not a plotted hop.
            shift_tail = -1;
          }
        }
        for (const MappedSystem &m : mapped) {
          const std::int16_t cid = m.zero_based_id;
          const System &cand =
              state.scenario.systems[static_cast<std::size_t>(cid)];
          if (!cand.is_visible) {
            continue;
          }
          if (std::abs(mp.x - m.sx) > kClickInset ||
              std::abs(mp.y - m.sy) > kClickInset) {
            continue;
          }
          bool accept =
              cand.discovered_this_rebuild || cid == selected_id ||
              std::any_of(mission_targets.begin(),
                          mission_targets.end(),
                          [&](std::int16_t target) { return target == cid; });
          if (!accept) {
            const std::int16_t adj_source =
                shift && shift_tail >= 0 ? shift_tail
                                         : state.player.current_system_id;
            const System &src =
                state.scenario.systems[static_cast<std::size_t>(adj_source)];
            if (shift && shift_tail >= 0 && src.pos_x == cand.pos_x &&
                src.pos_y == cand.pos_y) {
              accept = true;
            } else {
              for (const std::int16_t link : src.links) {
                if (link < 0x80) {
                  continue;
                }
                const std::int16_t resolved =
                    NovaSystem_ResolveVisibleForTravel(
                        state, static_cast<std::int16_t>(link - 0x80));
                if (resolved < 0 || static_cast<std::size_t>(resolved) >=
                                        state.scenario.systems.size()) {
                  continue;
                }
                if (resolved == cid) {
                  accept = true;
                  break;
                }
                const System &rsys =
                    state.scenario.systems[static_cast<std::size_t>(resolved)];
                if (rsys.pos_x == cand.pos_x && rsys.pos_y == cand.pos_y) {
                  accept = true;
                  break;
                }
              }
            }
          }
          if (accept) {
            hit = cid;
            break;
          }
        }
        if (hit >= 0) {
          hit = NovaSystem_ResolveVisibleForTravel(state, hit);
        }
        if (hit < 0) {
          // Empty space: begin a drag-pan (0x004a3f60 drag loop).
          dragging = true;
          drag_last = mp;
          break;
        }
        if (hit < 0) {
          if (!shift) {
            // Plain click on empty space DESELECTS: the original assigns
            // g_starmap_selected_system_id straight from the hit-test result
            // (0x004a4433 arm), which is -1 on a miss — the selection
            // reticle and the green selected arrow (CICN 0x3a99) both drop.
            selected_id = -1;
          }
          break;
        }
        search_active = false;
        if (shift) {
          // Shift-click: route editing (0x004a47cc) — reset at the current
          // system's slot, truncate at a plotted hop (the route ends AT the
          // clicked system), pop a twin of the tail, append when the hit
          // extends the chain. The original moves the selection only when
          // the click appends a hop.
          if (NovaStarmap_EditRouteAtHop(state, hit)) {
            selected_id = hit;
          }
        } else {
          selected_id = hit;
          if (hit == state.player.current_system_id) {
            // Re-selecting the current system disarms the plotted jump
            // (0x004a4e70 slot clear via the adjacency-scan miss).
            state.travel.travel_slot = -1;
            state.travel.starmap_destination_system_id = -1;
          } else if (!NovaTravel_PlotStarmapDestination(state, hit)) {
            // A plain click arms the travel slot when the system is directly
            // linked and clears it otherwise (0x004a5840).
            state.travel.travel_slot = -1;
          }
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
      case TextKey::physical:
        break;
      }
    }

    // Drag-pan: while the primary button is held after an empty-space press,
    // the pan origin follows the cursor delta scaled by zoom (0x004a3f60:
    // pan -= delta * zoom), then the overlay rebuilds.
    if (dragging) {
      if ((SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) != 0U) {
        const SDL_FPoint mp = platform.mouse_position();
        view.pan_x -= (mp.x - drag_last.x) * view.zoom;
        view.pan_y -= (mp.y - drag_last.y) * view.zoom;
        state.starmap_pan_x = view.pan_x;
        state.starmap_pan_y = view.pan_y;
        drag_last = mp;
        overlay_needs_rebuild = true;
      } else {
        dragging = false;
      }
    }

    // Keyboard pan (clean-room convenience kept alongside the original's
    // mouse-drag pan): arrows move the camera; content shifts opposite.
    const bool *const keys = SDL_GetKeyboardState(nullptr);
    constexpr float kPanPxPerFrame = 10.0F;
    bool panned = false;
    if (keys[SDL_SCANCODE_LEFT]) {
      view.pan_x -= kPanPxPerFrame * view.zoom;
      panned = true;
    }
    if (keys[SDL_SCANCODE_RIGHT]) {
      view.pan_x += kPanPxPerFrame * view.zoom;
      panned = true;
    }
    if (keys[SDL_SCANCODE_UP]) {
      view.pan_y -= kPanPxPerFrame * view.zoom;
      panned = true;
    }
    if (keys[SDL_SCANCODE_DOWN]) {
      view.pan_y += kPanPxPerFrame * view.zoom;
      panned = true;
    }
    if (panned) {
      state.starmap_pan_x = view.pan_x;
      state.starmap_pan_y = view.pan_y;
      overlay_needs_rebuild = true;
    }
  }

  StarmapResult quit;
  quit.exit = StarmapExit::kQuit;
  quit.destination_system_id = selected_id;
  return quit;
}

} // namespace game
