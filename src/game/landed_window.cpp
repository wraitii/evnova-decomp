#include "landed_window.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../pict_image.hpp"
#include "../sdl_platform.hpp"
#include "nova_font.hpp"
#include "outfit.hpp"
#include "scenario_data.hpp"
#include "services_buttons.hpp"
#include "targeting.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <string>

namespace game {

// ---------------------------------------------------------------------------
// Stellar_LandOnSpob (0x00456480): landing transition subset.
// ---------------------------------------------------------------------------
bool NovaLanding_EnterDocked(GameState &state, LandedContext &ctx) {
  ctx.landed = false;
  const std::int16_t sid = state.travel.selected_stellar_id;
  if (!NovaTargeting_IsLandingAvailable(state)) {
    return false;
  }
  const auto *st = state.scenario.Stellar(sid);
  if (!st) {
    return false;
  }

  // Reposition to the stellar, zero velocity/speed, refill shields/armor from
  // the effective maximums (mirrors Stellar_LandOnSpob).
  state.player.pos_x = static_cast<float>(st->pos_x);
  state.player.pos_y = static_cast<float>(st->pos_y);
  state.player.vel_x = 0.0F;
  state.player.vel_y = 0.0F;
  state.player.speed = 0.0F;
  const auto eff = Outfit_ComputePlayerEffectiveStats(state);
  state.player.shield_points = eff.max_shield_points;
  state.player.armor_points = eff.max_armor_points;
  state.cached_stats = eff;
  state.stat_cache_valid = true;

  // Service cost (StellarDef service_cost): deducted once per landing, waived
  // for hazard stellars, clamped >= 0 credits.
  if (st->service_cost > 0 && !st->hazard_marker) {
    state.player.credits -= st->service_cost;
    state.player.credits = std::max(0, state.player.credits);
  }

  ctx.stellar_id = sid;
  ctx.landed = true;
  ctx.selection = LandedService::kLaunch;
  state.travel.landed_this_frame = true;
  NovaLog::Info("landed at stellar {} ({}); shields/armor refilled, {} credits "
                "remaining after service cost",
                sid,
                st->name,
                state.player.credits);
  return true;
}

// ---------------------------------------------------------------------------
// Fuel service.
// ---------------------------------------------------------------------------
// Refuels the player ship toward its effective fuel capacity. `price_per_unit`
// is the price in credits for one fuel point (the raw fuel_points scale where
// a typical ship holds a few hundred units); the player pays per unit topped
// up, clamped so credits never go negative (they may only buy as much as they
// can afford). Returns the credits actually spent.
std::int32_t NovaLanded_Refuel(GameState &state, std::int32_t price_per_unit) {
  const PlayerEffectiveStats eff = Outfit_ComputePlayerEffectiveStats(state);
  state.cached_stats = eff;
  state.stat_cache_valid = true;

  const float missing =
      std::max(0.0F, eff.fuel_capacity - state.player.fuel_points);
  if (missing <= 0.0F || price_per_unit <= 0) {
    return 0;
  }
  // Full cost for the whole top-up; the player may only buy as far as credits
  // reach, so cap the spend at the wallet.
  const std::int64_t full_cost =
      std::int64_t{price_per_unit} * static_cast<std::int64_t>(missing);
  const std::int64_t spend =
      std::min<std::int64_t>(state.player.credits, full_cost);
  state.player.credits -= static_cast<std::int32_t>(spend);
  const float gained =
      static_cast<float>(spend) / static_cast<float>(price_per_unit);
  state.player.fuel_points =
      std::min(eff.fuel_capacity, state.player.fuel_points + gained);
  NovaLog::Info("refuel: bought {:.1f} fuel for {} credits ({} remaining)",
                gained,
                spend,
                state.player.credits);
  return static_cast<std::int32_t>(spend);
}

// ---------------------------------------------------------------------------
// Armor/shield repair service.
// ---------------------------------------------------------------------------
std::int32_t NovaLanded_Repair(GameState &state,
                               std::int32_t price_per_armor_point) {
  const PlayerEffectiveStats eff = Outfit_ComputePlayerEffectiveStats(state);
  state.cached_stats = eff;
  state.stat_cache_valid = true;

  // The original repairs the whole hull; shields are already full from the
  // landing transition, so only the armor gap is billed.
  const float armor_gap =
      std::max(0.0F, eff.max_armor_points - state.player.armor_points);
  if (armor_gap <= 0.0F || price_per_armor_point <= 0) {
    return 0;
  }
  const std::int64_t full_cost = std::int64_t{price_per_armor_point} *
                                 static_cast<std::int64_t>(armor_gap);
  const std::int32_t spend = static_cast<std::int32_t>(
      std::min<std::int64_t>(state.player.credits, full_cost));
  state.player.credits -= spend;
  const float repaired =
      static_cast<float>(spend) / static_cast<float>(price_per_armor_point);
  state.player.armor_points =
      std::min(eff.max_armor_points, state.player.armor_points + repaired);
  NovaLog::Info("repair: fixed {:.1f} armor for {} credits ({} remaining)",
                repaired,
                spend,
                state.player.credits);
  return spend;
}

// ---------------------------------------------------------------------------
// Window / service-button geometry.
//
// The original docked screen is the Spaceport window (DLOG 0x3e8, DITL 0x3e8),
// a near-full-screen 640x480 panel whose backdrop is destination-art PICT
// 0x2134 (Ghidra FUN_0048e970 sets g_travel_overlay_sprite_handle =
// Resource_LoadPictAsImage(0x2134), drawn across the whole window rect). The
// earlier 263x185 PICT 0x2137 is a *sub*-window decoration (the travel-services
// modal DLOG 0x3f5), not the docked backdrop, so presenting it as the full
// screenspace backdrop was wrong. Service controls live in two ~145px-wide
// columns down the left and right edges (DITL 0x3e8 entries 3,6,7,8,9,10,11,
// 12), mirroring the real docked buttons.
//
// We reproduce that docked panel here: the backdrop fills the full 640x480
// screen and the window content (panels, title band, and the two-column x
// 4-row service grid at the lower-left and lower-right) is laid out from the
// real Spaceport DLOG/DITL 0x3e8 via NovaDialogWindow_Layout, so the draw pass
// and the mouse hit-test share the same data-driven rects. The hardcoded
// two-column grid remains only as a fallback when the dialog archive is
// absent.

namespace {

// The docked panel covers the whole 640x480 logical screen.
using PanelRect = SDL_FRect;
[[nodiscard]] PanelRect FullScreenPanel() {
  return {0.0F, 0.0F, 640.0F, 480.0F};
}

// Fallback button columns (DITL-left-column values) used when the dialog
// archive is absent, so the dock still renders.
struct ServiceColumns {
  float left_x = 3.0F;
  float right_x = 471.0F;
  float width = 145.0F;
  float height = 25.0F;
  float row_pitch = 41.0F;
  float row0_y = 333.0F;
};

} // namespace

// Public adapter (#3): lays the Spaceport dialog (DLOG+ DITL 0x3e8) onto the
// 640x480 logical panel, centering the dialog window on it and mapping every
// DITL item to panel space (screen = window_origin + dialog_rect). This
// replaces the previously hardcoded panel/title/status/button geometry with
// geometry read straight from Nova.rez. Type definitions live in the header.
// When the dialog resources are unavailable it falls back to a minimal
// layout with just the buttons from the hardcoded two-column grid.
bool NovaDialogWindow_Layout(const SDL_FRect &panel, DockedLayout &out) {
  constexpr ServiceColumns kFallback;
  constexpr std::size_t kRows = 4;
  const std::size_t button_count =
      std::min<std::size_t>(static_cast<std::size_t>(LandedService::kCount),
                            kRows * 2U);

  const auto def = NovaResource_LoadDialogDefinition(0x3e8);
  const auto items = NovaResource_LoadDialogItems(0x3e8);
  if (def && items) {
    // Window size = DLOG bounds right-bottom minus left-top (618x517 for the
    // Spaceport window); centered like Dialog_CreateFromDlog centers it, with
    // the centered offset truncated toward zero to match the game's integer
    // (display - windowSize)/2 arithmetic.
    const float win_w = static_cast<float>(def->right - def->left);
    const float win_h = static_cast<float>(def->bottom - def->top);
    const auto center_trunc = [](float span) {
      return std::trunc(span / 2.0F);
    };
    const float win_x = panel.x + center_trunc(panel.w - win_w);
    const float win_y = panel.y + center_trunc(panel.h - win_h);
    out.window = {win_x, win_y, win_w, win_h};
    out.from_ditl = true;

    for (const auto &item : *items) {
      DockedItem d;
      d.rect.x = win_x + static_cast<float>(item.left);
      d.rect.y = win_y + static_cast<float>(item.top);
      d.rect.w = static_cast<float>(item.right - item.left);
      d.rect.h = static_cast<float>(item.bottom - item.top);
      const int w = item.right - item.left;
      const int h = item.bottom - item.top;
      if (w == 145 && h == 25) {
        d.kind = DockedItemKind::kButton;
      } else if (w > 500 && h > 200) {
        d.kind = DockedItemKind::kOuterPanel;
      } else if (w > 250 && h > 150) {
        d.kind = DockedItemKind::kInnerPanel;
      } else if (w > 250 && h < 30) {
        d.kind = DockedItemKind::kTitleBand;
      } else {
        d.kind = DockedItemKind::kOrnament;
      }
      out.items.push_back(d);
    }
    return true;
  }
  NovaLog::Todo("Spaceport DLOG/DITL 0x3e8 not usable; using hardcoded dock "
                "grid layout");
  // Minimal fallback: the eight buttons in the reference two-column grid.
  out.from_ditl = false;
  out.window = {panel.x, panel.y, 618.0F, 517.0F};
  for (std::size_t i = 0; i < button_count; ++i) {
    const std::size_t side = i >= kRows ? 1 : 0;
    const std::size_t row = i % kRows;
    DockedItem d;
    d.kind = DockedItemKind::kButton;
    d.rect.x = panel.x + (side == 0 ? kFallback.left_x : kFallback.right_x);
    d.rect.y = panel.y + kFallback.row0_y +
               static_cast<float>(row) * kFallback.row_pitch;
    d.rect.w = kFallback.width;
    d.rect.h = kFallback.height;
    out.items.push_back(d);
  }
  return false;
}

constexpr std::size_t kDockedButtonRows = 4;

// Real docked button order (top-to-bottom per column), mirroring the original
// Spaceport screen: LEFT column sits Bar, Mission BBS, Trade center, Repair;
// RIGHT column sits Shipyard, Outfitter, Refuel, Leave. The extra starmap
// service has no on-screen slot (it is still reachable via the number keys).
constexpr LandedService kLeftColumnServices[kDockedButtonRows] = {
    LandedService::kBar,          // top
    LandedService::kMissionBoard, // mission BBS
    LandedService::kBuySellCargo, // trade center
    LandedService::kRepair,       // bottom
};
constexpr LandedService kRightColumnServices[kDockedButtonRows] = {
    LandedService::kShipyard, // top
    LandedService::kOutfit,   // outfitter
    LandedService::kRefuel,   //
    LandedService::kLaunch,   // leave (bottom)
};

const LandedService *kColumnServices[2] = {kLeftColumnServices,
                                           kRightColumnServices};

// The service at a grid (column, row), mirroring BuildServiceButtons' slot
// assignment so navigation and the DITL button placement stay in sync.
LandedService NovaDialog_DockedServiceAt(std::size_t side, std::size_t row) {
  return kColumnServices[side][row];
}

// Maps an on-screen docked service to its grid (column, row), or nullopt when
// it has no on-screen slot (e.g. the starmap, reachable only via number keys).
std::optional<std::pair<std::size_t, std::size_t>>
NovaDialog_DockedGridOf(LandedService svc) {
  for (std::size_t side = 0; side < 2; ++side) {
    for (std::size_t row = 0; row < kDockedButtonRows; ++row) {
      if (kColumnServices[side][row] == svc) {
        return std::pair{side, row};
      }
    }
  }
  return std::nullopt;
}

namespace {
// Builds the service buttons from a laid-out dock: the DITL's eight 145x25
// button rects split into left/right columns, each sorted top-to-bottom. Each
// physical button is assigned the real service it represents (see the two
// k*ColumnServices tables), so the label and dispatch match the original
// Spaceport screen. `slot` carries the LandedService value (not a packed
// column*4+row index), so the draw/hit-test/navigation resolve the same
// service the number keys select.
std::vector<ServiceButton> BuildServiceButtons(const DockedLayout &layout) {
  std::vector<ServiceButton> buttons;
  buttons.reserve(kDockedButtonRows * 2U);

  // Collect the layout's button items into left/right columns by screen x,
  // then sort each column top-to-bottom before assigning the per-slot service.
  std::vector<SDL_FRect> cols[2];  // [0]=left, [1]=right
  for (const auto &item : layout.items) {
    if (item.kind != DockedItemKind::kButton) {
      continue;
    }
    cols[item.rect.x < 400.0F ? 0U : 1U].push_back(item.rect);
  }
  for (std::size_t side = 0; side < 2; ++side) {
    auto &col = cols[side];
    std::sort(col.begin(), col.end(),
              [](const SDL_FRect &a, const SDL_FRect &b) { return a.y < b.y; });
    for (std::size_t row = 0; row < col.size(); ++row) {
      if (row >= kDockedButtonRows) {
        continue;  // only four on-screen rows per column
      }
      const LandedService svc =
          (side == 0 ? kLeftColumnServices[row] : kRightColumnServices[row]);
      buttons.push_back(ServiceButton{col[row],
                                      static_cast<std::uint8_t>(svc)});
    }
  }
  return buttons;
}

} // namespace

namespace {

// Human-readable service-row labels for the MVP menu. The original draws these
// as PICT sub-window frames (Nova Graphics 3: 0x2135 Shipyard, 0x2136 Outfit,
// 0x2137 Bar, 0x2139 Mission BBS, 0x213e Trade, ...); we fall back to text so
// the MVP is
// navigable.
const char *ServiceLabel(LandedService t) {
  switch (t) {
  case LandedService::kLaunch:
    return "Leave";
  case LandedService::kRefuel:
    return "Refuel";
  case LandedService::kRepair:
    return "Repair";
  case LandedService::kBuySellCargo:
    return "Trade center";
  case LandedService::kOutfit:
    return "Outfitter";
  case LandedService::kShipyard:
    return "Shipyard";
  case LandedService::kBar:
    return "Bar";
  case LandedService::kStarmap:
    return "Starmap";
  case LandedService::kMissionBoard:
    return "Mission BBS";
  case LandedService::kCount:
    break;
  }
  return "";
}

// Draws the docked-screen backdrop + panels + header + service list with the
// real screen fonts: the destination title in Chicago (charcoal) and the status
// lines in Geneva. The backdrop PICT (0x2134) fills the whole 640x480 `panel`;
// the panels, title band, and buttons are positioned from the laid-out DITL
// items (`layout`) rather than hardcoded geometry.
void DrawLandedMenu(SdlPlatform &platform,
                    NovaFontCache &font_cache,
                    const ServicesButtonArt &buttons,
                    const GameState &state,
                    const LandedContext &ctx,
                    SDL_Texture *destination_art,
                    const SDL_FRect &panel,
                    const DockedLayout &layout,
                    const std::vector<ServiceButton> &button_rects,
                    std::optional<std::uint8_t> hovered) {
  SDL_Renderer *renderer = platform.renderer();
  const auto *st = state.scenario.Stellar(ctx.stellar_id);

  // The backdrop fills the whole screen; the panel is the full 640x480 rect.
  SDL_SetRenderDrawColor(renderer, 1, 4, 12, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(renderer);
  if (destination_art != nullptr) {
    SDL_RenderTexture(renderer, destination_art, nullptr, &panel);
  }

  const SDL_Color kTitle{202, 224, 255, 255};    // bright rows / highlight
  const SDL_Color kBody{128, 170, 210, 255};     // dim rows
  const SDL_Color kSelected{142, 209, 255, 255}; // selected row
  const SDL_Color kPanel{16, 40, 72, 255};       // flat panel frame fill
  const SDL_Color kPanelBorder{80, 140, 190, 255};

  // Draw the DITL panel/frame rects as flat frames (the real PICT sub-window
  // frames 0x2135.. are out of scope, so a fill + border stands in).
  SDL_FRect title_band = panel;
  SDL_FRect status_panel = {0.0F, 0.0F, 0.0F, 0.0F};
  for (const auto &item : layout.items) {
    if (item.kind == DockedItemKind::kOuterPanel ||
        item.kind == DockedItemKind::kInnerPanel) {
      SDL_SetRenderDrawColor(renderer, kPanel.r, kPanel.g, kPanel.b,
                             kPanel.a);
      SDL_RenderFillRect(renderer, &item.rect);
      SDL_SetRenderDrawColor(renderer, kPanelBorder.r, kPanelBorder.g,
                             kPanelBorder.b, kPanelBorder.a);
      SDL_RenderRect(renderer, &item.rect);
      if (item.kind == DockedItemKind::kInnerPanel) {
        status_panel = item.rect;
      }
    } else if (item.kind == DockedItemKind::kTitleBand) {
      title_band = item.rect;
    }
  }

  // Destination name centred in the DITL header band (Chicago/title font),
  // falling back to a top-of-panel line if the band is unavailable.
  std::string title = st ? st->name : std::string("(unknown stellar)");
  title += " -- services";
  const float band_cy = title_band.y + title_band.h / 2.0F;
  NovaText_DrawCentered(platform,
                        font_cache,
                        NovaFontFamily::kChicago,
                        18.0F,
                        kNovaFontStyleRegular,
                        kTitle,
                        title_band.x,
                        title_band.x + title_band.w,
                        band_cy,
                        title);

  // Credits/fuel/hull status lines in the inner content panel (Geneva body
  // font), or the panel top-left when no inner panel was laid out.
  const bool have_status = status_panel.w > 0.0F && status_panel.h > 0.0F;
  const float body_x = have_status ? status_panel.x + 12.0F
                                   : panel.x + 12.0F;
  float baseline =
      have_status ? status_panel.y + 66.0F : panel.y + 60.0F;

  baseline += 24.0F;
  const std::string credits =
      "Credits: " + std::to_string(state.player.credits);
  NovaText_Draw(platform,
                font_cache,
                NovaFontFamily::kGeneva,
                12.0F,
                kNovaFontStyleRegular,
                kBody,
                body_x,
                baseline,
                credits);

  const auto *eff = &state.cached_stats; // set during landing/refuel/repair
  const float cap = eff ? eff->fuel_capacity : 0.0F;
  char fuel[96];
  std::snprintf(fuel,
                sizeof(fuel),
                "Fuel: %.0f / %.0f      Hull: %.0f / %.0f",
                state.player.fuel_points,
                cap,
                state.player.armor_points,
                eff ? eff->max_armor_points : 0.0F);
  baseline += 18.0F;
  NovaText_Draw(platform,
                font_cache,
                NovaFontFamily::kGeneva,
                12.0F,
                kNovaFontStyleRegular,
                kBody,
                body_x,
                baseline,
                fuel);

  // The service buttons, drawn with the real three-state button art and their
  // labels centred in the body font. The selected (keyboard-focused) slot and
  // the mouse-hovered slot both render in the hover state, mirroring the
  // original highlighting the focused service. Label baseline is centred on the
  // +5px-below-centre rule the original uses (NovaUi_DrawThreeStateButton).
  for (std::size_t i = 0; i < button_rects.size(); ++i) {
    const auto slot = button_rects[i].slot;
    const bool focused = static_cast<int>(slot) ==
                         static_cast<int>(ctx.selection);
    const bool hovered_by_mouse = hovered.has_value() && *hovered == slot;
    const auto state =
        (focused || hovered_by_mouse) ? ButtonState::kHover
                                      : ButtonState::kNormal;
    buttons.Draw(platform, button_rects[i].rect, state);
    const SDL_Color &label_color =
        (focused || hovered_by_mouse) ? kSelected : kBody;
    const float label_baseline =
        button_rects[i].rect.y +
        std::max(9.0F, button_rects[i].rect.h / 2.0F + 5.0F);
    NovaText_DrawCentered(platform,
                          font_cache,
                          NovaFontFamily::kGeneva,
                          12.0F,
                          (focused || hovered_by_mouse) ? kNovaFontStyleBold
                                                        : kNovaFontStyleRegular,
                          label_color,
                          button_rects[i].rect.x,
                          button_rects[i].rect.x + button_rects[i].rect.w,
                          label_baseline,
                          ServiceLabel(static_cast<LandedService>(slot)));
  }

  // Footer hint row in the body font, just below the docked content (the
  // window bottom when laid out; otherwise the top of the panel).
  const float footer_y =
      layout.from_ditl ? layout.window.y + layout.window.h - 20.0F
                       : panel.y + 52.0F;
  NovaText_DrawCentered(platform,
                        font_cache,
                        NovaFontFamily::kGeneva,
                        11.0F,
                        kNovaFontStyleRegular,
                        kBody,
                        panel.x + 300.0F,
                        panel.x + 530.0F,
                        footer_y,
                        "WASD move, Enter select, Q launch, Esc back");
}

// Handles one service selection from the docked menu. Returns the exit intent:
// kLaunched when the player leaves the dock, otherwise kServiceComplete (the
// sub-screen ran and control returns to the menu). The buy/sell/outfit/shipyard
// /bar/starmap/mission sub-screens are all mocked for the MVP and return
// immediately; a reconstructed sub-screen would instead open its own modal and
// return kServiceComplete when it closes.
LandedExit
DispatchService(SdlPlatform &platform, GameState &state, LandedContext &ctx) {
  (void)platform;
  switch (ctx.selection) {
  case LandedService::kLaunch:
    NovaLog::Info("launching from stellar {} back into space",
                  static_cast<int>(ctx.stellar_id));
    return LandedExit::kLaunched;

  case LandedService::kRefuel: {
    // The travel-services fuel price is a shop constant placeholder; the
    // original prices refuelling per unit from an outfit/tech-level table
    // (TODO(decomp)). 2 credits per fuel point below a typical capacity is a
    // working default.
    constexpr std::int32_t kFuelPrice = 2; // credits per fuel point
    NovaLanded_Refuel(state, kFuelPrice);
    return LandedExit::kServiceComplete;
  }

  case LandedService::kRepair: {
    constexpr std::int32_t kRepairPrice = 10; // credits per armor point
    NovaLanded_Repair(state, kRepairPrice);
    return LandedExit::kServiceComplete;
  }

  case LandedService::kBuySellCargo:
  case LandedService::kOutfit:
  case LandedService::kShipyard:
  case LandedService::kBar:
  case LandedService::kStarmap:
  case LandedService::kMissionBoard:
    NovaLog::Todo(
        "landed service '{}' at stellar {} not reconstructed (sub-window "
        "mocked)",
        ServiceLabel(ctx.selection),
        static_cast<int>(ctx.stellar_id));
    return LandedExit::kServiceComplete;

  case LandedService::kCount:
    break;
  }
  return LandedExit::kServiceComplete;
}

} // namespace

// ---------------------------------------------------------------------------
// Landed window modal run loop.
// ---------------------------------------------------------------------------
LandedExit NovaLanded_RunWindow(SdlPlatform &platform,
                                GameState &state,
                                LandedContext &ctx) {
  NovaLog::Info("opening landed services window at stellar {}",
                static_cast<int>(ctx.stellar_id));

  // Load the docked-screen backdrop. The original fills the docking window
  // with destination-art PICT 0x2134 (618x517) via g_travel_overlay_sprite_handle
  // (Ghidra FUN_0048e970); the earlier 263x185 PICT 0x2137 belongs to the small
  // travel-services modal, not the docked screen, so it is only a fallback if
  // the large backdrop is unavailable. Either image is drawn across the full
  // 640x480 dock panel (the panel is not shrunk to the art's natural size).
  std::unique_ptr<SdlTexture> destination_art;
  const auto backdrop_data = NovaResource_LoadPictData(0x2134);
  const auto backdrop = backdrop_data
                            ? Resource_LoadPictAsImage(*backdrop_data)
                            : std::nullopt;
  if (backdrop_data && backdrop) {
    destination_art = SdlTexture::Create(platform.renderer(),
                                         backdrop->width,
                                         backdrop->height,
                                         backdrop->rgba_pixels);
    NovaLog::Info("landed dock backdrop PICT 0x2134 ({}x{})",
                  backdrop->width, backdrop->height);
  } else {
    NovaLog::Todo("docked backdrop PICT 0x2134 unavailable; falling back to "
                  "0x2137");
    if (const auto art_data = NovaResource_LoadPictData(0x2137)) {
      if (const auto art = Resource_LoadPictAsImage(*art_data)) {
        destination_art = SdlTexture::Create(platform.renderer(),
                                             art->width,
                                             art->height,
                                             art->rgba_pixels);
      }
    }
  }
  if (!destination_art) {
    NovaLog::Warn("no docked backdrop decoded; drawing a flat backdrop");
  }
  const SDL_FRect panel = FullScreenPanel();

  // Lay out the docked screen from the real Spaceport DLOG/DITL 0x3e8
  // (centered window + per-item screen rects). Falls back to a hardcoded
  // grid when the dialog resources are unavailable.
  DockedLayout layout;
  NovaDialogWindow_Layout(panel, layout);

  // Load the docked buttons' real three-state strip art (normal 0x1d4c..,
  // pressed 0x1d4f.., grey 0x1d52..) and lay out their desk rects. If the
  // strips are unavailable the buttons still render as flat fills behind the
  // labels.
  ServicesButtonArt button_art;
  const bool have_buttons = button_art.Initialize(platform);
  if (!have_buttons) {
    NovaLog::Warn("service button art unavailable");
  }
  const std::vector<ServiceButton> button_rects =
      BuildServiceButtons(layout);

  NovaLog::Todo(
      "docked screen geometry (panels, title band, buttons) is laid out from "
      "the real DITL 0x3e8, but the PICT sub-window frames (0x2135..) and the "
      "sub-window modals are out of scope");

  // Font subsystem init (SDL3_ttf) is refcounted and managed lazily by the
  // NovaFontCache itself (TTF_Init on first use, TTF_Quit in ~NovaFontCache),
  // so repeated dockings stay stable. Here we only surface the availability of
  // the faces the original ships/substitutes so a missing bundle is loud.
  NovaFontCache font_cache;
  NovaLog::Info("landed window font families available: Charcoal={} "
                "Geneva={} Times={} Helvetica={} NewYork={}",
                font_cache.IsFamilyAvailable(NovaFontFamily::kChicago),
                font_cache.IsFamilyAvailable(NovaFontFamily::kGeneva),
                font_cache.IsFamilyAvailable(NovaFontFamily::kTimes),
                font_cache.IsFamilyAvailable(NovaFontFamily::kHelvetica),
                font_cache.IsFamilyAvailable(NovaFontFamily::kNewYork));
  if (!font_cache.IsFamilyAvailable(NovaFontFamily::kChicago) ||
      !font_cache.IsFamilyAvailable(NovaFontFamily::kGeneva)) {
    NovaLog::Warn(
        "bundled Charcoal.ttf/Geneva.ttf not found next to the executable "
        "(looked in EV Nova/); text will fall back to debug font");
  }

  bool entered_sub_screen = false;

  // The docked services sit in a 2-column x 4-row grid whose physical button
  // order is the real Spaceport screen (left col: Bar, Mission BBS, Trade,
  // Repair; right col: Shipyard, Outfitter, Refuel, Leave; see
  // k*ColumnServices). Up/Down move a row within the current column
  // (wrapping), Left/Right cross to the other column at the same row.
  auto step_row = [&](int row_delta) {
    if (auto pos = NovaDialog_DockedGridOf(ctx.selection)) {
      const int row = static_cast<int>(pos->second) + row_delta;
      const std::size_t wrapped =
          static_cast<std::size_t>((row + static_cast<int>(kDockedButtonRows)) %
                                   static_cast<int>(kDockedButtonRows));
      ctx.selection = NovaDialog_DockedServiceAt(pos->first, wrapped);
    }
  };
  auto step_column = [&] {
    // Cross over to the other column at the same row (within the on-screen
    // slots).
    if (auto pos = NovaDialog_DockedGridOf(ctx.selection)) {
      ctx.selection =
          NovaDialog_DockedServiceAt(1U - pos->first, pos->second);
    }
  };

  while (!platform.quit_requested()) {
    // Compute the mouse-hovered service (for hover state on the buttons).
    std::optional<std::uint8_t> hovered;
    if (!entered_sub_screen) {
      hovered = ServiceButtonAt(button_rects, platform.mouse_position());
    }
    // Draw the current face. When a sub-screen is "open" the original swaps to
    // a nested modal widget; the MVP keeps the same menu surface and only
    // distinguishes via the hint, so we always redraw the list.
    DrawLandedMenu(platform,
                    font_cache,
                    button_art,
                    state,
                    ctx,
                    destination_art ? destination_art->get() : nullptr,
                    panel,
                    layout,
                    button_rects,
                    hovered);
    SDL_RenderPresent(platform.renderer());

    // Poll discrete raw keys for the modal (dedicated channel, so it never
    // interferes with the spaceflight flight controls).
    for (std::optional<TextInput> in; (in = platform.PollTextEvent());) {
      if (in->key == TextKey::escape) {
        // Esc leaves a sub-screen back to the menu, or launches from the menu.
        if (entered_sub_screen) {
          entered_sub_screen = false;
        } else {
          return LandedExit::kLaunched;
        }
        continue;
      }
      if (in->key == TextKey::enter) {
        // Enter activates the focused service (same as 'e').
        LandedExit exit = DispatchService(platform, state, ctx);
        if (exit == LandedExit::kLaunched) {
          return LandedExit::kLaunched;
        }
        entered_sub_screen = false;
        continue;
      }
      if (in->key == TextKey::primary) {
        // Left-click a service button: select it and activate (mirrors the
        // original's mouse button driving the services buttons).
        if (!entered_sub_screen) {
          if (const auto slot =
                  ServiceButtonAt(button_rects, platform.mouse_position())) {
            ctx.selection = static_cast<LandedService>(*slot);
            LandedExit exit = DispatchService(platform, state, ctx);
            if (exit == LandedExit::kLaunched) {
              return LandedExit::kLaunched;
            }
            entered_sub_screen = false;
          }
        }
        continue;
      }
      if (in->key == TextKey::character) {
        switch (in->character) {
        case 'w':
        case 'k':
          if (!entered_sub_screen) {
            step_row(-1);
          }
          break;
        case 's':
        case 'j':
          if (!entered_sub_screen) {
            step_row(1);
          }
          break;
        case 'a':
        case 'h':
        case 'd':
        case 'l':
          // Left/Right cross between the two docked button columns.
          if (!entered_sub_screen) {
            step_column();
          }
          break;
        case 'e': {
          LandedExit exit = DispatchService(platform, state, ctx);
          if (exit == LandedExit::kLaunched) {
            return LandedExit::kLaunched;
          }
          entered_sub_screen = false;
          break;
        }
        case '1':
          if (!entered_sub_screen) {
            const auto n = static_cast<int>(in->character - '0');
            if (n >= 0 && n < static_cast<int>(LandedService::kCount)) {
              ctx.selection = static_cast<LandedService>(n);
            }
          }
          break;
        default:
          break;
        }
      }
    }
    SDL_Delay(16);
  }
  return LandedExit::kQuit;
}

} // namespace game
