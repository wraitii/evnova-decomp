// Docked Trade center window (DLOG/DITL 0x3e9).
//
// Split out of the original docked_dialog.cpp.

#include "docked_dialog_internal.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../util/color.hpp"
#include "button_label.hpp"
#include "hud_overlay.hpp"
#include "nova_font.hpp"
#include "outfit.hpp"
#include "scenario_data.hpp"
#include "services_buttons.hpp"
#include "trade_center.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace game {

using evnova::util::ToSdlColor;

namespace {

// ---------------------------------------------------------------------------
// Ghidra 0x0048c730 NovaUi_RunTradeCenterWindow modal (DLOG/DITL 0x3e9, frame
// PICT 0x213e). The SDL-free price/transaction model is in trade_center.cpp;
// the input handler 0x0048d190 and redraw 0x0048d6f0 run inline here.
// ---------------------------------------------------------------------------

// Window furniture palette from Settings_InitColors (0x004ad7c0):
// DAT_00733b50 grey 0xc000 is the header text, DAT_00733b5c grey 0x4000 is the
// table frame/grid. PTR_DAT_00575acc is the default (white) window text.
constexpr SDL_Color kHeaderText{192, 192, 192, SDL_ALPHA_OPAQUE};
constexpr SDL_Color kGridLine{64, 64, 64, SDL_ALPHA_OPAQUE};
constexpr SDL_Color kDefaultText{255, 255, 255, SDL_ALPHA_OPAQUE};

// The three-state button captions come from the shared label table that
// NovaUi_InitThreeStateButtonArt (0x004a2f50) fills from STR# 0x96 entries
// 1..0x3d. NovaUi_TradeCenterDrawButtons indexes it with DAT_007d82e8 =
// {4, 1, 2}, i.e. the 1-based STR# entries below: "Done", "Buy", "Sell".
constexpr std::uint16_t kLeaveLabelEntry = 5; // table index 4
constexpr std::uint16_t kBuyLabelEntry = 2;   // table index 1
constexpr std::uint16_t kSellLabelEntry = 3;  // table index 2

struct TradeCenterLayout {
  SDL_FRect frame{};
  SDL_FRect header{};
  std::array<SDL_FRect, kTradeCenterRowCount> rows{};
  SDL_FRect summary{};
  SDL_FRect banner{};
  SDL_FRect leave{};
  SDL_FRect buy{};
  SDL_FRect sell{};
};

// Loads the window geometry from DITL 0x3e9 exactly the way the original
// addresses it: UiPanel_GetEntryInfo entry N is DITL item N-1 (see
// docs/dlog_ditl_dialog_format.md section 4). The frame is the DLOG bounds,
// centred on the logical playfield.
[[nodiscard]] std::optional<TradeCenterLayout>
LayoutTradeCenter(const SdlPlatform &platform) {
  const auto definition = NovaResource_LoadDialogDefinition(0x3e9);
  const auto items =
      definition ? NovaResource_LoadDialogItems(definition->dialog_item_list_id)
                 : std::nullopt;
  if (!definition || !items) {
    NovaLog::Todo("Trade Center DLOG/DITL 0x3e9 unavailable; refusing to use a "
                  "synthetic layout");
    return std::nullopt;
  }

  const float width = static_cast<float>(definition->right - definition->left);
  const float height = static_cast<float>(definition->bottom - definition->top);
  const SDL_FPoint output = platform.logical_playfield_size();
  const SDL_FPoint origin{(output.x - width) / 2.0F,
                          (output.y - height) / 2.0F};
  TradeCenterLayout layout;
  layout.frame = {origin.x, origin.y, width, height};
  const auto item_rect = [origin](const NovaDialogItem &item) {
    return SDL_FRect{origin.x + static_cast<float>(item.left),
                     origin.y + static_cast<float>(item.top),
                     static_cast<float>(item.right - item.left),
                     static_cast<float>(item.bottom - item.top)};
  };
  for (const auto &item : *items) {
    // Rows are UiPanel entries 4..11 = DITL items 3..10.
    if (item.index >= 3 && item.index <= 10) {
      layout.rows[item.index - 3] = item_rect(item);
      continue;
    }
    switch (item.index) {
    case 0: // entry 1: leave/Done
      layout.leave = item_rect(item);
      break;
    case 2: // entry 3: column header
      layout.header = item_rect(item);
      break;
    case 11: // entry 0xc: cargo summary
      layout.summary = item_rect(item);
      break;
    case 12: // entry 0xd: buy
      layout.buy = item_rect(item);
      break;
    case 13: // entry 0xe: sell
      layout.sell = item_rect(item);
      break;
    case 14: // entry 0xf: disaster banner
      layout.banner = item_rect(item);
      break;
    default:
      break;
    }
  }

  const bool rows_valid =
      std::all_of(layout.rows.begin(), layout.rows.end(), [](SDL_FRect r) {
        return r.w > 0.0F && r.h > 0.0F;
      });
  if (layout.header.w <= 0.0F || layout.summary.w <= 0.0F ||
      layout.banner.w <= 0.0F || layout.leave.w <= 0.0F ||
      layout.buy.w <= 0.0F || layout.sell.w <= 0.0F || !rows_valid) {
    NovaLog::Todo("Trade Center DITL 0x3e9 is missing a required control rect");
    return std::nullopt;
  }
  return layout;
}

// Three-state button body + label. Ghidra 0x004a06a0
// NovaUi_TradeCenterDrawButtons runs inline here; the original only ever calls
// it with the "no button highlighted" index from the redraw, so no hover art.
void DrawTradeButton(SdlPlatform &platform,
                     NovaFontCache &font_cache,
                     const ServicesButtonArt &button_art,
                     const SDL_FRect &rect,
                     std::string_view label,
                     bool enabled) {
  button_art.Draw(
      platform, rect, enabled ? ButtonState::kNormal : ButtonState::kDisabled);
  const SDL_Color color =
      enabled ? kDefaultText : SDL_Color{128, 128, 128, 255};
  DrawThreeStateButtonLabel(platform, font_cache, rect, label, color);
}

// Ghidra 0x0048d6f0 NovaUi_RedrawTradeCenterWindow. Draws the 8-row price
// list, cargo summary and disaster banner over the docked backdrop.
void DrawTradeCenterScreen(SdlPlatform &platform,
                           NovaFontCache &font_cache,
                           const ServicesButtonArt &button_art,
                           const GameState &state,
                           const TradeCenterSession &session,
                           const TradeCenterLayout &layout,
                           const std::function<void()> &render_background,
                           SDL_Texture *backdrop,
                           SDL_Texture *frame,
                           NovaRgbColor list_text,
                           NovaRgbColor list_background,
                           NovaRgbColor list_hilite) {
  SDL_Renderer *renderer = platform.renderer();
  const SDL_FPoint output = platform.logical_playfield_size();
  if (render_background) {
    render_background();
  } else {
    platform.SetFullscreenPlayfield();
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);
    if (backdrop != nullptr) {
      float width = 0.0F;
      float height = 0.0F;
      SDL_GetTextureSize(backdrop, &width, &height);
      const SDL_FRect dst{
          (output.x - width) / 2.0F, (output.y - height) / 2.0F, width, height};
      SDL_RenderTexture(renderer, backdrop, nullptr, &dst);
    }
  }
  if (frame != nullptr) {
    SDL_RenderTexture(renderer, frame, nullptr, &layout.frame);
  }
  constexpr NovaFontFamily kFont = NovaFontFamily::kGeneva;
  constexpr float kSize = 9.0F;
  const SDL_Color list_text_sdl = ToSdlColor(list_text);
  const SDL_Color list_hilite_sdl = ToSdlColor(list_hilite);
  const SDL_Color list_background_sdl = ToSdlColor(list_background);

  // Column header (entry 3): framed in the table grid color, labels grey.
  SDL_SetRenderDrawColor(
      renderer, kGridLine.r, kGridLine.g, kGridLine.b, SDL_ALPHA_OPAQUE);
  SDL_RenderRect(renderer, &layout.header);
  const bool in_fleet = Ship_ComputeShipTotalCargoCapacity(state) <
                        Player_ComputeFleetCargoCapacity(state);
  const float header_right = layout.header.x + layout.header.w;
  NovaText_Draw(platform,
                font_cache,
                kFont,
                kSize,
                kNovaFontStyleRegular,
                kHeaderText,
                layout.header.x + 7.0F,
                layout.header.y + 12.0F,
                InfoString(0xc4));
  NovaText_Draw(platform,
                font_cache,
                kFont,
                kSize,
                kNovaFontStyleRegular,
                kHeaderText,
                header_right - 123.0F,
                layout.header.y + 12.0F,
                InfoString(in_fleet ? 0xc5 : 0xc6));
  NovaText_Draw(platform,
                font_cache,
                kFont,
                kSize,
                kNovaFontStyleRegular,
                kHeaderText,
                header_right - 70.0F,
                layout.header.y + 12.0F,
                InfoString(0xc7));

  for (std::size_t i = 0; i < kTradeCenterRowCount; ++i) {
    const TradeCenterRow *row = session.Row(static_cast<int>(i));
    const SDL_FRect &rect = layout.rows[i];
    const float left = rect.x;
    const float top = rect.y;
    const float right = rect.x + rect.w;
    const bool selected = static_cast<std::int16_t>(i) == session.selected;

    // Interior fill (inset 1px) then the 1px table grid on the native rect.
    const SDL_FRect interior{
        left + 1.0F, top + 1.0F, rect.w - 2.0F, rect.h - 2.0F};
    const SDL_Color &fill = selected ? list_hilite_sdl : list_background_sdl;
    SDL_SetRenderDrawColor(renderer, fill.r, fill.g, fill.b, SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(renderer, &interior);
    SDL_SetRenderDrawColor(
        renderer, kGridLine.r, kGridLine.g, kGridLine.b, SDL_ALPHA_OPAQUE);
    SDL_RenderLine(renderer, left, top, left, rect.y + rect.h - 1.0F);
    SDL_RenderLine(
        renderer, right - 1.0F, top, right - 1.0F, rect.y + rect.h - 1.0F);
    if (i == 0) {
      SDL_RenderLine(renderer, left, top, right - 1.0F, top);
    }
    if (i == static_cast<std::size_t>(kTradeCenterRowCount - 1)) {
      SDL_RenderLine(renderer,
                     left,
                     rect.y + rect.h - 1.0F,
                     right - 1.0F,
                     rect.y + rect.h - 1.0F);
    }

    if (row == nullptr || row->price == 0) {
      continue;
    }
    const std::string name =
        NovaTradeCenter_RowName(state, session, static_cast<int>(i));
    const std::int16_t held =
        NovaTradeCenter_HeldCount(state, session, static_cast<int>(i));
    std::string trend;
    if (row->has_disaster) {
      // Resource_DrawStringEntry takes 1-based entries. InfoString uses the
      // corresponding zero-based pool index, so Ghidra entries 0xcc/0xcd
      // ("Higher"/"Lower") are 0xcb/0xcc here.
      trend = InfoString(row->disaster_raised ? 0xcb : 0xcc);
    } else {
      const std::int16_t lane = row->trend;
      trend = InfoString(lane == 1 ? 0xc8 : (lane == 4 ? 0xca : 0xc9));
    }
    const std::string price = std::to_string(row->price);

    // Column x origins: the j\x9fnk rows use a 6px name inset, the six
    // commodities 7px; the held count and price are right-aligned (to
    // right-110 and right-10) and the trend badge is left-aligned at right-70.
    NovaText_Draw(platform,
                  font_cache,
                  kFont,
                  kSize,
                  kNovaFontStyleRegular,
                  list_text_sdl,
                  left + (i < kTradeCenterCommodityCount ? 7.0F : 6.0F),
                  top + 10.0F,
                  name);
    if (held > 0) {
      const std::string count = std::to_string(held);
      const float count_width = static_cast<float>(
          font_cache.TextWidth(kFont, kSize, kNovaFontStyleRegular, count));
      NovaText_Draw(platform,
                    font_cache,
                    kFont,
                    kSize,
                    kNovaFontStyleRegular,
                    list_text_sdl,
                    right - (count_width + 110.0F),
                    top + 10.0F,
                    count);
    }
    NovaText_Draw(platform,
                  font_cache,
                  kFont,
                  kSize,
                  kNovaFontStyleRegular,
                  list_text_sdl,
                  right - 70.0F,
                  top + 10.0F,
                  trend);
    const float price_width = static_cast<float>(
        font_cache.TextWidth(kFont, kSize, kNovaFontStyleRegular, price));
    NovaText_Draw(platform,
                  font_cache,
                  kFont,
                  kSize,
                  kNovaFontStyleRegular,
                  list_text_sdl,
                  right - (price_width + 10.0F),
                  top + 10.0F,
                  price);
  }

  // Cargo summary (entry 0xc): framed in the grid color, white body text.
  SDL_SetRenderDrawColor(
      renderer, kGridLine.r, kGridLine.g, kGridLine.b, SDL_ALPHA_OPAQUE);
  SDL_RenderRect(renderer, &layout.summary);
  const std::int32_t free_space = Player_ComputeFleetCargoCapacity(state) -
                                  Player_ComputeCargoAndJunkTotal(state);
  const bool summary_in_ship = Ship_ComputeShipTotalCargoCapacity(state) <
                               Player_ComputeFleetCargoCapacity(state);
  std::string summary = InfoString(0x16b);
  summary += " ";
  summary += InfoString(summary_in_ship ? 0x16c : 0x16d);
  summary += ": ";
  summary += std::to_string(std::max<std::int32_t>(0, free_space));
  summary += " ";
  summary += InfoString(free_space == 1 ? 0x0 : 0x1);
  NovaText_Draw(platform,
                font_cache,
                kFont,
                kSize,
                kNovaFontStyleRegular,
                kDefaultText,
                layout.summary.x + 7.0F,
                layout.summary.y + 14.0F,
                summary);

  // Disaster banner (entry 0xf).
  for (const DisasterDef &disaster : state.scenario.disaster_defs) {
    if (!disaster.present || disaster.days_remaining <= 0 ||
        disaster.active_stellar !=
            static_cast<std::int16_t>(session.stellar_id - kResourceIdBase)) {
      continue;
    }
    std::string banner = disaster.display_name;
    banner += " ";
    banner += InfoString(0xbf);
    banner += " ";
    banner += InfoString(disaster.price_delta < 1 ? 0xc1 : 0xc0);
    banner += " ";
    banner += InfoString(0xb4);
    banner += " ";
    banner += BarCommodityName(disaster.commodity);
    banner += ".";
    NovaText_DrawCentered(platform,
                          font_cache,
                          kFont,
                          kSize,
                          kNovaFontStyleRegular,
                          kDefaultText,
                          layout.banner.x,
                          layout.banner.x + layout.banner.w,
                          layout.banner.y + 16.0F,
                          banner);
    break;
  }

  // Buttons (entries 1/0xd/0xe). The original's draw path passes the
  // "nothing highlighted" index, so no hover art; gating is unchanged.
  const TradeCenterRow *selected_row = session.Row(session.selected);
  const bool buy_enabled =
      selected_row != nullptr &&
      NovaTradeCenter_CanBuyRow(state, session, session.selected);
  const bool sell_enabled =
      NovaTradeCenter_HasRowStock(state, session, session.selected);
  DrawTradeButton(platform,
                  font_cache,
                  button_art,
                  layout.leave,
                  LoadButtonLabel(kLeaveLabelEntry, "Done"),
                  true);
  DrawTradeButton(platform,
                  font_cache,
                  button_art,
                  layout.buy,
                  LoadButtonLabel(kBuyLabelEntry, "Buy"),
                  buy_enabled);
  DrawTradeButton(platform,
                  font_cache,
                  button_art,
                  layout.sell,
                  LoadButtonLabel(kSellLabelEntry, "Sell"),
                  sell_enabled);
}

} // namespace

// Ghidra 0x0048c730 NovaUi_RunTradeCenterWindow modal loop.
LandedExit
RunTradeCenterDialog(SdlPlatform &platform,
                     GameState &state,
                     std::int16_t stellar_id,
                     const std::function<void()> &render_background) {
  TradeCenterSession session = NovaTradeCenter_OpenSession(state, stellar_id);
  auto backdrop = LoadPictTexture(platform, kDockedBackdropPict);
  auto frame = LoadPictTexture(
      platform, NovaLanded_SubWindowFramePict(LandedService::kBuySellCargo));
  // Row palette from c\x9alr (list_text/list_background/list_hilite, loaded
  // into DAT_00735664/6a/70 at startup). The fallback matches the shipped
  // colors only if the resource is missing entirely.
  const auto ui_style = NovaResource_LoadMainMenuStyle();
  const NovaRgbColor list_text =
      ui_style ? ui_style->list_text : NovaRgbColor{255, 255, 255};
  const NovaRgbColor list_background =
      ui_style ? ui_style->list_background : NovaRgbColor{0, 0, 0};
  const NovaRgbColor list_hilite =
      ui_style ? ui_style->list_hilite : NovaRgbColor{128, 0, 0};
  if (!ui_style) {
    NovaLog::Warn(
        "c\\x9alr unavailable; using provisional trade-center colors");
  }
  ServicesButtonArt button_art;
  (void)button_art.Initialize(platform);
  NovaFontCache font_cache;
  const auto render_trade_background = [&]() {
    const auto layout = LayoutTradeCenter(platform);
    if (!layout) {
      if (render_background) {
        render_background();
      }
      return;
    }
    DrawTradeCenterScreen(platform,
                          font_cache,
                          button_art,
                          state,
                          session,
                          *layout,
                          render_background,
                          backdrop ? backdrop->get() : nullptr,
                          frame ? frame->get() : nullptr,
                          list_text,
                          list_background,
                          list_hilite);
  };
  const auto run_mission_offer = [&]() {
    return Mission_TriggerLandingInteractions(
        state,
        4,
        static_cast<std::uint32_t>(platform.gameplay_ticks_ms()),
        [&](std::int16_t mission_def) {
          return NovaMission_RunOfferWindow(platform,
                                            state,
                                            mission_def,
                                            stellar_id,
                                            render_trade_background);
        });
  };

  // The commodity-exchange modal has its own AvailLoc lane. In the original,
  // it is entered with g_misn_list_page_group = 4 and immediately dispatches
  // it; tutorial follow-ups such as the first trade task depend on that pass.
  (void)run_mission_offer();
  // Probe-harness support (docs/probe_harness.md): a `trade` command may carry
  // an explicit `tons` or `max`, consumed here so the transaction still runs
  // through this handler. A negative pending value means "the whole lot" (the
  // alt quantity prompt's default); 0 keeps the original hardcoded click
  // quantity (up to 10 tons).
  const auto trade_quantity = [&platform](std::int16_t max) -> std::int16_t {
    const std::int16_t requested =
        platform.probe().ConsumePendingTradeQuantity();
    if (requested < 0) {
      return max;
    }
    return requested > 0 ? std::min(requested, max)
                         : std::min<std::int16_t>(10, max);
  };
  while (!platform.quit_requested()) {
    state.tick_60hz =
        static_cast<std::uint32_t>(platform.gameplay_ticks_ms() * 60 / 1000);
    const auto recheck_at =
        static_cast<std::uint32_t>(state.mission_interaction_recheck_tick_60hz);
    if (static_cast<std::int32_t>(state.tick_60hz - recheck_at) >= 0) {
      (void)run_mission_offer();
    }
    const auto layout = LayoutTradeCenter(platform);
    if (!layout) {
      return LandedExit::kServiceComplete;
    }
    // Publish the commodity rows as named rects (for `click`) and as
    // label/value items (for price assertions). Rows stay index-addressable
    // even when their price is 0.
    std::vector<ProbeNamedRect> probe_controls{
        {"window", layout->frame},
        // The original's STR# caption is "Done". Keep the older semantic
        // alias for existing probes.
        {"done", layout->leave},
        {"leave", layout->leave},
        {"buy", layout->buy},
        {"sell", layout->sell}};
    for (std::size_t i = 0; i < kTradeCenterRowCount; ++i) {
      ProbeNamedRect row;
      row.name = "trade.row." + std::to_string(i);
      row.rect = layout->rows[i];
      row.has_value = true;
      row.selected = static_cast<std::int16_t>(i) == session.selected;
      row.label = NovaTradeCenter_RowName(state, session, static_cast<int>(i));
      row.value = session.rows[i].price;
      probe_controls.push_back(std::move(row));
    }
    platform.PublishProbeUiItems("trade_center", std::move(probe_controls));
    DrawTradeCenterScreen(platform,
                          font_cache,
                          button_art,
                          state,
                          session,
                          *layout,
                          render_background,
                          backdrop ? backdrop->get() : nullptr,
                          frame ? frame->get() : nullptr,
                          list_text,
                          list_background,
                          list_hilite);
    platform.Present();
    for (std::optional<TextInput> in; (in = platform.PollTextEvent());) {
      if (in->key == TextKey::escape || in->key == TextKey::enter) {
        return LandedExit::kServiceComplete;
      }
      if (in->key == TextKey::physical && in->key_code == 0xc8) {
        NovaTradeCenter_CycleSelection(session, session.selected, true);
        continue;
      }
      if (in->key == TextKey::physical && in->key_code == 0xd0) {
        NovaTradeCenter_CycleSelection(session, session.selected, false);
        continue;
      }
      if (in->key == TextKey::character) {
        const char key = static_cast<char>(
            std::tolower(static_cast<unsigned char>(in->character)));
        if (key == 'l') {
          return LandedExit::kServiceComplete;
        }
        if (key == 'b') {
          const std::int16_t max =
              NovaTradeCenter_BuyMax(state, session, session.selected);
          const std::int16_t qty =
              in->alt ? (max <= 1 ? max
                                  : RunStoreQuantityPrompt(
                                        platform, max, render_trade_background))
                      : trade_quantity(max);
          if (qty > 0) {
            (void)NovaTradeCenter_Buy(state, session, session.selected, qty);
          }
          continue;
        }
        if (key == 's') {
          const std::int16_t max =
              NovaTradeCenter_SellMax(state, session, session.selected);
          const std::int16_t qty =
              in->alt ? (max <= 1 ? max
                                  : RunStoreQuantityPrompt(
                                        platform, max, render_trade_background))
                      : trade_quantity(max);
          if (qty > 0) {
            (void)NovaTradeCenter_Sell(state, session, session.selected, qty);
          }
          continue;
        }
      }
      // Ghidra 0x004a04d0 NovaUi_TradeCenterHitTestButtons runs inline here.
      if (in->key == TextKey::primary) {
        const SDL_FPoint mouse = platform.mouse_position();
        if (Contains(layout->leave, mouse)) {
          return LandedExit::kServiceComplete;
        }
        if (Contains(layout->buy, mouse)) {
          const std::int16_t qty = trade_quantity(
              NovaTradeCenter_BuyMax(state, session, session.selected));
          (void)NovaTradeCenter_Buy(state, session, session.selected, qty);
        }
        if (Contains(layout->sell, mouse)) {
          const std::int16_t qty = trade_quantity(
              NovaTradeCenter_SellMax(state, session, session.selected));
          (void)NovaTradeCenter_Sell(state, session, session.selected, qty);
        }
        for (std::size_t i = 0; i < kTradeCenterRowCount; ++i) {
          if (Contains(layout->rows[i], mouse) && session.rows[i].price != 0) {
            session.selected = static_cast<std::int16_t>(i);
          }
        }
      }
    }
    platform.PaceFrame();
  }
  return LandedExit::kQuit;
}

} // namespace game
