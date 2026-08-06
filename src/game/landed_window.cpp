#include "landed_window.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../pict_image.hpp"
#include "../sdl_platform.hpp"
#include "nova_font.hpp"
#include "outfit.hpp"
#include "scenario_data.hpp"
#include "targeting.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
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

namespace {

// Row-layout for the six service entries, shared by the draw pass and the
// mouse hit-test so a click lands on the same row the text is drawn on.
// Rows are Geneva body text at 13 logical px whose baseline is kTop + i*kRowH;
// we treat the clickable band as the glyph band above that baseline.
constexpr int kServiceRowTop = 110;
constexpr int kServiceRowHeight = 24;
constexpr int kServiceRowFontSize = 13;

// Human-readable service-row labels for the MVP menu. The original draws these
// as PICT service icons (0x2152..0x2178); we fall back to text so the MVP is
// navigable.
const char *ServiceLabel(LandedService t) {
  switch (t) {
  case LandedService::kLaunch:
    return "Launch into space (Q)";
  case LandedService::kRefuel:
    return "Refuel";
  case LandedService::kRepair:
    return "Repair";
  case LandedService::kBuySellCargo:
    return "Buy/Sell cargo";
  case LandedService::kOutfit:
    return "Outfitting";
  case LandedService::kShipyard:
    return "Shipyard";
  case LandedService::kBar:
    return "Bar";
  case LandedService::kStarmap:
    return "Starmap";
  case LandedService::kMissionBoard:
    return "Mission computer";
  case LandedService::kCount:
    break;
  }
  return "";
}

// Returns the service whose clickable row contains the logical-space point, or
// std::nullopt when the point is not over any service row. The logo area / art
// and the footer are not selectable.
std::optional<LandedService> ServiceAtPoint(SDL_FPoint point) {
  const float left = 10.0F;
  const float right = 620.0F;
  if (point.x < left || point.x > right) {
    return std::nullopt;
  }
  for (int i = 0; i < static_cast<int>(LandedService::kCount); ++i) {
    const float row_top =
        static_cast<float>(kServiceRowTop + i * kServiceRowHeight -
                           kServiceRowFontSize);
    const float row_bottom =
        static_cast<float>(kServiceRowTop + i * kServiceRowHeight);
    if (point.y >= row_top && point.y <= row_bottom) {
      return static_cast<LandedService>(i);
    }
  }
  return std::nullopt;
}

// Draws the landed window header + service list with the real screen fonts:
// the destination title in Chicago (charcoal) and the rows/bars in Geneva,
// mirroring the original's window (title = family 0/Chicago UI font, body =
// family 3/Geneva). Text positions are in the original 640x480 logical space.
// When `destination_art` is non-null it is stretched across the window rect
// first (mirroring NovaUi_DrawTravelDestinationServicesWindow blitting the
// destination PICT across the window), so the piece is recognisable beneath
// the services list.
void DrawLandedMenu(SdlPlatform &platform,
                    NovaFontCache &font_cache,
                    const GameState &state,
                    const LandedContext &ctx,
                    SDL_Texture *destination_art) {
  SDL_Renderer *renderer = platform.renderer();
  const auto *st = state.scenario.Stellar(ctx.stellar_id);

  SDL_SetRenderDrawColor(renderer, 1, 4, 12, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(renderer);
  if (destination_art != nullptr) {
    const SDL_FRect full{0.0F, 0.0F, 640.0F, 480.0F};
    SDL_RenderTexture(renderer, destination_art, nullptr, &full);
  }

  const SDL_Color kTitle{202, 224, 255, 255};    // bright rows / highlight
  const SDL_Color kBody{128, 170, 210, 255};     // dim rows
  const SDL_Color kSelected{142, 209, 255, 255}; // selected row

  // Header: destination name (+ service marker) as the window title font.
  std::string title = st ? st->name : std::string("(unknown stellar)");
  title += " -- services";
  NovaText_Draw(platform,
                font_cache,
                NovaFontFamily::kChicago,
                18.0F,
                kNovaFontStyleRegular,
                kTitle,
                20.0F,
                30.0F, // baseline
                title);

  // Credits / fuel / hull status bars in the Geneva body font.
  const std::string credits =
      "Credits: " + std::to_string(state.player.credits);
  NovaText_Draw(platform,
                font_cache,
                NovaFontFamily::kGeneva,
                12.0F,
                kNovaFontStyleRegular,
                kBody,
                20.0F,
                56.0F,
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
  NovaText_Draw(platform,
                font_cache,
                NovaFontFamily::kGeneva,
                12.0F,
                kNovaFontStyleRegular,
                kBody,
                20.0F,
                74.0F,
                fuel);

  const int kTop = kServiceRowTop;
  const int kRowH = kServiceRowHeight;
  for (int i = 0; i < static_cast<int>(LandedService::kCount); ++i) {
    const auto svc = static_cast<LandedService>(i);
    const bool selected = (i == static_cast<int>(ctx.selection));
    const std::string row =
        std::string(selected ? "> " : "  ") + ServiceLabel(svc);
    const SDL_Color &color = selected ? kSelected : kBody;
    NovaText_Draw(platform,
                  font_cache,
                  NovaFontFamily::kGeneva,
                  static_cast<float>(kServiceRowFontSize),
                  selected ? kNovaFontStyleBold : kNovaFontStyleRegular,
                  color,
                  20.0F,
                  static_cast<float>(kTop + i * kRowH),
                  row);
  }

  // Footer hint row in the body font.
  NovaText_Draw(platform,
                font_cache,
                NovaFontFamily::kGeneva,
                11.0F,
                kNovaFontStyleRegular,
                kBody,
                20.0F,
                464.0F,
                "Click a service, Up/Down or W/S move, Enter/E select, Q launch, "
                "Esc back");
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

  // Load the stellar's destination art. The original picks PICT 0x2138 when a
  // custom selection-dialog variant is installed (g_selection_dialog_variant >=
  // 0x80), else the default 0x2137; both are 16-bit DirectBitsRect PICTs. We
  // use the default unless the scenario data later surfaces a variant id. The
  // six PICT service icons and the GVNO UiWindow dialog (0x3f5/0x3fd) are still
  // out of scope, so the service list stays as screen-font text.
  std::unique_ptr<SdlTexture> destination_art;
  if (const auto art_data = NovaResource_LoadPictData(0x2137)) {
    if (const auto art = Resource_LoadPictAsImage(*art_data)) {
      destination_art = SdlTexture::Create(platform.renderer(),
                                           art->width,
                                           art->height,
                                           art->rgba_pixels);
      if (!destination_art) {
        NovaLog::Error("destination art 0x2137 decoded but SDL texture upload "
                       "failed");
      } else {
        NovaLog::Info("landed window destination art 0x2137 ({}x{})",
                      art->width, art->height);
      }
    } else {
      NovaLog::Todo("destination art PICT 0x2137 failed to decode");
    }
  } else {
    NovaLog::Todo("destination art PICT 0x2137 not found; drawing flat "
                  "backdrop");
  }

  NovaLog::Todo(
      "landed window service list still uses screen fonts; the original "
      "UiWindow dialog 0x3f5/0x3fd six PICT service buttons + icon art are "
      "out of scope");

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

  auto step_selection = [&](int delta) {
    const int count = static_cast<int>(LandedService::kCount);
    int next = (static_cast<int>(ctx.selection) + delta + count) % count;
    ctx.selection = static_cast<LandedService>(next);
  };

  while (!platform.quit_requested()) {
    // Draw the current face. When a sub-screen is "open" the original swaps to
    // a nested modal widget; the MVP keeps the same menu surface and only
    // distinguishes via the hint, so we always redraw the list.
    DrawLandedMenu(platform,
                    font_cache,
                    state,
                    ctx,
                    destination_art ? destination_art->get() : nullptr);
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
        // Left-click a service row: select it and activate (mirrors the
        // original's mouse button driving the services buttons).
        if (!entered_sub_screen) {
          if (const auto svc = ServiceAtPoint(platform.mouse_position())) {
            ctx.selection = *svc;
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
            step_selection(-1);
          }
          break;
        case 's':
        case 'j':
          if (!entered_sub_screen) {
            step_selection(1);
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
