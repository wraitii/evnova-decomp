#include "starmap.hpp"
#include "starmap_internal.hpp"

#include "../brgr_archive.hpp"
#include "../cicn_image.hpp"
#include "../log.hpp"
#include "../pict_image.hpp"
#include "../util/format.hpp"
#include "hud_overlay.hpp"
#include "hud_renderer.hpp"
#include "mission.hpp"
#include "nova_font.hpp"
#include "outfit.hpp"
#include "scenario_data.hpp"
#include "services_buttons.hpp"
#include "spaceflight_view.hpp"
#include "targeting.hpp"
#include "travel.hpp"
#include "ui_dialog.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

namespace game {
namespace {

using starmap_detail::BuildMappedSystems;
using starmap_detail::BuildPoliticalOverlay;
using starmap_detail::DrawGalaxy;
using starmap_detail::MappedSystem;
using starmap_detail::MapView;
using starmap_detail::PoliticalOverlay;
using starmap_detail::StarmapGeometry;

// ---- Window geometry (native DLOG authoring space) -------------------------
// The starmap window is the EV Nova DLOG 0x7d0 dialog: a 601x513 frame whose
// backdrop is the PICT 0x213d "Map" starfield, blitted across the whole window
// (DAT_007dc73c in NovaUi_RunStarmapWindow 0x004a3aa0). Only the galaxy-graph
// viewport (DITL item 2 / UiPanel_GetEntryInfo entry 3) is filled opaque with
// the system-space background colour; the side column (item 5 / entry 6) and
// the bottom status bar (item 1 / entry 2) render their text straight onto the
// starfield, separated from the graph by thin rules. The platform contains
// this native composition as a whole when the output window is smaller.
constexpr int kStarmapWindowW = 601;
constexpr int kStarmapWindowH = 513;
constexpr float kStarmapWindowX = 0.0F;
constexpr float kStarmapWindowY = 0.0F;
constexpr float kMapPanelX = 9.0F;
constexpr float kMapPanelY = 8.0F;
constexpr float kMapPanelW = 458.0F;
constexpr float kMapPanelH = 420.0F;
constexpr float kSidePanelX = 474.0F;
constexpr float kSidePanelY = 8.0F;
constexpr float kSidePanelW = 120.0F;
constexpr float kSidePanelH = 429.0F;
constexpr float kBottomBarX = 8.0F;
constexpr float kBottomBarY = 436.0F;
constexpr float kBottomBarW = 586.0F;
constexpr float kBottomBarH = 42.0F;

// The bottom button row (DITL items 8,7,9,3,4,0 in left-to-right screen
// order; 1-based entries 9,8,10,4,5,1). Each is the item rect in the DITL
// authoring space; the rects are re-derived from the resource at runtime.
struct StarmapButtonRect {
  float x, y, w, h;
};

constexpr std::array<StarmapButtonRect, 6> kStarmapButtonRects{{
    {11.0F, 483.0F, 130.0F, 25.0F},
    {155.0F, 483.0F, 120.0F, 25.0F},
    {288.0F, 483.0F, 99.0F, 25.0F},
    {408.0F, 483.0F, 25.0F, 25.0F},
    {438.0F, 483.0F, 25.0F, 25.0F},
    {483.0F, 483.0F, 99.0F, 25.0F},
}};

// ---- Palette (Ghidra Settings_InitColors 0x004ad7c0 fills the fixed
// RGBColor triples at DAT_00733b2c..DAT_00733b78; 16-bit components render as
// >>8 in the 8-bit draw paths). PTR_DAT_00575acc targets the black system-
// space colour (map background + marker disc fill); PTR_DAT_00575ad8 and
// PTR_DAT_00575af0 are the static white and cyan triples at 0x575ad0/0x575ae8.
constexpr SDL_Color kColorBlack{0, 0, 0, 255};
constexpr SDL_Color kColorWhite{255, 255, 255, 255};
// 4000 grey (DAT_00733b68) -- the link tint in the destination-window route
// mode, which is not reconstructed; kept for reference.
[[maybe_unused]] constexpr SDL_Color kLinkGreyDim{15, 15, 15, 255};
constexpr SDL_Color kHeaderGrey{192, 192, 192, 255}; // 0xc000 (0x733b50)

// Zoom model (g_starmap_zoom 0x005759d8, factors at 0x005759f0/f8, enable
// limits at 0x00575a00/08). Zoom is a DIVISOR: screen = world / zoom, so the
// zoom-out key multiplies and zoom-in divides. Both factors multiply the
// stored value (0x004a3fae / 0x004a4035): zoom-out x4/3 while zoom < 2.0
// (0.5625 -> 0.75 -> 1.0 -> 1.333 -> 1.778 -> 2.370), zoom-in x0.75 while
// zoom > 0.5 (one step to 0.4219). Opening clamps a non-positive stored zoom
// back to 1.0 (0x004a3cf1).
constexpr double kZoomOutStep = 4.0 / 3.0; // '-' / zoom-out button
constexpr double kZoomInStep = 0.75;       // '+' / zoom-in button
constexpr double kZoomOutLimit = 2.0;      // zoom-out allowed while below
constexpr double kZoomInLimit = 0.5;       // zoom-in allowed while above
constexpr float kClickInset = 10.0F; // click hit-test rect (Rect_Inset -10)

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
    const auto entry = static_cast<std::uint16_t>(i + 1);
    s.goods_classes[i] =
        NovaResources_LoadPatchedStringEntry(0xfa0, entry, 9000)
            .value_or(std::string(kGoodsFallback[i]));
  }
  return s;
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
                       static_cast<float>(kStarmapWindowW),
                       static_cast<float>(kStarmapWindowH)};
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
    return SDL_FRect{kStarmapWindowX + static_cast<float>(it.left),
                     kStarmapWindowY + static_cast<float>(it.top),
                     static_cast<float>(it.right - it.left),
                     static_cast<float>(it.bottom - it.top)};
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

void DrawRule(SDL_Renderer *renderer, float x0, float x1, float y) {
  SDL_SetRenderDrawColor(
      renderer, kColorBlack.r, kColorBlack.g, kColorBlack.b, SDL_ALPHA_OPAQUE);
  SDL_RenderLine(renderer, x0, y, x1, y);
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
  platform.SetPlacement(
      PlaceContained({601.0F, 513.0F}, platform.logical_playfield_size()));

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
  platform.SetPlacement(
      PlaceContained({601.0F, 513.0F}, platform.logical_playfield_size()));
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

// ---- Info panes ------------------------------------------------------------

// @port 0x00468d90 100%
// Ghidra 0x00468d90 NovaUi_DrawSystemFactionConflictStatus: the selected
// system's Legal Status text, graded from the system's reputation against the
// owning government's flee threshold (t). The ladder (levels 1..15) is the
// binary's exact threshold chain; a hazard-marked destination among the first
// three nav stellars overrides to 0x10/0x11 (the STR# 0x86 pool's trailing
// "Military Dictator"/"Military Governor" entries -- odd but verbatim), and
// governments with flags_primary bit 0 suppress the status to "N/A".
[[nodiscard]] int LegalStatusLevel(const GameState &state,
                                   std::int16_t zero_based_id) {
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
    if (st->dominated) {
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
  return level;
}

// @port 0x00469d30 100%
// Ghidra 0x00469d30 Stellar_ExtractTravelFlagConnectiveValue: the six
// trade-class lanes packed into the stellar flags word, class i at bits
// (28 - 4*i) .. (30 - 4*i); any nonzero lane means the class is traded.
[[nodiscard]] bool StellarTradesClass(std::uint32_t flags, int class_index) {
  const std::uint32_t lane = 0xE0000000UL >> (4 * class_index);
  return (flags & lane) != 0U;
}

// @port 0x00468210 70% gameplay
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

// @port 0x004682d0 70% gameplay
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
                    std::int16_t selected_id) {
  const float s = 1.0F;
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
             NovaUi_SystemFactionConflictStatusText(state, selected_id));
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
    const std::string date_text = NovaText_FormatDateString(
        state.date, true, state.date_prefix, state.date_suffix);
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
  // The original composes the hazard summary, falls back to STR# 0x7d2 0x14f
  // ("none"), then runs the first byte through the C-locale toupper
  // MWRuntime_ToUpper (0x004a70b3), so the row reads "None". The goods/ports
  // rows call Resource_DrawStringEntry directly and stay lower-case.
  DrawTextAt(
      platform,
      font_cache,
      hazard_x,
      bar.y + 36.0F * s,
      10.0F,
      kColorWhite,
      evnova::util::UpperFirstAscii(hazards.empty() ? strings.none : hazards));
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
                 bool route_has_hops,
                 double zoom,
                 std::optional<std::size_t> hovered) {
  constexpr SDL_Color kButtonLabelNormal{255, 255, 255, 255};
  constexpr SDL_Color kButtonLabelGrey{128, 128, 128, 255};
  // The zoom buttons grey out exactly on the original's enable gates
  // (DAT_007dc742 = zoom > 2.0, DAT_007dc743 = zoom < 1.1); Clear Route greys
  // out unless a route is plotted (DAT_007dc744, NovaUi_DrawStarmapButtons
  // 0x0049f1f0).
  const std::array<std::pair<StarmapButton, bool>, 6> buttons{{
      {StarmapButton::kShowBorders, true},
      {StarmapButton::kClearRoute, route_has_hops},
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

// Ghidra 0x004aab30 NovaUi_RunStarmapSearchDialog: the modal Find box (DLOG
// 0xbbd, row 5 = edit text, activation 1 = Find, 3 = Cancel).
// `render_background` is redrawn behind the box each frame. Returns the chosen
// system, or nullopt on cancel / no match. The caller applies the selection and
// re-centres the map, mirroring the original's pan update.
std::optional<std::int16_t>
RunStarmapSearchDialog(SdlPlatform &platform,
                       NovaFontCache &font_cache,
                       const GameState &state,
                       const std::function<void()> &render_background) {
  auto window = UiWindow_CreateFromDialogResource(platform, 0xbbd);
  if (!window) {
    NovaLog::Todo("starmap search DLOG 0xbbd unavailable; Find is a no-op");
    return std::nullopt;
  }
  UiPanel_SetEntryTextPascal(*window, 5, "");
  ProbeUiAutoClear probe_ui_guard(platform);
  short code = -1;
  bool accepted = false;
  while (!accepted && !platform.quit_requested()) {
    UiWindow_RunInteractionLoop(
        platform, font_cache, *window, &code, render_background);
    if (code == 1) {
      accepted = true;
    } else if (code == 3) {
      return std::nullopt;
    }
    code = -1;
  }
  if (!accepted || platform.quit_requested()) {
    return std::nullopt;
  }
  const std::int16_t match = starmap_detail::FindBestSystemMatch(
      state, UiPanel_GetEntryTextPascal(*window, 5));
  if (match < 0) {
    // 0x004aab30 failure path: no candidate, or a sub-2-character match shared
    // by more than one system (the original plays the 0x160 UI sound).
    return std::nullopt;
  }
  return match;
}

} // namespace

// Ghidra 0x004aa980 Mission_RebuildMissionTargetSystemList. Per active mission
// emits ONE arrow system: the TravelStel system, replaced by the ReturnStel
// system once travel_stellar_reached is set and the two differ (an unavailable
// ReturnStel leaves TravelStel in place). Bible Flags 0x0002 suppresses the
// arrow; 0x0200 adds the resolved ShipSyst (current_system_id +0x10).
std::vector<std::int16_t> BuildMissionTargetSystems(const GameState &state) {
  std::vector<std::int16_t> out;
  const auto append_unique = [&out](std::int16_t system_id) {
    if (system_id >= 0 &&
        std::find(out.begin(), out.end(), system_id) == out.end()) {
      out.push_back(system_id);
    }
  };
  for (std::size_t slot = 0; slot < state.active_missions.size(); ++slot) {
    if (!state.active_mission_runtime_flags[slot].is_active) {
      continue;
    }
    const ActiveMission &mission = state.active_missions[slot];
    // MisnActive +0x00/+0x04 hold 0-based stellar indices (the original's
    // g_stellar_defs convention); ScenarioData::Stellar takes the 0x80-based
    // resource id.
    std::int16_t target_system = -1;
    if (mission.travel_stellar_id >= 0) {
      const Stellar *travel = state.scenario.Stellar(
          static_cast<std::int16_t>(mission.travel_stellar_id + 0x80));
      if (travel != nullptr && travel->is_available && travel->system_id >= 0) {
        target_system = travel->system_id;
      }
    }
    if (mission.return_stellar_id >= 0 &&
        mission.return_stellar_id != mission.travel_stellar_id &&
        state.active_mission_runtime_flags[slot].travel_stellar_reached) {
      const Stellar *ret = state.scenario.Stellar(
          static_cast<std::int16_t>(mission.return_stellar_id + 0x80));
      if (ret != nullptr && ret->is_available && ret->system_id >= 0) {
        target_system = ret->system_id;
      }
    }
    // Bible Flags 0x0002: "Don't show the red destination arrows on the map".
    if (target_system >= 0 && (mission.flags_primary & 0x0002U) == 0U) {
      append_unique(target_system);
    }
    // Bible Flags 0x0200: "Show an additional arrow on the map for the
    // ShipSyst". MisnActive +0x10 holds the resolved mission-ship system.
    if ((mission.flags_primary & 0x0200U) != 0U &&
        (mission.flags_primary & 0x0002U) == 0U &&
        mission.target_ship_count > 0 && mission.current_system_id >= 0) {
      append_unique(
          Misn_ResolveVisibleSystemForTravel(state, mission.current_system_id));
    }
  }
  return out;
}

std::string
NovaUi_SystemFactionConflictStatusText(const GameState &state,
                                       std::int16_t zero_based_system_id) {
  if (zero_based_system_id < 0 ||
      static_cast<std::size_t>(zero_based_system_id) >=
          state.scenario.systems.size()) {
    return NovaHud_LoadStringEntry(0x7d2, 0x18c).value_or("N/A");
  }
  const int level = LegalStatusLevel(state, zero_based_system_id);
  if (level == 0) {
    return NovaHud_LoadStringEntry(0x7d2, 0x18c).value_or("N/A");
  }
  return NovaHud_LoadStringEntry(0x86, static_cast<std::uint16_t>(level + 1))
      .value_or("N/A");
}

// Ghidra 0x004a99f0 Ui_DrawSystemRouteMap draw pass (see starmap.hpp). The
// original re-renders NovaUi_DrawStarmapRoutesAndMarkers into a dedicated
// square surface with g_starmap_pan_origin swapped to the current system and
// g_starmap_zoom swapped to g_route_map_zoom_scale; the port projects
// directly with a temporary MapView. The surface is filled with the per-system
// space background colour (PTR_DAT_00575acc) and framed with the c\x9alr
// Colors floating_map colour (DAT_00735658, supplied by the caller). The
// 16/32-bit path draws the frame BEFORE the markers (only the 8-bit path
// frames afterwards, 0x004a9a63 vs 0x004a9ae5); the port matches the former.
void NovaStarmap_DrawRouteMapChart(SdlPlatform &platform,
                                   NovaFontCache &font_cache,
                                   const GameState &state,
                                   const SDL_FRect &rect,
                                   float zoom,
                                   std::int16_t selected_id,
                                   float alpha,
                                   const NovaStarmap_MarkerIcons &icons,
                                   SDL_Color border_color) {
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
  // hiding the world behind it.
  const auto *cur_def =
      state.scenario.System(static_cast<std::int16_t>(current_id + 0x80));
  const std::uint32_t bg = cur_def ? cur_def->bkgnd_color : 0;
  SDL_SetRenderDrawColor(renderer,
                         static_cast<std::uint8_t>((bg >> 16) & 0xff),
                         static_cast<std::uint8_t>((bg >> 8) & 0xff),
                         static_cast<std::uint8_t>(bg & 0xff),
                         SDL_ALPHA_OPAQUE);
  SDL_RenderFillRect(renderer, &rect);

  // Frame first (DrawContext_FrameRect16WithCurrentColor at 0x004a9a63, the
  // non-8-bit path): in the shipped Colors record floating_map is (12,12,12),
  // so this reads as a near-invisible outline on the black space backdrop.
  SDL_SetRenderDrawColor(renderer,
                         border_color.r,
                         border_color.g,
                         border_color.b,
                         SDL_ALPHA_OPAQUE);
  SDL_RenderRect(renderer, &rect);

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
  const SdlPlatform::ScopedPlacement restore_placement(
      platform, platform.current_placement());
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
      return Outfit_PlayerHasOutfitForControlExpression(state, id);
    };
    expression.has_explored = [&state](std::int16_t id) {
      return NovaSystem_HasExploredToken(state, id);
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
  // Runtime copy of NovaPreferences::starmap_show_borders (Ghidra
  // g_starmap_show_borders, .prf +0x76); seeded from prefs at startup and
  // synced back at the .prf save points. The port defaults it ON because its
  // overlay is cheap (the original defaulted OFF).
  bool &show_borders = state.starmap_show_borders;

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

  // Draws one full starmap frame. Shared with the Find modal's background
  // redraw so the map stays visible behind the search box. `mapped` is owned
  // by the loop because the click hit-test below also reads it.
  std::vector<MappedSystem> mapped;
  const auto draw_starmap = [&](std::optional<std::size_t> hovered) {
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
    DrawSidePanels(platform, font_cache, state, geometry, strings, selected_id);
    DrawButtons(platform,
                font_cache,
                button_art,
                geometry,
                show_borders,
                NovaStarmap_RouteHasHops(state),
                static_cast<double>(view.zoom),
                hovered);
  };

  while (!platform.quit_requested()) {
    // Ghidra 0x0049eef0 UiPanel_TrackSixEntryMouseSelection (hover half):
    // the six-button row highlights the entry under the cursor, gated by the
    // same enable flags the draw pass uses. The press/release tracking half is
    // folded into the click dispatch below.
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
    mapped = BuildMappedSystems(state, view, panel);
    draw_starmap(hovered_button);
    platform.Present();

    // One batch of raw editable keys / clicks.
    for (std::optional<TextInput> in; (in = platform.PollTextEvent());) {
      switch (in->key) {
      case TextKey::escape:
        return close_with_selection();

      case TextKey::enter:
        return close_with_selection();

      case TextKey::character: {
        const char ch = in->character;
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
              // Ghidra 0x00872550 NovaUi_StarmapEnableBordersAction ->
              // 0x004A9D10 NovaUi_EnableStarmapPoliticalOverlay.
              show_borders = !show_borders;
              overlay_needs_rebuild = true;
              break;
            case StarmapButton::kClearRoute:
              // Disabled (and unclickable) unless a route is plotted, matching
              // NovaUi_DrawStarmapButtons / UiPanel_TrackSixEntryMouseSelection
              // (DAT_007dc744).
              if (NovaStarmap_RouteHasHops(state)) {
                NovaStarmap_ClearRoute(state);
              }
              break;
            case StarmapButton::kFind: {
              const auto match =
                  RunStarmapSearchDialog(platform, font_cache, state, [&] {
                    draw_starmap(std::nullopt);
                  });
              if (match) {
                selected_id = *match;
                const System &found =
                    state.scenario.systems[static_cast<std::size_t>(*match)];
                view.pan_x = static_cast<float>(found.pos_x);
                view.pan_y = static_cast<float>(found.pos_y);
                state.starmap_pan_x = view.pan_x;
                state.starmap_pan_y = view.pan_y;
                overlay_needs_rebuild = true;
              }
              break;
            }
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
    platform.PaceFrame();
  }

  StarmapResult quit;
  quit.exit = StarmapExit::kQuit;
  quit.destination_system_id = selected_id;
  return quit;
}

} // namespace game
