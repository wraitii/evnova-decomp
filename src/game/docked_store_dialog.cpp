// Docked store windows: Outfitter and Shipyard, plus the Shipyard Ship Info
// sub-modal and the shared quantity prompt.
//
// Split out of the original docked_dialog.cpp.

#include "docked_dialog_internal.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../sdl_platform.hpp"
#include "../util/format.hpp"
#include "button_label.hpp"
#include "compatibility.hpp"
#include "hud_overlay.hpp"
#include "landed_store.hpp"
#include "mission.hpp"
#include "nova_font.hpp"
#include "outfit.hpp"
#include "scenario_data.hpp"
#include "selection_text_dialog.hpp"
#include "services_buttons.hpp"
#include "ship_visual.hpp"
#include "sprite_world.hpp"
#include "ui_dialog.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace game {

using evnova::util::GroupThousands;

// Label/unit text for the store and ship-info panels, from the game-strings
// pool STR# 0x7d2 (Nova Data 5). The original's Resource_DrawStringEntry
// (0x004cd1f0) / Resource_LoadStringEntry (0x004b8ca0) take the 1-based entry
// number; the indices passed here are the original's values minus one (pool
// indices), so InfoString adds the 1 back for the 1-based helper. The
// original also caches a few entries as Pascal strings in static storage
// (DAT_0072d3cc "ton", DAT_0072d9cc "Energy:", DAT_0072ddcc "Shields:"); the
// pool loads below cover them. The armor row's cached pstring (DAT_0072e3cc)
// is pool 0x0f "Armor:" — not 0x10, which is the boarding screen's
// "Armor Status:".
[[nodiscard]] std::string InfoString(std::uint16_t pool_index) {
  return NovaHud_LoadStringEntry(0x7d2,
                                 static_cast<std::uint16_t>(pool_index + 1U))
      .value_or(std::string{});
}

namespace {

// Shared three-state button captions come from STR# 0x96 (the table
// NovaUi_InitThreeStateButtonArt 0x004a2f50 fills, index n -> entry n+1). The
// Outfitter indices are DAT_007d82d0 = {4,1,2,0x12,0x13} (Done/Buy/Sell/^/&);
// the Shipyard's are DAT_007d82c6 = {4, buy, 0x2f, 0x12, 0x13} with buy 3
// (Buy Ship) or 0xc (Hire Escort) and 0x2f = Info.

// DLOG 0x3ea/0x3ec are 765 pixels wide and their frame PICTs are 321/323
// pixels high. The original centres those native-size windows on its 1024x768
// drawing surface. Store coordinates below are the corresponding DITL-local
// coordinates; keeping them local makes window resize centring and hit tests
// use the same transform.
struct StoreLayout {
  SDL_FPoint origin{};
  SDL_FRect frame{};
  SDL_FRect grid{};
  SDL_FRect description{};
  SDL_FRect preview{};
  SDL_FRect details{};
  SDL_FRect leave{};
  SDL_FRect buy{};
  SDL_FRect sell_or_info{};
  SDL_FRect previous{};
  SDL_FRect next{};
};

[[nodiscard]] StoreLayout LayoutStore(bool outfit_store) {
  constexpr float kWidth = 765.0F;
  const float height = outfit_store ? 321.0F : 323.0F;
  const SDL_FPoint origin{0.0F, 0.0F};
  auto at = [origin](SDL_FRect rect) { return OffsetRect(rect, origin); };

  StoreLayout layout;
  layout.origin = origin;
  layout.frame = at({0.0F, 0.0F, kWidth, height});
  // One-based DITL entries 5, 6, 8 and 9.
  layout.grid = at({9.0F, 8.0F, 333.0F, 271.0F});
  layout.description = at({354.0F, 10.0F, 192.0F, 267.0F});
  layout.preview = at({557.0F, 8.0F, 200.0F, 200.0F});
  layout.details = at({outfit_store ? 618.0F : 614.0F,
                       214.0F,
                       outfit_store ? 135.0F : 143.0F,
                       100.0F});
  if (outfit_store) {
    // DITL entries 1, 7, 4, 10 and 11: Leave, Buy, Sell, Previous, Next.
    layout.leave = at({500.0F, 289.0F, 99.0F, 25.0F});
    layout.buy = at({288.0F, 289.0F, 99.0F, 25.0F});
    layout.sell_or_info = at({394.0F, 289.0F, 99.0F, 25.0F});
    layout.previous = at({148.0F, 288.0F, 25.0F, 25.0F});
    layout.next = at({178.0F, 288.0F, 25.0F, 25.0F});
  } else {
    // DITL entries 7, 1, 10, 12 and 13: Leave, Buy, Info, Previous, Next.
    layout.leave = at({480.0F, 289.0F, 109.0F, 25.0F});
    layout.buy = at({365.0F, 289.0F, 109.0F, 25.0F});
    layout.sell_or_info = at({253.0F, 289.0F, 89.0F, 25.0F});
    layout.previous = at({141.0F, 288.0F, 25.0F, 25.0F});
    layout.next = at({171.0F, 288.0F, 25.0F, 25.0F});
  }
  return layout;
}

[[nodiscard]] SDL_FRect StoreCell(const StoreLayout &layout, std::size_t slot) {
  // thunk_FUN_008745b6 (0x00499150): four columns, five rows. The original
  // generated inclusive 84x55 rectangles at 83x54 steps from DITL item 5.
  constexpr float kStepX = 83.0F;
  constexpr float kStepY = 54.0F;
  return {layout.grid.x + static_cast<float>(slot % 4) * kStepX,
          layout.grid.y + static_cast<float>(slot / 4) * kStepY,
          84.0F,
          55.0F};
}

struct StoreLabelLines {
  std::string_view first;
  std::string_view second;
};

// NovaText_SplitPascalStringAtNewline is used by both original store redraw
// routines. Scenario short names encode the separator as the two characters
// "\\n" (not an embedded LF), for example "Light Blaster\\nTurret".
[[nodiscard]] StoreLabelLines SplitStoreLabel(std::string_view label) {
  std::size_t separator = label.find("\\n");
  std::size_t separator_width = 2;
  if (separator == std::string_view::npos) {
    separator = label.find('\n');
    separator_width = 1;
  }
  if (separator == std::string_view::npos) {
    return {label, {}};
  }
  return {label.substr(0, separator),
          label.substr(separator + separator_width)};
}

void DrawStoreBase(SdlPlatform &platform,
                   const std::function<void()> &render_background,
                   SDL_Texture *backdrop,
                   SDL_Texture *frame,
                   const StoreLayout &layout) {
  SDL_Renderer *renderer = platform.renderer();
  if (render_background) {
    // Re-render the preserved docked menu and layer the store window on top
    // (deliberate divergence, see docs/dlog_ditl_dialog_format.md).
    render_background();
  } else {
    platform.SetPlacement(PlaceWindow(platform.logical_playfield_size()));
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);
    if (backdrop != nullptr) {
      DrawContainedPict(platform, backdrop);
    }
  }
  // Ghidra's NovaUi_RedrawOutfitterMenu (0x00490c70) and the analogous
  // shipyard redraw fill and draw their modal window surface, then composite
  // it over the existing travel scene. There is no full-screen dim/scrim.
  platform.SetPlacement(PlaceContained({layout.frame.w, layout.frame.h},
                                       platform.logical_playfield_size()));
  if (frame != nullptr) {
    SDL_RenderTexture(renderer, frame, nullptr, &layout.frame);
  }
}

struct StoreTextureCache {
  std::unordered_map<std::int16_t, std::unique_ptr<SdlTexture>> pictures;
  std::unordered_map<std::int16_t, std::unique_ptr<SpriteAsset>> ship_sprites;
};

// @port 0x00497b70 100%
// Ghidra 0x00497b70 NovaUi_BlitPictThumbnailCached: per-modal thumbnail cache.
// The original's shared 128-entry atlas LRU and three-load-per-redraw cadence
// are not reproduced.

[[nodiscard]] SDL_Texture *StorePreviewTexture(SdlPlatform &platform,
                                               const GameState &state,
                                               StoreTextureCache &cache,
                                               bool outfit_store,
                                               std::int16_t id) {
  if (id < 0x80) {
    return nullptr;
  }
  const auto found = cache.pictures.find(id);
  if (found != cache.pictures.end()) {
    if (found->second) {
      return found->second->get();
    }
    const auto sprite = cache.ship_sprites.find(id);
    return sprite != cache.ship_sprites.end() && sprite->second &&
                   !sprite->second->frames.empty()
               ? sprite->second->frames.front().texture->get()
               : nullptr;
  }
  // Ship thumbnails use the class's resolved portrait PICT
  // (ShipClassDef +0xa0a portrait_pict_resource_id, the id
  // NovaUi_DrawShipyardShipList 0x004948b0 passes to
  // NovaUi_BlitPictThumbnailCached 0x00497b70): PICT 5000+class when it
  // exists, else the base-sprite owner's portrait (0x004aeda0). Computing
  // 5000+id here instead showed the wrong ship for clone classes (e.g. the
  // Used Heavy Shuttle).
  const ShipClass *ship_class =
      outfit_store ? nullptr : state.scenario.Ship(id);
  const std::uint16_t portrait_pict =
      ship_class != nullptr ? ship_class->portrait_pict_resource_id : 0;
  const std::int32_t pict_id = outfit_store
                                   ? static_cast<std::int32_t>(id - 0x80) + 6000
                                   : static_cast<std::int32_t>(portrait_pict);
  auto picture =
      pict_id > 0
          ? LoadPictTexture(platform, static_cast<std::uint16_t>(pict_id))
          : nullptr;
  if (picture) {
    SDL_Texture *result = picture->get();
    cache.pictures.emplace(id, std::move(picture));
    return result;
  }
  cache.pictures.emplace(id, nullptr);
  if (outfit_store) {
    return nullptr;
  }

  // ShipClass visual fallback for classes with no resolvable portrait: the
  // port's extra safety net (the original leaves the atlas cell black,
  // NovaUi_BlitPictThumbnailCached 0x00497b70).
  // Deliberate divergence (docs/dlog_ditl_dialog_format.md section 7.2).
  const auto visual_data = NovaResource_Load(kShipVisualResourceType,
                                             static_cast<std::uint16_t>(id));
  if (!visual_data) {
    cache.ship_sprites.emplace(id, nullptr);
    return nullptr;
  }
  const auto visual = DecodeShipVisualDescriptor(*visual_data);
  if (!visual) {
    cache.ship_sprites.emplace(id, nullptr);
    return nullptr;
  }
  auto sprite =
      SpriteAsset::LoadSheet(platform.renderer(), visual->base_image_id);
  SDL_Texture *result = sprite && !sprite->frames.empty()
                            ? sprite->frames.front().texture->get()
                            : nullptr;
  cache.ship_sprites.emplace(id, std::move(sprite));
  return result;
}

// @port 0x004948b0 100%
// @port 0x00490c70 100%
// Ghidra 0x004948b0 NovaUi_DrawShipyardShipList. Exact cache cadence and
// scripted colour variants are rendering mechanics intentionally not
// reproduced; the SDL texture cache (StoreTextureCache/StorePreviewTexture,
// 0x00497b70) owns the replacement.
void DrawStoreContents(SdlPlatform &platform,
                       NovaFontCache &font_cache,
                       const ServicesButtonArt &button_art,
                       GameState &state,
                       const LandedStoreSession &session,
                       std::int16_t stellar_id,
                       const StoreLayout &layout,
                       StoreTextureCache &texture_cache,
                       SDL_Texture *selected_image,
                       std::string_view selected_description,
                       NovaRgbColor grid_dim,
                       NovaRgbColor grid_bright) {
  constexpr SDL_Color kText{255, 255, 255, 255};
  constexpr SDL_Color kMuted{192, 192, 192, 255};
  // 20000,20000,20000 (16-bit) -- the grey tinted over two-line grid names
  // whose line starts with a non-alphanumeric char (DAT_00733b62).
  constexpr SDL_Color kUnlicensedTint{78, 78, 78, 255};
  const bool outfit_store = session.kind == LandedStoreKind::kOutfitter;
  SDL_Renderer *renderer = platform.renderer();
  SDL_FRect selected_rect{};
  bool have_selected = false;
  for (std::size_t slot = 0; slot < LandedStoreSession::kPageSlots; ++slot) {
    const std::size_t index = session.page_base + slot;
    if (index >= session.available_ids.size())
      break;
    const std::int16_t id = session.available_ids[index];
    const SDL_FRect rect = StoreCell(layout, slot);
    const bool selected = id == session.selected_id;
    // Every page cell gets the dim c\x9alr grid frame; the selected square is
    // re-framed in the bright color after the loop (Ghidra 0x00490c70 /
    // 0x004948b0 frame it once, on top of the whole page). The 84x55 cells
    // step by 83x54, so drawing it inline would let the next cell's border
    // clip its right/bottom edge.
    SDL_SetRenderDrawColor(renderer,
                           grid_dim.red,
                           grid_dim.green,
                           grid_dim.blue,
                           SDL_ALPHA_OPAQUE);
    SDL_RenderRect(renderer, &rect);
    if (selected) {
      selected_rect = rect;
      have_selected = true;
    }
    if (SDL_Texture *thumbnail = StorePreviewTexture(
            platform, state, texture_cache, outfit_store, id)) {
      const SDL_FRect thumbnail_rect{
          rect.x + (rect.w - 32.0F) / 2.0F, rect.y + 3.0F, 32.0F, 32.0F};
      SDL_RenderTexture(renderer, thumbnail, nullptr, &thumbnail_rect);
    }
    const std::string_view name =
        outfit_store ? std::string_view{state.scenario.Outfit(id)->short_name}
                     : std::string_view{state.scenario.Ship(id)->short_name};
    const StoreLabelLines label = SplitStoreLabel(name);
    // NovaUi_RedrawOutfitterMenu (0x00490c70) and
    // NovaUi_DrawShipyardShipList (0x004948b0) place a single line at
    // bottom-6, or split labels at bottom-14 and bottom-3.
    const float first_baseline =
        rect.y + (label.second.empty() ? 49.0F : 41.0F);
    // Grid labels use the shared 9-pt Geneva screen font (DAT_00735684 /
    // DAT_00735686 = "Geneva", 9), same as the detail rows and description.
    // In the two-line arm the original tints a line grey (DAT_00733b62) when
    // its first character is neither alphabetic nor a digit -- MSL ctype bits
    // 0x0001/0x0008 (0x00490c70). This is what dims the "- used -" /
    // "- illegal -" qualifiers. Single-line names stay white.
    const auto line_color = [&](std::string_view line) {
      const auto first =
          line.empty() ? 0U : static_cast<unsigned char>(line.front());
      return std::isalnum(first) != 0 ? kText : kUnlicensedTint;
    };
    const bool two_line = !label.second.empty();
    NovaText_DrawCentered(platform,
                          font_cache,
                          NovaFontFamily::kGeneva,
                          9.0F,
                          kNovaFontStyleRegular,
                          two_line ? line_color(label.first) : kText,
                          rect.x + 2.0F,
                          rect.x + rect.w - 2.0F,
                          first_baseline,
                          label.first);
    if (two_line) {
      NovaText_DrawCentered(platform,
                            font_cache,
                            NovaFontFamily::kGeneva,
                            9.0F,
                            kNovaFontStyleRegular,
                            line_color(label.second),
                            rect.x + 2.0F,
                            rect.x + rect.w - 2.0F,
                            rect.y + 52.0F,
                            label.second);
    }
    if (outfit_store) {
      // NovaUi_RedrawOutfitterMenu (0x00490c70): the owned count is drawn only
      // when positive, right-aligned to rect.right - (width + 3) at
      // rect.y + 0xc, in PTR_DAT_00575ad8 (white, DAT_00575ad0).
      const std::int16_t count = state.inventory.outfit_owned_count[id - 0x80];
      if (count > 0) {
        const std::string count_text = std::to_string(count);
        const int count_width = font_cache.TextWidth(
            NovaFontFamily::kGeneva, 9.0F, kNovaFontStyleRegular, count_text);
        NovaText_Draw(platform,
                      font_cache,
                      NovaFontFamily::kGeneva,
                      9.0F,
                      kNovaFontStyleRegular,
                      kText,
                      rect.x + rect.w - static_cast<float>(count_width) - 3.0F,
                      rect.y + 12.0F,
                      count_text);
      }
    }
  }
  if (have_selected) {
    SDL_SetRenderDrawColor(renderer,
                           grid_bright.red,
                           grid_bright.green,
                           grid_bright.blue,
                           SDL_ALPHA_OPAQUE);
    SDL_RenderRect(renderer, &selected_rect);
  }
  // 0x0048ea70: the outfitter's Buy button is also gated by DAT_007d4c0d, a
  // stellar-wide "sells outfits at all" flag (a TechLevel of 0 with no
  // positive SpecialTech disables it regardless of the selection).
  const bool store_offers_outfits =
      !outfit_store || NovaLanded_StellarSellsOutfits(state, stellar_id);
  const bool buy_allowed =
      session.selected_id >= 0 && store_offers_outfits &&
      (outfit_store
           ? NovaLanded_CanBuyOutfit(state, stellar_id, session.selected_id)
       : session.hire_mode
           ? NovaLanded_CanHireShip(state, stellar_id, session.selected_id)
           : NovaLanded_CanBuyShip(state, stellar_id, session.selected_id));
  const bool sell_allowed =
      outfit_store && session.selected_id >= 0 &&
      state.inventory.outfit_owned_count[session.selected_id - 0x80] > 0 &&
      (state.scenario.Outfit(session.selected_id)->flags & 0x0008U) == 0U;
  // Captions are the original STR# 0x96 entries, not hardcoded all-caps text.
  const std::uint16_t buy_label_entry =
      outfit_store ? 2 : (session.hire_mode ? 13 : 4);
  const std::array<std::tuple<SDL_FRect, std::string, bool>, 5> controls{
      {{layout.leave, LoadButtonLabel(5, "Done"), true},
       {layout.buy,
        LoadButtonLabel(buy_label_entry,
                        outfit_store
                            ? "Buy"
                            : (session.hire_mode ? "Hire Escort" : "Buy Ship")),
        buy_allowed},
       {layout.sell_or_info,
        LoadButtonLabel(outfit_store ? 3 : 48, outfit_store ? "Sell" : "Info"),
        outfit_store ? sell_allowed : session.selected_id >= 0},
       {layout.previous, LoadButtonLabel(19, "^"), session.CanPagePrevious()},
       {layout.next, LoadButtonLabel(20, "&"), session.CanPageNext()}}};
  for (const auto &[rect, label, enabled] : controls) {
    button_art.Draw(platform,
                    rect,
                    enabled ? ButtonState::kNormal : ButtonState::kDisabled);
    // Three-state button labels: white enabled, 50% grey disabled, in the
    // plain (non-bold) screen font -- the original's shared renderer
    // (NovaUi_DrawThreeStateButton, label colours DAT_007d8350).
    const SDL_Color kLabelGrey{128, 128, 128, 255};
    DrawThreeStateButtonLabel(
        platform, font_cache, rect, label, enabled ? kText : kLabelGrey);
  }
  if (session.selected_id >= 0) {
    const std::int32_t price =
        outfit_store
            ? NovaLanded_OutfitPrice(state, stellar_id, session.selected_id)
            : 0; // Ship price rows render in the details-panel block below.
    // The details block's "Ship Price" row is the selected ship's scaled
    // list price; the "Final Price" row subtracts the trade-in credit below.
    const std::int32_t ship_price =
        outfit_store
            ? 0
            : NovaLanded_ShipPrice(state, stellar_id, session.selected_id);
    if (selected_image != nullptr) {
      SDL_RenderTexture(renderer, selected_image, nullptr, &layout.preview);
    } else {
      // Empty preview frame placeholder ('No Picture' / 'Available'), entry 8
      // of both store redraws (0x004948b0 / 0x00490c70).
      const float mid = layout.preview.y + layout.preview.h / 2.0F;
      NovaText_DrawCentered(platform,
                            font_cache,
                            NovaFontFamily::kGeneva,
                            9.0F,
                            kNovaFontStyleRegular,
                            kMuted,
                            layout.preview.x,
                            layout.preview.x + layout.preview.w,
                            mid - 6.0F,
                            InfoString(0xd4));
      NovaText_DrawCentered(platform,
                            font_cache,
                            NovaFontFamily::kGeneva,
                            9.0F,
                            kNovaFontStyleRegular,
                            kMuted,
                            layout.preview.x,
                            layout.preview.x + layout.preview.w,
                            mid + 6.0F,
                            InfoString(0xd5));
    }
    // DITL entry 6 (both store redraws) is the desc alone: left-aligned at the
    // panel edge, word-wrapped to its full width, drawn white (the original
    // fills + InvertRects the panel; the frame already supplies the black).
    const auto draw_description = [&]() {
      constexpr float kSize = 9.0F;
      const auto lines = WrapDescriptionLines(
          selected_description,
          static_cast<int>(std::max(1.0F, layout.description.w)),
          [&](std::string_view line) {
            return font_cache.TextWidth(
                NovaFontFamily::kGeneva, kSize, kNovaFontStyleRegular, line);
          });
      const float pitch = static_cast<float>(
          font_cache.LineHeight(NovaFontFamily::kGeneva, kSize));
      float y = layout.description.y + kSize;
      for (const std::string &line : lines) {
        if (y > layout.description.y + layout.description.h) {
          break;
        }
        NovaText_Draw(platform,
                      font_cache,
                      NovaFontFamily::kGeneva,
                      kSize,
                      kNovaFontStyleRegular,
                      kText,
                      layout.description.x,
                      y,
                      line);
        y += pitch;
      }
    };
    if (outfit_store) {
      const Outfit *outfit = state.scenario.Outfit(session.selected_id);
      const ShipClass *player_ship = state.scenario.Ship(
          static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
      const std::int32_t item_mass =
          player_ship == nullptr ? 0
                                 : outfit->PurchaseMass(player_ship->mass_tons);
      const std::int32_t free_mass = Outfit_ComputePlayerFreeMass(state);
      // Ghidra NovaUi_RedrawOutfitterMenu (0x00490c70) detail panel (DITL
      // entry 9): labels at the panel's left edge and values at +0x46 (70px),
      // all in PTR_DAT_00575ad8 (white). Item Price sits at +0xc and You Have
      // at +0x18; Item Mass (+0x30) and Available (+0x3c) appear only when the
      // item carries positive mass; the status line is at +0x5d.
      const auto detail_row =
          [&](float dy, std::uint16_t label, std::string value) {
            NovaText_Draw(platform,
                          font_cache,
                          NovaFontFamily::kGeneva,
                          9.0F,
                          kNovaFontStyleRegular,
                          kText,
                          layout.details.x,
                          layout.details.y + dy,
                          InfoString(label));
            NovaText_Draw(platform,
                          font_cache,
                          NovaFontFamily::kGeneva,
                          9.0F,
                          kNovaFontStyleRegular,
                          kText,
                          layout.details.x + 70.0F,
                          layout.details.y + dy,
                          value);
          };
      detail_row(12.0F, 0xd6, GroupThousands(price) + " " + InfoString(0x21));
      detail_row(24.0F,
                 0xd8,
                 GroupThousands(state.player.credits) + " " + InfoString(0x21));
      if (outfit->mass_tons > 0) {
        detail_row(48.0F,
                   0xd7,
                   GroupThousands(item_mass) + " " +
                       (item_mass == 1 ? InfoString(0) : InfoString(1)));
        const std::int32_t available = std::max(0, free_mass);
        detail_row(60.0F,
                   0xd9,
                   GroupThousands(available) + " " +
                       (available == 1 ? InfoString(0) : InfoString(1)));
      }
      // Outfit_ClampOutfitOwnedCountToCurrentLimits (0x004656a0) returning an
      // at-cap count picks the ownership messages (0xdb owned / 0xdc none); an
      // item that still has room but will not fit the remaining free mass
      // picks the hold messages (0xdd owned / 0xde none).
      const std::int16_t owned =
          state.inventory.outfit_owned_count[session.selected_id - 0x80];
      const OutfitOwnership ownership = Outfit_ClampOwnedCountToLimits(
          state, static_cast<std::int16_t>(session.selected_id - 0x80));
      std::string status;
      if (ownership.limited) {
        status = InfoString(owned < 1 ? 0xdb : 0xda);
      } else if (item_mass > 0 && free_mass < item_mass) {
        status = InfoString(owned < 1 ? 0xdd : 0xdc);
      }
      if (!status.empty()) {
        NovaText_Draw(platform,
                      font_cache,
                      NovaFontFamily::kGeneva,
                      9.0F,
                      kNovaFontStyleRegular,
                      kText,
                      layout.details.x,
                      layout.details.y + 93.0F,
                      status);
      }
      draw_description();
    } else {
      // Ghidra NovaUi_DrawShipyardShipList 0x004948b0: the right-hand panels
      // show the desc text (DITL entry 6) and the Ship Price / Trade-In /
      // Final Price / You Have block (entry 9, values left-aligned at +0x46);
      // the full stat block lives in the Info sub-modal (0x00495c80).
      const std::int32_t trade_in =
          session.hire_mode ? 0
                            : NovaLanded_ShipTradeInValue(state, stellar_id);
      const std::int16_t player_class =
          static_cast<std::int16_t>(state.player.ship_class_id + 0x80);
      // Ghidra NovaUi_DrawShipyardShipList (0x004948b0) entry 9: label and
      // value share the small system font (DAT_00735684) and PTR_DAT_00575ad8
      // (white); labels sit at the panel's left edge and values at +0x46
      // (70px), matching the outfitter's entry 9 exactly.
      const auto price_row =
          [&](float dy, std::uint16_t label, std::int32_t value) {
            NovaText_Draw(platform,
                          font_cache,
                          NovaFontFamily::kGeneva,
                          9.0F,
                          kNovaFontStyleRegular,
                          kText,
                          layout.details.x,
                          layout.details.y + dy,
                          InfoString(label));
            NovaText_Draw(platform,
                          font_cache,
                          NovaFontFamily::kGeneva,
                          9.0F,
                          kNovaFontStyleRegular,
                          kText,
                          layout.details.x + 70.0F,
                          layout.details.y + dy,
                          GroupThousands(value) + " " + InfoString(0x21));
          };
      if (session.hire_mode) {
        // Ghidra NovaUi_DrawShipyardShipList 0x004948b0 hire-mode arm: the
        // price block shows the Hire Escort line (10% of the scaled purchase
        // price, DAT_00575950) instead of the trade-in ladder.
        price_row(
            12.0F,
            0xe3,
            NovaLanded_ShipHirePrice(state, stellar_id, session.selected_id));
        price_row(36.0F, 0xd8, state.player.credits); // You Have:
      } else if (session.selected_id != player_class) {
        price_row(12.0F, 0xe0, ship_price); // Ship Price:
        price_row(24.0F, 0xe1, trade_in);   // Trade-In:
        price_row(
            48.0F,
            0xe2,
            std::max<std::int32_t>(0, ship_price - trade_in)); // Final Price:
        price_row(72.0F, 0xd8, state.player.credits);          // You Have:
      }
      draw_description();
    }
  }
}

// ---------------------------------------------------------------------------
struct ShipyardInfoLayout {
  SDL_FRect window{};
  SDL_FRect button{};  // entry 1: "Done" (STR# 0x96 entry 5)
  SDL_FRect title{};   // entry 3: ship display name
  SDL_FRect stats{};   // entry 5: two-column stat block
  SDL_FRect picture{}; // entry 7: desc Graphic PICT (custom variant only)
  SDL_FRect weapons{}; // entry 8: stock weapons block (custom variant only)
  bool custom_picture = false;
};

[[nodiscard]] ShipyardInfoLayout LayoutShipyardInfo(bool custom_picture) {
  const float width = custom_picture ? 614.0F : 250.0F;
  const float height = custom_picture ? 537.0F : 285.0F;
  const SDL_FPoint origin{0.0F, 0.0F};
  auto at = [origin](SDL_FRect rect) { return OffsetRect(rect, origin); };

  ShipyardInfoLayout layout;
  layout.custom_picture = custom_picture;
  layout.window = at({0.0F, 0.0F, width, height});
  if (custom_picture) {
    layout.button = at({503.0F, 507.0F, 99.0F, 25.0F});
    layout.title = at({7.0F, 413.0F, 600.0F, 24.0F});
    layout.stats = at({11.0F, 442.0F, 256.0F, 92.0F});
    layout.picture = at({7.0F, 6.0F, 600.0F, 400.0F});
    layout.weapons = at({266.0F, 442.0F, 342.0F, 59.0F});
  } else {
    layout.button = at({86.0F, 253.0F, 74.0F, 25.0F});
    layout.title = at({3.0F, 3.0F, 240.0F, 24.0F});
    layout.stats = at({9.0F, 32.0F, 234.0F, 214.0F});
  }
  return layout;
}

// Label/unit text for the ship-info panel, from STR# 0x7d2 (see InfoString
// above for the 0-based pool index convention).

// "Accel:" rating word. The loader stores base_accel as payload/10000 (the
// 10000.0 divisor is the double at 0x575e68); the panel compares that against
// the descending double thresholds at 0x575960..0x575988 (FCOM chain).
[[nodiscard]] std::string AccelRating(float accel_payload) {
  const double rating = accel_payload / 10000.0;
  if (rating > 0.073) {
    return InfoString(0xf4); // Excellent
  }
  if (rating > 0.055) {
    return InfoString(0xf5); // Very Good
  }
  if (rating > 0.038) {
    return InfoString(0xf6); // Good
  }
  if (rating > 0.025) {
    return InfoString(0xf7); // Average
  }
  if (rating > 0.013) {
    return InfoString(0xf8); // Poor
  }
  if (rating <= 0.0) {
    return InfoString(0x18b); // N/A
  }
  return InfoString(0xf9); // Terrible
}

// "Turn:" rating word. base_turn_rate_deg = payload * 0.1
// (k_guided_turn_scale_f64 at 0x575e58), compared against the float
// thresholds 0..5 at 0x575990..0x5759a4.
[[nodiscard]] std::string TurnRating(float turn_payload) {
  const float rating = turn_payload * 0.1F;
  if (rating > 5.0F) {
    return InfoString(0xf4);
  }
  if (rating > 4.0F) {
    return InfoString(0xf5);
  }
  if (rating > 3.0F) {
    return InfoString(0xf6);
  }
  if (rating > 2.0F) {
    return InfoString(0xf7);
  }
  if (rating > 1.0F) {
    return InfoString(0xf8);
  }
  if (rating > 0.0F) {
    return InfoString(0xf9);
  }
  return InfoString(0x18b);
}

// The "Standard Weapons" lines (loop inline in the detail panel, walking the
// 0x100 weapon slots; the loader seeds slot i from each mounted stock bank as
// default_weapon_ammo[i] = count and default_weapon_secondary[i] = ammo_load,
// both indexed by the zero-based weapon id). Guns (ModType 1) print
// "<count> <name>" with a plural "s" and a " + <ammo_load> ammo" tail when the
// bank loads rounds; mode-99 banks (carried ships) print the bank's ammo_load
// against the ModType-3 fighter-bay outfit instead. Verified against shïp
// 0x08f Fed Carrier (bay bank 149/1/4) and 0x080 Shuttle (gun 128/1/-1).
[[nodiscard]] std::vector<std::string> StockWeaponLines(const GameState &state,
                                                        const ShipClass &ship) {
  std::vector<std::string> lines;
  auto outfit_for_slot = [&](std::int16_t mod_type,
                             std::size_t slot) -> const Outfit * {
    for (const Outfit &outfit : state.scenario.outfits) {
      if (outfit.mod_type == mod_type &&
          outfit.mod_val == static_cast<std::int16_t>(slot) &&
          outfit.tech_level < 0x7fff) {
        return &outfit;
      }
    }
    return nullptr;
  };
  for (std::size_t slot = 0; slot < 0x100; ++slot) {
    const ShipDefaultWeaponBank *bank = nullptr;
    for (const ShipDefaultWeaponBank &candidate : ship.stock_weapons) {
      if (candidate.weapon_id >= 0x80 && candidate.weapon_id < 0x180 &&
          static_cast<std::size_t>(candidate.weapon_id - 0x80) == slot) {
        bank = &candidate;
        break;
      }
    }
    if (bank == nullptr || bank->count <= 0) {
      continue;
    }
    const Weapon *weapon =
        state.scenario.Weapon(static_cast<std::int16_t>(slot + 0x80));
    const bool is_bay = weapon != nullptr && weapon->weapon_mode_code == 99;
    const Outfit *outfit = outfit_for_slot(is_bay ? 3 : 1, slot);
    // The original still draws the bare count when the outfit lookup fails;
    // a nameless "2" line is noise, so skip instead.
    if (outfit == nullptr) {
      continue;
    }
    const std::int16_t count = is_bay ? bank->ammo_load : bank->count;
    std::string line = std::to_string(count) + " " + outfit->name;
    if (count > 1) {
      line += "s";
    }
    if (!is_bay && bank->ammo_load > 0) {
      line += " + " + std::to_string(bank->ammo_load) + " " + InfoString(0xf2);
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

// "Space:" value in the Shipyard Info panel. The original (0x00495c80) starts
// from the loader-folded ShipClassDef.free_mass and re-subtracts the default
// weapon/ammo/DefaultItem masses. Its ammo subtraction is keyed on the weapon
// bank index, while the loader's matching addition (0x004bd3c0) is keyed on
// the mounted weapon's ammo_or_energy_cost_code, so a weapon that shares
// another weapon's ammo outfit leaves its loaded rounds in the total
// (FreeMass + loaded ammo). BUGFIX(original): show the advertised payload
// FreeMass instead. The faithful (buggy) recompute is kept for
// kApplyOriginalBugFixes == false.
[[nodiscard]] std::int32_t ShipyardDisplayFreeMass(const GameState &state,
                                                   const ShipClass &ship) {
  if (kApplyOriginalBugFixes) {
    return std::max(0, static_cast<std::int32_t>(ship.advertised_free_mass));
  }
  std::int32_t free_mass = ship.free_mass;
  const auto subtract = [&free_mass](std::int32_t mass) {
    if (mass <= free_mass) {
      free_mass -= mass;
    } else {
      free_mass = 0;
    }
  };
  for (const ShipDefaultWeaponBank &stock : ship.stock_weapons) {
    if (stock.weapon_id < 0x80 || stock.weapon_id >= 0x180) {
      continue;
    }
    const std::int16_t bank = static_cast<std::int16_t>(stock.weapon_id - 0x80);
    if (stock.count > 0) {
      for (const Outfit &outfit : state.scenario.outfits) {
        if (outfit.mod_type == 1 && outfit.mod_val == bank &&
            outfit.tech_level < 0x7fff) {
          subtract(outfit.PurchaseMass(ship.mass_tons) * stock.count);
          break;
        }
      }
    }
    if (stock.ammo_load > 0) {
      // Original display keys the ammo subtraction on the bank, not on the
      // mounted weapon's ammo code -- the shared-ammo source of the bug.
      for (const Outfit &outfit : state.scenario.outfits) {
        if (outfit.mod_type == 3 && outfit.mod_val == bank &&
            outfit.tech_level < 0x7fff) {
          subtract(outfit.PurchaseMass(ship.mass_tons) * stock.ammo_load);
          break;
        }
      }
    }
  }
  for (std::size_t i = 0; i < ship.default_outfit_ids.size(); ++i) {
    const std::int16_t id = ship.default_outfit_ids[i];
    const std::int16_t count = ship.default_outfit_counts[i];
    if (id < 0x80 || count <= 0) {
      continue;
    }
    const Outfit *outfit = state.scenario.Outfit(id);
    if (outfit == nullptr || outfit->tech_level >= 0x7fff) {
      continue;
    }
    subtract(outfit->PurchaseMass(ship.mass_tons) * count);
  }
  return std::max(0, free_mass);
}

// @port 0x00495c80 100% bugfix
void DrawShipyardInfoPanel(SdlPlatform &platform,
                           NovaFontCache &font_cache,
                           const ServicesButtonArt &button_art,
                           const GameState &state,
                           const LandedStoreSession &session,
                           const ShipyardInfoLayout &layout,
                           SDL_Texture *backdrop,
                           SDL_Texture *custom_picture) {
  const ShipClass *ship = state.scenario.Ship(session.selected_id);
  if (ship == nullptr) {
    return;
  }
  SDL_Renderer *renderer = platform.renderer();
  // Shared dialog palette: black fill, white values/title, grey labels (same
  // colours the boarding window uses for PTR_DAT_00575ad8 / DAT_00733b56).
  constexpr SDL_Color kBg{0, 0, 0, 255};
  constexpr SDL_Color kValue{255, 255, 255, 255};
  constexpr SDL_Color kLabel{128, 128, 128, 255};
  constexpr NovaFontFamily kFont = NovaFontFamily::kGeneva;
  constexpr float kTextSize = 10.0F;
  // The title font is DAT_0085fb12 with scaled size 0x12; Geneva 18 matches
  // the window art's name strip.
  constexpr float kTitleSize = 18.0F;

  SDL_SetRenderDrawColor(renderer, kBg.r, kBg.g, kBg.b, SDL_ALPHA_OPAQUE);
  SDL_RenderFillRect(renderer, &layout.window);
  if (backdrop != nullptr) {
    SDL_RenderTexture(renderer, backdrop, nullptr, &layout.window);
  }

  // Title: the base variant draws the stripped record name (ShipClassDef+0x6c,
  // filled by the loader through NameString_StripSubtitleSuffix); the custom
  // variant draws the shïp +0x62e string (DAT_005a9bcc table) = long_name.
  const std::string &title =
      layout.custom_picture ? ship->long_name : ship->display_name;
  NovaText_DrawCentered(platform,
                        font_cache,
                        kFont,
                        kTitleSize,
                        kNovaFontStyleRegular,
                        kValue,
                        layout.title.x,
                        layout.title.x + layout.title.w,
                        layout.title.y + 18.0F,
                        title);

  SDL_RenderFillRect(renderer, &layout.stats);
  const float stats_left = layout.stats.x;
  const float stats_top = layout.stats.y;
  auto row = [&](float label_x,
                 float value_x,
                 float dy,
                 const std::string &label,
                 const std::string &value) {
    NovaText_Draw(platform,
                  font_cache,
                  kFont,
                  kTextSize,
                  kNovaFontStyleRegular,
                  kLabel,
                  label_x,
                  stats_top + dy,
                  label);
    NovaText_Draw(platform,
                  font_cache,
                  kFont,
                  kTextSize,
                  kNovaFontStyleRegular,
                  kValue,
                  value_x,
                  stats_top + dy,
                  value);
  };

  const std::string none = InfoString(0x14e); // "None"
  const std::string ton = InfoString(0x0);
  const std::string tons = InfoString(0x1);
  auto quantity =
      [](int count, const std::string &singular, const std::string &plural) {
        return std::to_string(count) + " " + (count == 1 ? singular : plural);
      };

  // Left column (label at +0, value at +0x2d; rows every 0xc from +0xc).
  // Speed: the original rounds base_speed (stored payload/100) times the
  // 100.0 multiplier at 0x575958; the port keeps the raw payload value.
  row(stats_left,
      stats_left + 45.0F,
      12.0F,
      InfoString(0xe4),
      std::to_string(std::lround(ship->speed)));
  row(stats_left,
      stats_left + 45.0F,
      24.0F,
      InfoString(0xe5),
      AccelRating(ship->accel));
  row(stats_left,
      stats_left + 45.0F,
      36.0F,
      InfoString(0xe6),
      TurnRating(ship->turn_rate));
  row(stats_left,
      stats_left + 45.0F,
      48.0F,
      InfoString(0x0a),
      ship->base_shield > 0 ? std::to_string(ship->base_shield) : none);
  row(stats_left,
      stats_left + 45.0F,
      60.0F,
      InfoString(0x0f),
      ship->base_armor > 0 ? std::to_string(ship->base_armor) : none);
  row(stats_left,
      stats_left + 45.0F,
      72.0F,
      InfoString(0xe7),
      ship->max_gun > 0 ? InfoString(0xed) + " " + std::to_string(ship->max_gun)
                        : none);
  row(stats_left,
      stats_left + 45.0F,
      84.0F,
      InfoString(0xe8),
      ship->max_turret > 0
          ? InfoString(0xed) + " " + std::to_string(ship->max_turret)
          : none);

  // Right column (label at +0x82, value at +0xaf). "Space:" is the shïp
  // advertised FreeMass; see ShipyardDisplayFreeMass for the original
  // (0x00495c80) recompute and the shared-ammo BUGFIX(original).
  constexpr float kRightLabelDx = 130.0F;
  constexpr float kRightValueDx = 175.0F;
  row(stats_left + kRightLabelDx,
      stats_left + kRightValueDx,
      12.0F,
      InfoString(0xe9),
      quantity(ShipyardDisplayFreeMass(state, *ship), ton, tons));
  row(stats_left + kRightLabelDx,
      stats_left + kRightValueDx,
      24.0F,
      InfoString(0x6d),
      quantity(ship->cargo_holds, ton, tons));
  const int jumps = ship->base_fuel / 100; // Bible: Fuel 100 = 1 jump.
  row(stats_left + kRightLabelDx,
      stats_left + kRightValueDx,
      36.0F,
      InfoString(0x06),
      jumps > 0 ? quantity(jumps, InfoString(0xee), InfoString(0xef)) : none);
  row(stats_left + kRightLabelDx,
      stats_left + kRightValueDx,
      48.0F,
      InfoString(0xea),
      std::to_string(ship->length_meters) + " " + InfoString(0xf3));
  row(stats_left + kRightLabelDx,
      stats_left + kRightValueDx,
      60.0F,
      InfoString(0xeb),
      quantity(ship->mass_tons, ton, tons));
  if (ship->crew > 0) {
    row(stats_left + kRightLabelDx,
        stats_left + kRightValueDx,
        72.0F,
        InfoString(0xec),
        std::to_string(ship->crew));
  }

  const std::vector<std::string> weapons = StockWeaponLines(state, *ship);
  const std::string weapons_label =
      weapons.empty() ? InfoString(0xf1) : InfoString(0xf0);
  if (!layout.custom_picture) {
    // Base variant: the block lives inside the stats panel, label at +0x6c
    // and lines 8px indented, 12px apart from +0x78.
    NovaText_Draw(platform,
                  font_cache,
                  kFont,
                  kTextSize,
                  kNovaFontStyleRegular,
                  kValue,
                  stats_left,
                  stats_top + 108.0F,
                  weapons_label);
    float line_y = stats_top + 120.0F;
    for (const std::string &line : weapons) {
      NovaText_Draw(platform,
                    font_cache,
                    kFont,
                    kTextSize,
                    kNovaFontStyleRegular,
                    kValue,
                    stats_left + 8.0F,
                    line_y,
                    line);
      line_y += 12.0F;
    }
  } else {
    // Custom variant: the label sits at the top of the entry-8 panel (top
    // + 0xc) and the weapon lines are joined with ", " into one block drawn
    // below it (top + 0x10; the original fills that block and inverts it —
    // the net white-on-black look is kept). The joined block is wrapped here;
    // the original lets DrawPascalStringInFilledRect clip it.
    NovaText_Draw(platform,
                  font_cache,
                  kFont,
                  kTextSize,
                  kNovaFontStyleRegular,
                  kValue,
                  layout.weapons.x,
                  layout.weapons.y + 12.0F,
                  weapons_label);
    if (!weapons.empty()) {
      std::string block;
      for (std::size_t i = 0; i < weapons.size(); ++i) {
        if (i > 0) {
          block += ", ";
        }
        block += weapons[i];
      }
      const auto lines =
          WrapDescriptionLines(block, 60, [](std::string_view text) {
            return static_cast<int>(text.size());
          });
      // Block top is + 0x10 (a filled-rect top in the original, not a text
      // baseline), so the first line starts one 12px line below it — otherwise
      // its ascenders collide with the heading above.
      float line_y = layout.weapons.y + 28.0F;
      for (std::size_t i = 0;
           i < lines.size() && line_y < layout.weapons.y + layout.weapons.h;
           ++i) {
        NovaText_Draw(platform,
                      font_cache,
                      kFont,
                      kTextSize,
                      kNovaFontStyleRegular,
                      kValue,
                      layout.weapons.x,
                      line_y,
                      lines[i]);
        line_y += 12.0F;
      }
    }
  }

  if (layout.custom_picture && custom_picture != nullptr) {
    SDL_RenderTexture(renderer, custom_picture, nullptr, &layout.picture);
  }

  button_art.Draw(platform, layout.button, ButtonState::kNormal);
  DrawThreeStateButtonLabel(
      platform, font_cache, layout.button, LoadButtonLabel(5, "Done"), kValue);
}

// One full frame of the store screen, shared by the store loop and the Info
// sub-modal: like the modern UiWindow_RunInteractionLoop render_background
// path, the sub-modal keeps re-rendering the store behind its window every
// frame instead of compositing over a stale snapshot.
void RenderStoreScreen(SdlPlatform &platform,
                       NovaFontCache &font_cache,
                       const ServicesButtonArt &button_art,
                       GameState &state,
                       const LandedStoreSession &session,
                       std::int16_t stellar_id,
                       StoreTextureCache &texture_cache,
                       const std::function<void()> &render_background,
                       SDL_Texture *dock_backdrop,
                       SDL_Texture *store_frame,
                       std::string_view selected_description,
                       NovaRgbColor grid_dim,
                       NovaRgbColor grid_bright) {
  const bool outfit_store = session.kind == LandedStoreKind::kOutfitter;
  platform.SetPlacement(PlaceContained({765.0F, outfit_store ? 321.0F : 323.0F},
                                       platform.logical_playfield_size()));
  const StoreLayout layout = LayoutStore(outfit_store);
  SDL_Texture *selected_image = StorePreviewTexture(
      platform, state, texture_cache, outfit_store, session.selected_id);
  DrawStoreBase(
      platform, render_background, dock_backdrop, store_frame, layout);
  DrawStoreContents(platform,
                    font_cache,
                    button_art,
                    state,
                    session,
                    stellar_id,
                    layout,
                    texture_cache,
                    selected_image,
                    selected_description,
                    grid_dim,
                    grid_bright);
}

// Ghidra 0x004956a0 NovaUi_RunShipyardDetailWindow: modal loop over the
// shipyard store. The window closes on the Done button (the original's only
// exit) or Escape. TODO(decomp(0x004956a0)) skipped: the original re-enters
// the starmap (action 4), player special interaction (action 2) and mission
// computer (action 6) windows from inside this loop; the store modal does not
// host those nested interactions yet, so the Info window only offers Done.
// @port 0x004956a0 90% ui
// Ghidra 0x004956a0 NovaUi_RunShipyardDetailWindow + 0x00495c80
// NovaUi_DrawShipyardDetailPanel: the Shipyard's Info sub-modal.
//
// The window is DLOG 0x3ed (DITL 0x3ed, backdrop PICT 0x213a "Ship
// Description", 250x285) unless the selected ship's desc resource (ship class
// id + 13000) carries a Graphic PICT that actually loads, in which case it is
// DLOG 0x3fb (backdrop PICT 0x213b "Ship description + pict", 614x537) and the
// Graphic is blitted into DITL entry 7 (the shipped descs point at the 600x400
// PICT 20128+ set, matching the entry rect exactly).
void RunShipyardInfoDialog(SdlPlatform &platform,
                           GameState &state,
                           const LandedStoreSession &session,
                           std::int16_t stellar_id,
                           const std::function<void()> &render_background,
                           SDL_Texture *dock_backdrop,
                           SDL_Texture *store_frame,
                           StoreTextureCache &texture_cache,
                           NovaFontCache &font_cache,
                           const ServicesButtonArt &button_art,
                           std::string_view selected_description,
                           NovaRgbColor grid_dim,
                           NovaRgbColor grid_bright) {
  const SdlPlatform::ScopedPlacement restore_placement(
      platform, platform.current_placement());
  std::unique_ptr<SdlTexture> custom_picture;
  bool custom = false;
  std::uint16_t backdrop_pict = 0x213a;
  if (session.selected_id >= 0x80) {
    // The desc key is the zero-based ship class + 13000 (purchase) or +14000
    // (hire) (the original's g_shipyard_selected_ship_class_id is the 0-based
    // defs index; the port's selected_id is the raw 0x80-based resource id).
    if (const auto desc = NovaResource_LoadDescription(
            static_cast<std::uint16_t>(session.selected_id - 0x80 +
                                       (session.hire_mode ? 14000 : 13000)));
        desc && desc->dialog_variant >= 0x80) {
      custom_picture = LoadPictTexture(platform, desc->dialog_variant);
      if (custom_picture) {
        custom = true;
        backdrop_pict = 0x213b;
      }
    }
  }
  auto backdrop = LoadPictTexture(platform, backdrop_pict);
  while (!platform.quit_requested()) {
    RenderStoreScreen(platform,
                      font_cache,
                      button_art,
                      state,
                      session,
                      stellar_id,
                      texture_cache,
                      render_background,
                      dock_backdrop,
                      store_frame,
                      selected_description,
                      grid_dim,
                      grid_bright);
    platform.SetPlacement(
        PlaceContained({custom ? 614.0F : 250.0F, custom ? 537.0F : 285.0F},
                       platform.logical_playfield_size()));
    const ShipyardInfoLayout layout = LayoutShipyardInfo(custom);
    const Placement detail_placement = platform.current_placement();
    const auto probe_rect = [&detail_placement](SDL_FRect rect) {
      return detail_placement.ToWindowRect(rect);
    };
    platform.PublishProbeUi("shipyard_info",
                            {{"window", probe_rect(layout.window)},
                             {"done", probe_rect(layout.button)}});
    DrawShipyardInfoPanel(platform,
                          font_cache,
                          button_art,
                          state,
                          session,
                          layout,
                          backdrop ? backdrop->get() : nullptr,
                          custom_picture ? custom_picture->get() : nullptr);
    platform.Present();
    bool done = false;
    for (std::optional<TextInput> input;
         (input = platform.PollTextEvent()) && !done;) {
      if (input->key == TextKey::escape) {
        done = true;
      } else if (input->key == TextKey::character) {
        const char key = static_cast<char>(
            std::tolower(static_cast<unsigned char>(input->character)));
        if (key == 'l') {
          done = true;
        }
      } else if (input->key == TextKey::primary) {
        if (Contains(layout.button, platform.mouse_position())) {
          done = true;
        }
      }
    }
    if (done) {
      return;
    }
    platform.PaceFrame();
  }
}

// Composes and shows the STR# 0x7d2 message for a blocked outfit sale
// (NovaUi_RunOutfitterInteractionLoop 0x0048ea70). `item_id` is the selected
// item's resource id; the blocker is named from the dependent/ammo outfit's
// LCName/LCPlural (falling back to the generic "unit(s) of ammunition" form
// for a carrier-bay weapon with no ammo outfit).
void ShowStoreSaleBlock(SdlPlatform &platform,
                        GameState &state,
                        std::int16_t item_id,
                        const OutfitSaleResult &sale,
                        const std::function<void()> &render_background) {
  if (sale.block == OutfitSaleBlock::kNone)
    return;
  std::string text;
  if (sale.block == OutfitSaleBlock::kNegativeMass) {
    text = InfoString(0xce); // entry 0xcf
  } else {
    // Ship_FormatLocalizedCountWord (0x00465d90): 1..10 load the number word,
    // everything else is decimal.
    const auto count_word = [](std::int16_t count) -> std::string {
      if (count < 1 || 10 < count)
        return std::to_string(count);
      if (const auto word = NovaHud_LoadStringEntry(
              0x89, static_cast<std::uint16_t>(count + 0x1c))) {
        return *word;
      }
      return std::to_string(count);
    };
    const Outfit *item = state.scenario.Outfit(item_id);
    const std::string item_name =
        item == nullptr ? std::string{}
                        : (sale.item_plural ? item->lc_plural : item->lc_name);
    std::string blocker_name;
    if (sale.blocker_id >= 0) {
      const Outfit *blocker = state.scenario.Outfit(sale.blocker_id);
      blocker_name =
          blocker == nullptr
              ? std::string{}
              : (sale.blocker_plural ? blocker->lc_plural : blocker->lc_name);
    } else {
      blocker_name = InfoString(sale.blocker_plural ? 0xd1 : 0xd0) + " " +
                     InfoString(0xd2);
    }
    text = InfoString(0xcf) + " " + count_word(sale.excess) + " " +
           blocker_name + " " + InfoString(0xd3) + " " + item_name + ".";
  }
  if (!text.empty()) {
    NovaUi_RunTextReaderDialog(platform, state, text, false, render_background);
  }
}

} // namespace

// @port 0x0049e8e0 100%
// Ghidra 0x0049e8e0 FUN_0049e8e0: the landed-store quantity prompt (DLOG
// 0x3eb). Entry 2 carries the "Enter quantity:" label (STR# 0x7d2 0x173) and
// entry 3 the editable amount, pre-filled with the maximum. Returns the
// entered amount clamped to [0, max], or 0 on cancel (ordinal 4). Invalid
// input resets the field and keeps the prompt open, matching the original.
std::int16_t
RunStoreQuantityPrompt(SdlPlatform &platform,
                       std::int16_t max_quantity,
                       const std::function<void()> &render_background) {
  auto loaded = UiWindow_CreateFromDialogResource(platform, 0x3eb);
  if (!loaded)
    return 0;
  UiDialogWindow window = std::move(*loaded);
  NovaFontCache font_cache;
  UiPanel_SetEntryTextPascal(window, 2, InfoString(0x172));
  UiPanel_SetEntryTextPascal(window, 3, std::to_string(max_quantity));
  UiPanel_SetTextEntrySelectionRange(window, 3, 0, 0xfe);
  ProbeUiAutoClear probe_ui_guard(platform);
  while (!platform.quit_requested()) {
    short code = -1;
    UiWindow_RunInteractionLoop(
        platform, font_cache, window, &code, render_background);
    if (code == 4)
      return 0;
    if (code != 1)
      continue;
    const std::string text = UiPanel_GetEntryTextPascal(window, 3);
    if (text.empty()) {
      UiPanel_SetEntryTextPascal(window, 3, "0");
      UiPanel_SetTextEntrySelectionRange(window, 3, 0, 0xfe);
      continue;
    }
    std::int32_t value = 0;
    bool numeric = true;
    for (const char c : text) {
      if (c < '0' || c > '9') {
        numeric = false;
        break;
      }
      value = value * 10 + (c - '0');
      if (value > 1000000) {
        numeric = false;
        break;
      }
    }
    if (!numeric)
      continue;
    if (value < 0 || value > max_quantity) {
      UiPanel_SetEntryTextPascal(window, 3, std::to_string(max_quantity));
      UiPanel_SetTextEntrySelectionRange(window, 3, 0, 0xfe);
      continue;
    }
    return static_cast<std::int16_t>(value);
  }
  return 0;
}

namespace {

// @port 0x00492f30 92% gameplay
// Ghidra 0x00492f30 -> 0x00497900: replacement purchases propose the new
// class short name plus a random three-digit suffix. The accepted edit is the
// christening/name-entry buffer copied onto the player ship.
[[nodiscard]] std::optional<std::string>
RunShipPurchaseConfirmation(SdlPlatform &platform,
                            GameState &state,
                            const ShipClass &ship,
                            const std::function<void()> &render_background) {
  auto loaded = UiWindow_CreateFromDialogResource(platform, 0xbb9);
  if (!loaded)
    return std::nullopt;
  UiDialogWindow window = std::move(*loaded);
  NovaFontCache font_cache;
  const std::string prompt = InfoString(0x78) + " " + ship.long_name + ":";
  std::string proposed = ship.long_name;
  proposed.push_back(' ');
  for (int i = 0; i < 3; ++i) {
    proposed +=
        std::to_string(std::uniform_int_distribution<int>{1, 9}(state.rng));
  }
  UiPanel_SetEntryTextPascal(window, 3, prompt);
  UiPanel_SetEntryTextPascal(window, 5, proposed);
  UiPanel_SetTextEntrySelectionRange(window, 5, 0, 0xfe);
  ProbeUiAutoClear probe_ui_guard(platform);
  while (!platform.quit_requested()) {
    short code = -1;
    UiWindow_RunInteractionLoop(
        platform, font_cache, window, &code, render_background);
    if (code == 6)
      return std::nullopt;
    if (code != 1)
      continue;
    std::string name = UiPanel_GetEntryTextPascal(window, 5);
    if (name.size() > 0x40) {
      UiPanel_SetTextEntrySelectionRange(window, 5, 0, 0x3f);
      continue;
    }
    constexpr std::string_view article = "the ";
    if (name.size() > article.size() &&
        std::equal(article.begin(),
                   article.end(),
                   name.begin(),
                   [](char expected, char actual) {
                     return expected ==
                            std::tolower(static_cast<unsigned char>(actual));
                   })) {
      name.erase(0, article.size());
    }
    return name;
  }
  return std::nullopt;
}

} // namespace

// @port 0x0048ea70 65% gameplay
// TODO(decomp): in-loop starmap/mission-computer re-entry
// actions, exact STR label resources, pressed-button tracking. Ghidra
// 0x0048ea70 NovaUi_RunOutfitterInteractionLoop and 0x00492f30
// NovaUi_RunShipyardPurchaseLoop: one generic store loop replaces both (service
// is a parameter). The 0x00493fc0 NovaUi_ShipyardHandleSelection Input /
// 0x0049f3f0 HitTestAndTrackShipyardActionButtons / 0x0049f6f0
// DrawShipyardActionButtons / 0x0049f8f0 HitTestAndTrackOutfitterActionButtons
// / 0x0049fbb0 DrawOutfitterActionButtons and the 0x00497b70
// BlitPictThumbnailCached cache all run inline within this function and the
// DrawStore* helpers below; the 0x004956a0/0x00495c80 detail window runs in
// RunShipyardInfoDialog above (opened by the Info action button).
LandedExit RunStoreDialog(SdlPlatform &platform,
                          SdlAudio &audio,
                          GameState &state,
                          LandedService service,
                          std::int16_t stellar_id,
                          const std::function<void()> &render_background,
                          bool hire_mode) {
  const SdlPlatform::ScopedPlacement restore_placement(
      platform, platform.current_placement());
  const bool outfit_store = service == LandedService::kOutfit;
  LandedStoreSession session =
      outfit_store
          ? NovaLanded_OpenOutfitterSession(state, stellar_id)
          : NovaLanded_OpenShipyardSession(state, stellar_id, hire_mode);
  // Ghidra 0x00492f30: an empty availability list pops the STR# 0x7d2
  // 0xdf/0xe0 notice (per mode) instead of opening the store window.
  if (!outfit_store && session.available_ids.empty()) {
    NovaUi_RunTextReaderDialog(platform,
                               state,
                               InfoString(hire_mode ? 0xe0 : 0xdf),
                               false,
                               render_background);
    return LandedExit::kServiceComplete;
  }
  auto backdrop = LoadPictTexture(platform, kDockedBackdropPict);
  auto frame =
      LoadPictTexture(platform, NovaLanded_SubWindowFramePict(service));
  // c\x9alr GridDim/GridBright (Bible: store grid color / selection square).
  // The original copies them into DAT_007d8282/DAT_007d827c at startup and the
  // two redraw routines read those globals. The fallback only applies when the
  // color resource is missing entirely.
  const auto ui_style = NovaResource_LoadMainMenuStyle();
  const NovaRgbColor grid_dim =
      ui_style ? ui_style->grid_dim : NovaRgbColor{80, 140, 190};
  const NovaRgbColor grid_bright =
      ui_style ? ui_style->grid_bright : NovaRgbColor{220, 235, 255};
  if (!ui_style) {
    NovaLog::Warn("c\\x9alr unavailable; using provisional store grid colors");
  }
  StoreTextureCache texture_cache;
  std::string selected_description;
  std::int16_t selected_description_id = -1;
  ServicesButtonArt button_art;
  (void)button_art.Initialize(platform);
  NovaFontCache font_cache;
  // Ghidra 0x00492f30 skips this lane for the escort-hire mode. The ordinary
  // Shipyard uses AvailLoc 5, while the Outfitter owns AvailLoc 6.
  const std::optional<std::int16_t> mission_context =
      hire_mode ? std::nullopt
                : std::optional<std::int16_t>{outfit_store ? 6 : 5};
  const auto render_store_background = [&]() {
    RenderStoreScreen(platform,
                      font_cache,
                      button_art,
                      state,
                      session,
                      stellar_id,
                      texture_cache,
                      render_background,
                      backdrop ? backdrop->get() : nullptr,
                      frame ? frame->get() : nullptr,
                      selected_description,
                      grid_dim,
                      grid_bright);
  };
  const auto run_mission_offer = [&]() {
    if (!mission_context) {
      return false;
    }
    return Mission_RunAvailLocOffers(
        state,
        *mission_context,
        static_cast<std::uint32_t>(platform.gameplay_ticks_ms()),
        [&](std::int16_t mission_def) {
          return NovaMission_RunOfferWindow(
              platform, audio, state, mission_def, render_store_background);
        });
  };
  (void)run_mission_offer();
  // Ghidra 0x0048ea70 quantity arms (local_652 & 0x800): an alt-modified
  // buy/sell first computes the affordable/owned maximum and prompts DLOG
  // 0x3eb (FUN_0049e8e0). Plain actions buy/sell one unit. Returns 1 when the
  // maximum is 1 (nothing to choose) and 0 when the prompt is cancelled.
  const auto prompt_buy_quantity = [&]() -> std::int16_t {
    if (!outfit_store || session.selected_id < 0)
      return 1;
    const std::int32_t price =
        NovaLanded_OutfitPrice(state, stellar_id, session.selected_id);
    std::int32_t max = price >= 1 ? state.player.credits / price : 32000;
    max = std::min<std::int32_t>(max, 32000);
    const Outfit *outfit = state.scenario.Outfit(session.selected_id);
    const std::int16_t owned =
        state.inventory.outfit_owned_count[session.selected_id - 0x80];
    if (outfit != nullptr && owned < outfit->max_count) {
      max = std::min<std::int32_t>(max, outfit->max_count - owned);
    }
    const OutfitOwnership own =
        Outfit_ClampOwnedCountToLimits(state, session.selected_id - 0x80);
    if (own.max_allowed < max)
      max = own.max_allowed;
    if (own.max_allowed - owned < max)
      max = own.max_allowed - owned;
    if (max <= 1)
      return 1;
    return RunStoreQuantityPrompt(
        platform, static_cast<std::int16_t>(max), render_store_background);
  };
  const auto prompt_sell_quantity = [&]() -> std::int16_t {
    if (!outfit_store || session.selected_id < 0)
      return 1;
    const std::int16_t owned =
        state.inventory.outfit_owned_count[session.selected_id - 0x80];
    const std::int16_t max = std::min<std::int16_t>(owned, 32000);
    if (max <= 1)
      return 1;
    return RunStoreQuantityPrompt(platform, max, render_store_background);
  };
  // The original input callback only emits the confirm action when the
  // purchase-allowed latch is set. Keep the input gate identical to the
  // disabled-button rendering gate so keyboard and mouse cannot open a
  // quantity/christening dialog for an unavailable purchase.
  const auto can_buy_selected = [&]() {
    if (session.selected_id < 0) {
      return false;
    }
    if (outfit_store) {
      return NovaLanded_StellarSellsOutfits(state, stellar_id) &&
             NovaLanded_CanBuyOutfit(state, stellar_id, session.selected_id);
    }
    return session.hire_mode
               ? NovaLanded_CanHireShip(state, stellar_id, session.selected_id)
               : NovaLanded_CanBuyShip(state, stellar_id, session.selected_id);
  };
  ProbeUiAutoClear probe_ui_guard(platform);
  while (!platform.quit_requested()) {
    state.tick_60hz =
        static_cast<std::uint32_t>(platform.gameplay_ticks_ms() * 60 / 1000);
    const auto recheck_at =
        static_cast<std::uint32_t>(state.mission_interaction_recheck_tick_60hz);
    if (mission_context &&
        static_cast<std::int32_t>(state.tick_60hz - recheck_at) >= 0) {
      (void)run_mission_offer();
    }
    platform.SetPlacement(
        PlaceContained({765.0F, outfit_store ? 321.0F : 323.0F},
                       platform.logical_playfield_size()));
    const StoreLayout layout = LayoutStore(outfit_store);
    // The store windows share one control set; the grid slots are published
    // as label/price items so a harness can find a ship or outfit by name and
    // click its cell without guessing the page layout. Window names
    // distinguish the three screens (the Shipyard and the Bar's Hire Escort
    // action both run this loop).
    const Placement store_placement = platform.current_placement();
    const auto probe_rect = [&store_placement](SDL_FRect rect) {
      return store_placement.ToWindowRect(rect);
    };
    std::vector<ProbeNamedRect> probe_controls{
        // Both store windows caption this control "Done". Retain "leave" as
        // a compatibility alias.
        {"window", probe_rect(layout.frame)},
        {"done", probe_rect(layout.leave)},
        {"leave", probe_rect(layout.leave)},
        {"buy", probe_rect(layout.buy)},
        {"sell_or_info", probe_rect(layout.sell_or_info)},
        {"previous", probe_rect(layout.previous)},
        {"next", probe_rect(layout.next)}};
    for (std::size_t slot = 0; slot < LandedStoreSession::kPageSlots; ++slot) {
      const std::size_t index = session.page_base + slot;
      if (index >= session.available_ids.size()) {
        break;
      }
      const std::int16_t id = session.available_ids[index];
      ProbeNamedRect cell;
      cell.name = "store.slot." + std::to_string(slot);
      cell.rect = probe_rect(StoreCell(layout, slot));
      cell.has_value = true;
      cell.selected = id == session.selected_id;
      if (outfit_store) {
        const Outfit *outfit = state.scenario.Outfit(id);
        cell.label = outfit == nullptr ? std::string{} : outfit->short_name;
        cell.value = NovaLanded_OutfitPrice(state, stellar_id, id);
      } else {
        const ShipClass *ship = state.scenario.Ship(id);
        cell.label = ship == nullptr ? std::string{} : ship->short_name;
        cell.value = session.hire_mode
                         ? NovaLanded_ShipHirePrice(state, stellar_id, id)
                         : NovaLanded_ShipPrice(state, stellar_id, id);
      }
      probe_controls.push_back(std::move(cell));
    }
    platform.PublishProbeUiItems(
        outfit_store ? "outfitter"
                     : (session.hire_mode ? "shipyard_hire" : "shipyard"),
        std::move(probe_controls));
    if (session.selected_id != selected_description_id) {
      selected_description.clear();
      if (session.selected_id >= 0x80) {
        // The selection-desc resource is keyed at the zero-based item index
        // plus the family base: outfits (DLOG 0x3ea) at +3000 (Ghidra
        // NovaUi_HandleOutfitterMenuInput 0x004903c0), ship classes at
        // +13000 for purchase and +14000 for hire (NovaUi_ShipyardHandle
        // SelectionInput 0x00493fc0). The port's selected_id is the raw
        // 0x80+ resource id, so subtract 0x80 first.
        const auto desc_id = static_cast<std::uint16_t>(
            (outfit_store ? 3000 : (session.hire_mode ? 14000 : 13000)) +
            (session.selected_id - 0x80));
        if (const auto description = NovaResource_LoadDescription(desc_id)) {
          selected_description = description->text;
          // The original loads every selection desc through
          // Ui_LoadSelectionDialogResource (0x004c6d50), which runs the
          // {g}/{p}/{b} placeholder pass. These descs carry no mission
          // wildcards, so only the placeholder pass applies.
          Mission_ExpandStringPlaceholders(state, selected_description);
        }
      }
      selected_description_id = session.selected_id;
    }
    RenderStoreScreen(platform,
                      font_cache,
                      button_art,
                      state,
                      session,
                      stellar_id,
                      texture_cache,
                      render_background,
                      backdrop ? backdrop->get() : nullptr,
                      frame ? frame->get() : nullptr,
                      selected_description,
                      grid_dim,
                      grid_bright);
    platform.Present();
    for (std::optional<TextInput> input; (input = platform.PollTextEvent());) {
      if (input->key == TextKey::escape) {
        if (outfit_store)
          NovaLanded_CloseOutfitterSession(state);
        return LandedExit::kServiceComplete;
      }
      if (input->key == TextKey::character) {
        // Deliberate divergence (docs/dlog_ditl_dialog_format.md section 7.2):
        // the original NovaUi_HandleOutfitterMenuInput (0x004903c0) navigates
        // through the command-map menu actions and DIK arrow keys; only 'b'
        // (buy) and 's' (sell) are direct letters. The port adds 'l' leave,
        // 'p'/'n' page and 'i' info as conveniences while the original
        // command-map navigation remains TODO(decomp).
        const char key = static_cast<char>(
            std::tolower(static_cast<unsigned char>(input->character)));
        if (key == 'l') {
          if (outfit_store)
            NovaLanded_CloseOutfitterSession(state);
          return LandedExit::kServiceComplete;
        }
        if (key == 'p') {
          session.PagePrevious();
          continue;
        }
        if (key == 'n') {
          session.PageNext();
          continue;
        }
        if (key == 'b' && session.selected_id >= 0) {
          if (!can_buy_selected()) {
            continue;
          }
          if (outfit_store) {
            const std::int16_t quantity =
                input->alt ? prompt_buy_quantity() : 1;
            if (quantity > 0) {
              (void)NovaLanded_BuyOutfit(
                  state, stellar_id, session.selected_id, quantity);
              NovaLanded_RefreshStoreSession(state, session, stellar_id);
            }
          } else if (session.hire_mode) {
            // Ghidra 0x00492f30 hire arm (the g_shipyard_purchase_mode==1
            // branch of the confirm action): charge + spawn + daily reroll.
            if (NovaLanded_HireShip(state, stellar_id, session.selected_id) !=
                -1) {
              session = NovaLanded_OpenShipyardSession(
                  state, stellar_id, session.hire_mode);
            }
          } else {
            const ShipClass *ship = state.scenario.Ship(session.selected_id);
            if (ship != nullptr) {
              const auto name = RunShipPurchaseConfirmation(
                  platform, state, *ship, render_store_background);
              if (name && NovaLanded_BuyShip(
                              state, stellar_id, session.selected_id, *name)) {
                return LandedExit::kServiceComplete;
              }
            }
          }
          continue;
        }
        if (key == 's' && outfit_store && session.selected_id >= 0) {
          const std::int16_t sold_id = session.selected_id;
          const std::int16_t quantity = input->alt ? prompt_sell_quantity() : 1;
          if (quantity > 0) {
            const OutfitSaleResult sale = NovaLanded_SellOutfit(
                state, session, stellar_id, sold_id, quantity);
            ShowStoreSaleBlock(
                platform, state, sold_id, sale, render_background);
            NovaLanded_RefreshStoreSession(state, session, stellar_id);
          }
          continue;
        }
        if (key == 'i' && !outfit_store && session.selected_id >= 0) {
          RunShipyardInfoDialog(platform,
                                state,
                                session,
                                stellar_id,
                                render_background,
                                backdrop ? backdrop->get() : nullptr,
                                frame ? frame->get() : nullptr,
                                texture_cache,
                                font_cache,
                                button_art,
                                selected_description,
                                grid_dim,
                                grid_bright);
          continue;
        }
      }
      if (input->key != TextKey::primary)
        continue;
      const SDL_FPoint point = platform.mouse_position();
      if (Contains(layout.leave, point)) {
        if (outfit_store)
          NovaLanded_CloseOutfitterSession(state);
        return LandedExit::kServiceComplete;
      }
      if (Contains(layout.previous, point)) {
        session.PagePrevious();
        continue;
      }
      if (Contains(layout.next, point)) {
        session.PageNext();
        continue;
      }
      if (Contains(layout.grid, point)) {
        for (std::size_t slot = 0; slot < LandedStoreSession::kPageSlots;
             ++slot) {
          if (Contains(StoreCell(layout, slot), point)) {
            session.SelectSlot(slot);
            break;
          }
        }
        continue;
      }
      if (Contains(layout.buy, point) && can_buy_selected()) {
        if (outfit_store) {
          const std::int16_t quantity = input->alt ? prompt_buy_quantity() : 1;
          if (quantity > 0) {
            (void)NovaLanded_BuyOutfit(
                state, stellar_id, session.selected_id, quantity);
            NovaLanded_RefreshStoreSession(state, session, stellar_id);
          }
        } else if (session.hire_mode) {
          if (NovaLanded_HireShip(state, stellar_id, session.selected_id) !=
              -1) {
            session = NovaLanded_OpenShipyardSession(state, stellar_id, true);
          }
        } else {
          const ShipClass *ship = state.scenario.Ship(session.selected_id);
          if (ship != nullptr) {
            const auto name = RunShipPurchaseConfirmation(
                platform, state, *ship, render_store_background);
            if (name && NovaLanded_BuyShip(
                            state, stellar_id, session.selected_id, *name)) {
              return LandedExit::kServiceComplete;
            }
          }
        }
        continue;
      }
      if (Contains(layout.sell_or_info, point) && session.selected_id >= 0) {
        if (outfit_store) {
          const std::int16_t sold_id = session.selected_id;
          const std::int16_t quantity = input->alt ? prompt_sell_quantity() : 1;
          if (quantity > 0) {
            const OutfitSaleResult sale = NovaLanded_SellOutfit(
                state, session, stellar_id, sold_id, quantity);
            ShowStoreSaleBlock(
                platform, state, sold_id, sale, render_background);
            NovaLanded_RefreshStoreSession(state, session, stellar_id);
          }
        } else {
          // The shipyard's third action button is Info
          // (0x0049f3f0 HitTestAndTrackShipyardActionButtons index 2), which
          // opens the detail window (0x004956a0).
          RunShipyardInfoDialog(platform,
                                state,
                                session,
                                stellar_id,
                                render_background,
                                backdrop ? backdrop->get() : nullptr,
                                frame ? frame->get() : nullptr,
                                texture_cache,
                                font_cache,
                                button_art,
                                selected_description,
                                grid_dim,
                                grid_bright);
        }
      }
    }
    platform.PaceFrame();
  }
  return LandedExit::kQuit;
}

} // namespace game
