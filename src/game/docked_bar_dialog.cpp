// Docked Bar window: prompt/holovid/gamble/hire actions, the Holovid news
// window, and the exposed news-text helpers.
//
// Split out of the original docked_dialog.cpp.

#include "docked_dialog_internal.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../sdl_platform.hpp"
#include "boarding_plunder.hpp"
#include "government.hpp"
#include "hud_overlay.hpp"
#include "mission.hpp"
#include "nova_font.hpp"
#include "scenario_data.hpp"
#include "selection_text_dialog.hpp"
#include "services_buttons.hpp"
#include "ship_ai.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace game {
namespace {

constexpr std::uint16_t kBarDialogId = 0x3f5;
constexpr std::uint16_t kBarDialogWithArtId = 0x3fd;
constexpr std::uint16_t kNewsDialogId = 0x3f6;
constexpr std::uint16_t kBarBackdropPict = 0x2137;
constexpr std::uint16_t kBarBackdropWithArtPict = 0x2138;
constexpr std::uint16_t kNewsDefaultPict = 9000;

// STR# 0x96 button-label pool indices per DITL slot (DAT_007d831a). Slots 3
// and 5 are never written by the original (0x004a2810 writes only 0/1/2/4)
// AND their DITL rects lie outside the window (clipped away there, skipped
// by slot_visible here), so they are unreachable either way.
constexpr std::array<std::uint16_t, 6> kBarButtonLabels{
    0, 10, 0x0b, 0xffff, 0x0c, 0xffff};

// Shared filled-rect text draw, mirroring
// DrawContext_DrawPascalStringInFilledRect (0x004bcd30) with the shared screen
// font (Ship_InitGameplayDataTables 0x004b0c20 sets DAT_00735684 = Geneva and
// DAT_00735686 = 9). The original fills the rect, then NovaText_DrawText
// (0x004bc760) lays the text out word-wrapped *inside the rect from its top*
// via DrawTextW's DT_WORDBREAK branch (the cursor it set is only used by the
// measure branch); callers invert the rect, netting white-on-black. We
// approximate the top-aligned first line with a baseline at
// rect.top + font size and advance later lines by the font line height.
constexpr float kDialogTextSize = 9.0F;
constexpr float kDialogTextFirstBaseline = 9.0F;

void DrawDialogFilledText(SdlPlatform &platform,
                          NovaFontCache &font_cache,
                          const SDL_FRect &rect,
                          std::string_view text) {
  if (rect.w <= 0.0F || rect.h <= 0.0F || text.empty()) {
    return;
  }
  SDL_SetRenderDrawColor(platform.renderer(), 0, 0, 0, SDL_ALPHA_OPAQUE);
  SDL_RenderFillRect(platform.renderer(), &rect);
  const auto lines = WrapDescriptionLines(
      text,
      static_cast<int>(std::max(1.0F, rect.w)),
      [&](std::string_view line) {
        return font_cache.TextWidth(NovaFontFamily::kGeneva,
                                    kDialogTextSize,
                                    kNovaFontStyleRegular,
                                    line);
      });
  const SDL_Rect clip{static_cast<int>(rect.x),
                      static_cast<int>(rect.y),
                      static_cast<int>(rect.w),
                      static_cast<int>(rect.h)};
  SDL_SetRenderClipRect(platform.renderer(), &clip);
  constexpr SDL_Color kWhite{255, 255, 255, 255};
  const float line_height = static_cast<float>(
      font_cache.LineHeight(NovaFontFamily::kGeneva, kDialogTextSize));
  float y = rect.y + kDialogTextFirstBaseline;
  for (const auto &line : lines) {
    if (y > rect.y + rect.h) {
      break;
    }
    NovaText_Draw(platform,
                  font_cache,
                  NovaFontFamily::kGeneva,
                  kDialogTextSize,
                  kNovaFontStyleRegular,
                  kWhite,
                  rect.x,
                  y,
                  line);
    y += line_height;
  }
  SDL_SetRenderClipRect(platform.renderer(), nullptr);
}

// Ghidra 0x0047d600 NovaUi_ComposeTravelNewsTexts: composes the news-window
// texts shown by the Bar's Holovid button. Headline: a random STR# 0x1fa4
// (Commercials) entry, falling back to STR# 0x7d2 0xbe when the pool is
// missing. Body, in precedence order: (1) the disaster report for an active
// öops record (preferring one at the current stellar, or a target -2 record,
// else any active one) composed from the STR# 0x7d2 fragments and the STR#
// 0xfa1 commodity name; (2) the crön-news arm (Bar_SelectCronNewsStr), which
// overwrites the body -- local allied-government news wins over independent
// news, and either over the disaster report; (3) a random STR# 0x1fa5
// (Generic News) entry, falling back to 0x7d2 0xbf. Only the body is
// disaster/crön-specific; the headline is unaffected.
void NovaBar_ComposeNewsTexts(GameState &state,
                              std::int16_t landed_stellar_id,
                              std::string &headline,
                              std::string &body) {
  headline.clear();
  body.clear();
  const auto roll_entry =
      [&](std::uint16_t pool) -> std::optional<std::string> {
    const std::uint16_t count = NovaHud_StringPoolEntryCount(pool);
    if (count == 0) {
      return std::nullopt;
    }
    const auto entry = static_cast<std::uint16_t>(
        std::uniform_int_distribution<int>{0, count - 1}(state.rng) + 1);
    return NovaHud_LoadStringEntry(pool, entry);
  };
  headline = roll_entry(0x1fa4).value_or(
      NovaHud_LoadStringEntry(0x7d2, 0xbe).value_or("No news is good news"));

  if (const auto disaster_body =
          Bar_ComposeDisasterReport(state, landed_stellar_id)) {
    body = *disaster_body;
  }

  // The crön arm replaces the body in place (0x0047d600 overwrites
  // DAT_007d0e9c), so active crön news takes precedence over the disaster
  // report.
  if (const auto cron_str = Bar_SelectCronNewsStr(state, landed_stellar_id)) {
    body = roll_entry(static_cast<std::uint16_t>(*cron_str)).value_or("");
  }

  if (body.empty()) {
    body = roll_entry(0x1fa5).value_or(
        NovaHud_LoadStringEntry(0x7d2, 0xbf).value_or(""));
  }
}

// Ghidra 0x0047d180 NovaUi_RunTravelNewsWindow + 0x0047d370
// NovaUi_DrawTravelNewsWindow (inline): the Holovid window (DLOG 0x3f6). The
// news PICT is the destination government's news_pic_id, else the generic
// PICT 9000; the headline band sits at (left+10, top+140, right-10,
// bottom-180) and the body panel at (left+10, top+170, right-4, bottom-10);
// both are the original's filled+inverted rects, i.e. a black panel with
// white wrapped Geneva-12 text. The input handler
// (NovaUi_HandleTravelNewsWindowInput 0x0047d560) runs inline in the modal loop
// below: Esc/Return (key codes 0xd/0x1b) close with action 1, a primary click
// inside the window rect closes, and the per-frame draw covers its action-6
// redraw request.
void RunBarNewsWindow(SdlPlatform &platform,
                      GameState &state,
                      std::int16_t stellar_id,
                      const std::string &headline,
                      const std::string &body,
                      const std::function<void()> &render_background) {
  const SdlPlatform::ScopedPlacement restore_placement(
      platform, platform.current_placement());
  // Government news PICT with the 9000 fallback (0x0047d180 prologue).
  const std::int16_t news_pict =
      static_cast<std::int16_t>(NovaBar_NewsPictId(state, stellar_id));
  auto news_art =
      LoadPictTexture(platform, static_cast<std::uint16_t>(news_pict));
  if (news_art == nullptr && news_pict != kNewsDefaultPict) {
    news_art = LoadPictTexture(platform, kNewsDefaultPict);
  }

  const auto dlog = NovaResource_LoadDialogDefinition(kNewsDialogId);
  if (!dlog) {
    NovaLog::Todo("news DLOG 0x3f6 unavailable; skipping the Holovid window");
    return;
  }
  // DITL 0x3f6 is the 2-byte 0xffff placeholder in Nova.rez (the news text
  // is drawn from the DLOG bounds plus two hardcoded panels by
  // NovaUi_DrawTravelNewsWindow 0x0047d370); no DITL items are required.
  const float win_w = static_cast<float>(dlog->right - dlog->left);
  const float win_h = static_cast<float>(dlog->bottom - dlog->top);
  const SDL_FPoint origin{0.0F, 0.0F};
  const SDL_FRect window{origin.x, origin.y, win_w, win_h};

  auto backdrop = LoadPictTexture(platform, kDockedBackdropPict);
  ServicesButtonArt button_art;
  (void)button_art.Initialize(platform);
  NovaFontCache font_cache;

  ProbeUiAutoClear probe_ui_guard(platform);
  const auto draw_frame = [&]() {
    SDL_SetRenderDrawColor(platform.renderer(), 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(platform.renderer());
    if (render_background) {
      render_background();
    } else if (backdrop != nullptr) {
      DrawContainedPict(platform, backdrop->get());
    }
    platform.SetPlacement(PlaceContained({window.w, window.h},
                                         platform.logical_playfield_size()));
    const SDL_FRect probe_window =
        platform.current_placement().ToWindowRect(window);
    platform.PublishProbeUi(
        "bar_news", {{"window", probe_window}, {"close", probe_window}});
    // Window fill + news art blitted over the window rect (0x0047d370).
    SDL_SetRenderDrawColor(platform.renderer(), 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(platform.renderer(), &window);
    if (news_art != nullptr) {
      SDL_RenderTexture(platform.renderer(), news_art->get(), nullptr, &window);
    }
    // The two filled+inverted text panels (NovaUi_DrawTravelNewsWindow
    // 0x0047d370), window-relative as computed by NovaBar_NewsTextPanelRects;
    // the body is drawn second and covers the 10px overlap.
    const NewsTextPanels panels = NovaBar_NewsTextPanelRects(win_w, win_h);
    const auto panel_rect = [&](const std::array<float, 4> &r) {
      return SDL_FRect{
          window.x + r[0], window.y + r[1], r[2] - r[0], r[3] - r[1]};
    };
    DrawDialogFilledText(
        platform, font_cache, panel_rect(panels.headline), headline);
    DrawDialogFilledText(platform, font_cache, panel_rect(panels.body), body);
    platform.Present();
  };

  while (!platform.quit_requested()) {
    draw_frame();
    for (std::optional<TextInput> in; (in = platform.PollTextEvent());) {
      const bool close_key =
          in->key == TextKey::escape || in->key == TextKey::enter;
      const bool close_click = in->key == TextKey::primary &&
                               Contains(window, platform.mouse_position());
      if (close_key || close_click) {
        return;
      }
    }
    platform.PaceFrame();
  }
}

} // namespace

// Commodity name for the disaster report (g_disaster_defs +4 indexes the
// 0x100-stride DAT_0069d2cc Pascal-string table; NovaData_LoadDisplayName-
// PstringTables 0x004c7040 fills entry n from STR# 0xfa1 n+1, falling back to
// FUN_004c73b0(id+0x238c)). The original clamps a negative commodity to -1 and
// would read the preceding table slot; only 0..5 are shipped, so guard here.
std::string BarCommodityName(std::int16_t commodity) {
  if (commodity < 0 || commodity > 5) {
    return "?";
  }
  return NovaResources_LoadPatchedStringEntry(
             0xfa1, static_cast<std::uint16_t>(commodity + 1), 0x238c)
      .value_or("?");
}

// Ghidra 0x0047c8e0 NovaUi_RunTravelDestinationServicesWindow.
LandedExit RunBarDialog(SdlPlatform &platform,
                        SdlAudio &audio,
                        GameState &state,
                        std::int16_t stellar_id,
                        const std::function<void()> &render_background) {
  const SdlPlatform::ScopedPlacement restore_placement(
      platform, platform.current_placement());
  // The original composes the news texts once at bar entry
  // (NovaUi_ComposeTravelNewsTexts 0x0047d600, before the selection-dialog
  // load); the Holovid window (0x0047d180) only displays the stored strings,
  // so repeated Holovid opens show the same text. Keep the same split here.
  std::string news_headline;
  std::string news_body;
  NovaBar_ComposeNewsTexts(state, stellar_id, news_headline, news_body);

  // Prompt text: the bar/destination desc is the `dësc` at (0-based stellar
  // index + 10000), e.g. Earth -> 10000 "Bars: Earth", Port Kane (0x89) ->
  // 10009 "The Hypergate". The original passes
  // g_ship_states->ai_secondary_target_slot + 10000 and that slot is the
  // 0-based g_stellar_defs index -- NOT the raw stellar id, which is a
  // different desc family (the landing description loaded by
  // NovaResource_LoadStellarDescription). Run through the placeholder pass
  // (Ui_LoadSelectionDialogResource 0x004c6d50). A variant >= 0x80 is the art
  // PICT and selects the DLOG 0x3fd / PICT 0x2138 pair.
  std::string prompt_text;
  std::uint16_t art_pict = 0;
  if (const auto desc_id = NovaLanded_BarDescriptionId(stellar_id)) {
    if (const auto desc = NovaResource_LoadDescription(*desc_id)) {
      prompt_text = desc->text;
      Mission_ExpandStringPlaceholders(state, prompt_text);
      if (desc->dialog_variant >= 0x80) {
        art_pict = static_cast<std::uint16_t>(desc->dialog_variant);
      }
    }
  }
  const bool has_art = art_pict != 0;

  const std::uint16_t dialog_id = has_art ? kBarDialogWithArtId : kBarDialogId;
  const auto dlog = NovaResource_LoadDialogDefinition(dialog_id);
  const auto items =
      dlog ? NovaResource_LoadDialogItems(dlog->dialog_item_list_id)
           : std::nullopt;
  if (!dlog || !items) {
    NovaLog::Todo("bar DLOG/DITL 0x{:x} unavailable; returning to the dock "
                  "menu",
                  dialog_id);
    return LandedExit::kServiceComplete;
  }

  auto bar_art = LoadPictTexture(
      platform, has_art ? kBarBackdropWithArtPict : kBarBackdropPict);
  auto destination_art =
      has_art ? LoadPictTexture(platform, art_pict) : nullptr;
  auto backdrop = LoadPictTexture(platform, kDockedBackdropPict);
  ServicesButtonArt button_art;
  (void)button_art.Initialize(platform);
  NovaFontCache font_cache;

  const float win_w = static_cast<float>(dlog->right - dlog->left);
  const float win_h = static_cast<float>(dlog->bottom - dlog->top);
  const SDL_FPoint origin{0.0F, 0.0F};
  const SDL_FRect window{origin.x, origin.y, win_w, win_h};
  const auto item_rect = [&](std::size_t index) {
    for (const auto &item : *items) {
      if (item.index == index) {
        return SDL_FRect{origin.x + static_cast<float>(item.left),
                         origin.y + static_cast<float>(item.top),
                         static_cast<float>(item.right - item.left),
                         static_cast<float>(item.bottom - item.top)};
      }
    }
    return SDL_FRect{};
  };
  std::array<SDL_FRect, 6> buttons{};
  for (std::size_t slot = 0; slot < buttons.size(); ++slot) {
    buttons[slot] = item_rect(slot); // UiPanel entries 1..6
  }
  // DITL 0x3f5/0x3fd define two extra inert button slots (3 and 5) whose
  // rects extend below the DLOG bottom edge (relative tops 214/227 vs a 185px
  // window). The original never shows them because modal windows clip
  // drawing to the DLOG bounds; skip any button rect that does not fit so
  // the strip stays inside the modal.
  const auto slot_visible = [&](const SDL_FRect &rect) {
    return rect.w > 0.0F && rect.h > 0.0F && rect.x >= window.x &&
           rect.y >= window.y &&
           rect.x + rect.w <= window.x + window.w + 0.5F &&
           rect.y + rect.h <= window.y + window.h + 0.5F;
  };
  const SDL_FRect prompt_rect = item_rect(6); // UiPanel entry 7
  const SDL_FRect art_rect = item_rect(7);    // UiPanel entry 8

  // 0x004a2810: slots 0-3 always enabled, slot 4 (Hire Escort) gated on the
  // escort-capacity check, slot 5 always disabled.
  const auto slot_enabled = [&](std::size_t slot) {
    if (slot == 4) {
      return NovaShip_CanPlayerHaveMoreEscorts(state);
    }
    return slot != 5;
  };

  // Draws the bar surface into the current backbuffer and does NOT present.
  // It is both the body of the bar's own frame and the render_background
  // handed to nested modals (mission offers, text readers, the hire-escort
  // store). Nested modals must layer themselves over this and present once at
  // the end of their frame; passing draw_frame (which presents) as
  // render_background made SDL swap a bar frame and then draw the nested
  // window into the already-invalidated backbuffer, which is what produced the
  // glitchy mission-offer rendering. Mirrors the landed/store/trade pattern
  // (docs/dlog_ditl_dialog_format.md section 7.1).
  const auto draw_bar_contents = [&]() {
    SDL_SetRenderDrawColor(platform.renderer(), 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(platform.renderer());
    if (render_background) {
      render_background();
    } else if (backdrop != nullptr) {
      DrawContainedPict(platform, backdrop->get());
    }
    platform.SetPlacement(PlaceContained({window.w, window.h},
                                         platform.logical_playfield_size()));
    // Window fill + backdrop PICT (0x0047cfe0).
    SDL_SetRenderDrawColor(platform.renderer(), 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(platform.renderer(), &window);
    if (bar_art != nullptr) {
      SDL_RenderTexture(platform.renderer(), bar_art->get(), nullptr, &window);
    }
    // Destination art panel (entry 8, 0x3fd pair only).
    if (destination_art != nullptr) {
      SDL_RenderTexture(
          platform.renderer(), destination_art->get(), nullptr, &art_rect);
    }
    // Prompt panel (entry 7): filled+inverted -> black panel, white text
    // (DrawContext_DrawPascalStringInFilledRect 0x004bcd30 via the bar draw
    // 0x0047cfe0).
    DrawDialogFilledText(platform, font_cache, prompt_rect, prompt_text);
    // The six-button strip. Hover highlights a slot the way
    // 0x004a26e0's hit-test loop re-draws with the pressed art.
    const SDL_FPoint mouse = platform.mouse_position();
    constexpr SDL_Color kLabel{255, 255, 255, 255};
    constexpr SDL_Color kLabelGrey{128, 128, 128, 255};
    for (std::size_t slot = 0; slot < buttons.size(); ++slot) {
      if (!slot_visible(buttons[slot])) {
        continue;
      }
      const bool enabled = slot_enabled(slot);
      const bool hovered = enabled && Contains(buttons[slot], mouse);
      button_art.Draw(platform,
                      buttons[slot],
                      !enabled  ? ButtonState::kDisabled
                      : hovered ? ButtonState::kHover
                                : ButtonState::kNormal);
      const std::uint16_t label_id = kBarButtonLabels[slot];
      if (label_id == 0xffff) {
        continue;
      }
      const auto label = NovaHud_LoadStringEntry(
          0x96, static_cast<std::uint16_t>(label_id + 1U));
      DrawThreeStateButtonLabel(platform,
                                font_cache,
                                buttons[slot],
                                label.value_or(""),
                                enabled ? kLabel : kLabelGrey);
    }
  };

  const auto draw_frame = [&]() {
    draw_bar_contents();
    platform.Present();
  };

  ProbeUiAutoClear probe_ui_guard(platform);
  const auto publish_probe_controls = [&]() {
    const Placement bar_placement =
        PlaceContained({window.w, window.h}, platform.logical_playfield_size())
            .Canonicalized();
    const auto probe_rect = [&bar_placement](SDL_FRect rect) {
      return bar_placement.ToWindowRect(rect);
    };
    platform.PublishProbeUi("bar",
                            {{"window", probe_rect(window)},
                             {"leave", probe_rect(buttons[0])},
                             {"gamble", probe_rect(buttons[1])},
                             {"holovid", probe_rect(buttons[2])},
                             {"hire_escort", probe_rect(buttons[4])}});
  };
  publish_probe_controls();

  const auto run_mission_offer = [&]() {
    return Mission_RunAvailLocOffers(
        state,
        1,
        static_cast<std::uint32_t>(platform.gameplay_ticks_ms()),
        [&](std::int16_t mission_def) {
          return NovaMission_RunOfferWindow(platform,
                                            audio,
                                            state,
                                            mission_def,
                                            stellar_id,
                                            draw_bar_contents);
        });
  };

  // Ghidra 0x0047c8e0 sets g_misn_list_page_group = 1 and schedules action 6
  // (Mission_RunAvailLocOffers(1)) fifteen 60 Hz ticks after
  // entry.
  state.tick_60hz =
      static_cast<std::uint32_t>(platform.gameplay_ticks_ms() * 60 / 1000);
  state.mission_interaction_recheck_tick_60hz =
      static_cast<std::int32_t>(state.tick_60hz) + 0x0f;

  // Modal action dispatcher (0x0047c8e0's action arms). Returns true when
  // the bar window should close.
  const auto run_action = [&](std::int16_t action) -> bool {
    switch (action) {
    case 2: {
      // Gamble (NovaUi_RunBarGamblingWindow 0x0047dc50 -- the DB name was
      // NovaUi_RunTravelGoodsFlashWindow before the 2026 decode).
      if (state.player.credits < 1) {
        // STR# 0x7d2 0x169 via the shared text-reader dialog.
        NovaUi_RunTextReaderDialog(
            platform,
            state,
            NovaHud_LoadStringEntry(0x7d2, 0x169)
                .value_or("Sorry, you don't have enough credits to bet today."),
            false,
            render_background);
      } else {
        // TODO(decomp(0x0047dc50)) skipped: the gambling window (DLOG 0x3ff,
        // backdrop 0x2151, bet 1000/10000 arms, 4x payout on the rand(4)
        // match, outcome descs 0x7ff8+roll) is not reconstructed.
        NovaLog::Todo("bar gamble window 0x0047dc50 not reconstructed yet");
      }
      break;
    }
    case 3:
      // Holovid / news window (NovaUi_RunTravelNewsWindow 0x0047d180).
      RunBarNewsWindow(platform,
                       state,
                       stellar_id,
                       news_headline,
                       news_body,
                       render_background);
      break;
    case 5:
      // Hire Escort: capacity gate + the shipyard purchase loop in hire mode
      // (g_shipyard_purchase_mode = 1).
      if (NovaShip_CanPlayerHaveMoreEscorts(state)) {
        (void)RunStoreDialog(platform,
                             audio,
                             state,
                             LandedService::kShipyard,
                             stellar_id,
                             render_background,
                             /*hire_mode=*/true);
      }
      break;
    default:
      break;
    }
    return false;
  };

  while (!platform.quit_requested()) {
    // Mission offers and other nested modals clear their semantic controls.
    // Restore the bar surface when its root loop resumes.
    publish_probe_controls();
    // The original's shared 60 Hz counter advances independently of the
    // flight loop, so refresh it here (this modal owns the frame clock) before
    // consulting the recheck deadline.
    state.tick_60hz =
        static_cast<std::uint32_t>(platform.gameplay_ticks_ms() * 60 / 1000);
    const auto recheck_at =
        static_cast<std::uint32_t>(state.mission_interaction_recheck_tick_60hz);
    if (static_cast<std::int32_t>(state.tick_60hz - recheck_at) >= 0) {
      (void)run_mission_offer();
    }
    draw_frame();
    bool close = false;
    for (std::optional<TextInput> in; (in = platform.PollTextEvent());) {
      const SDL_FPoint point = platform.mouse_position();
      if (in->key == TextKey::escape || in->key == TextKey::enter) {
        close = true; // action 1: Leave
        break;
      }
      if (in->key == TextKey::character) {
        // 0x0047cdb0's key arms: g/r gamble, w/n holovid, h/e hire.
        const char key = static_cast<char>(
            std::tolower(static_cast<unsigned char>(in->character)));
        if (key == 'g' || key == 'r') {
          close = run_action(2);
        } else if (key == 'w' || key == 'n') {
          close = run_action(3);
        } else if (key == 'h' || key == 'e') {
          close = run_action(5);
        }
        if (close) {
          break;
        }
        continue;
      }
      if (in->key != TextKey::primary) {
        continue;
      }
      // Button hit test (0x004a26e0): slot 0 Leave, 1 Gamble, 2 Holovid,
      // 4 Hire Escort; the out-of-window slots are unreachable (clipped),
      // and the remaining inert slot redraws only.
      for (std::size_t slot = 0; slot < buttons.size(); ++slot) {
        if (slot_visible(buttons[slot]) && Contains(buttons[slot], point)) {
          if (slot == 0) {
            close = true;
          } else if (slot_enabled(slot)) {
            const std::array<std::int16_t, 6> slot_action{1, 2, 3, -1, 5, -1};
            if (slot_action[slot] > 0) {
              close = run_action(slot_action[slot]);
            }
          }
          break;
        }
      }
      if (close) {
        break;
      }
    }
    if (close) {
      // Mission_ClearActiveReactionMission (0x00448660) runs on the window
      // teardown path of NovaUi_RunTravelDestinationServicesWindow.
      Mission_ClearActiveReactionMission(state);
      return LandedExit::kServiceComplete;
    }
    platform.PaceFrame();
  }
  Mission_ClearActiveReactionMission(state);
  return LandedExit::kQuit;
}

// Ghidra 0x0047c8e0 NovaUi_RunTravelDestinationServicesWindow prompt id.
std::optional<std::uint16_t>
NovaLanded_BarDescriptionId(std::int16_t stellar_id) {
  if (stellar_id < kResourceIdBase) {
    return std::nullopt;
  }
  return static_cast<std::uint16_t>((stellar_id - kResourceIdBase) + 10000);
}

// Ghidra 0x0047d370 NovaUi_DrawTravelNewsWindow panel geometry.
NewsTextPanels NovaBar_NewsTextPanelRects(float window_w, float window_h) {
  return NewsTextPanels{
      .headline = {10.0F, 140.0F, window_w - 10.0F, 180.0F},
      .body = {10.0F, 170.0F, window_w - 10.0F, window_h - 4.0F},
  };
}

// Ghidra 0x0047d600 NovaUi_ComposeTravelNewsTexts, crön-news arm. The original
// scans all 0x200 cron blocks for active, past-holdoff events; each allied
// NewsGovt/GovtNewsStr pair leaves its (last matching) string id as the slot's
// local candidate, and IndNewsStr arms the slot for independent news only when
// no allied pair matched. Local candidates always win; the chosen id is drawn
// uniformly over the contributing slots (the original's rejection sampling over
// 0x200 slots is equivalent).
std::optional<std::int16_t>
Bar_SelectCronNewsStr(GameState &state, std::int16_t landed_stellar_id) {
  const Stellar *stellar = state.scenario.Stellar(landed_stellar_id);
  const std::int16_t stellar_govt =
      stellar != nullptr ? stellar->government_id : -1;
  std::vector<std::int16_t> local;
  std::vector<std::int16_t> independent;
  const std::size_t count = std::min(state.scenario.cron_events.size(),
                                     state.cron_event_states.size());
  for (std::size_t i = 0; i < count; ++i) {
    const CronEventDef &def = state.scenario.cron_events[i];
    const GameState::CronEventState &runtime = state.cron_event_states[i];
    if (!def.present || !runtime.is_active || runtime.holdoff_counter >= 1) {
      continue;
    }
    std::int16_t slot_str = -2; // no allied NewsGovt matched
    for (std::size_t k = 0; k < def.news_govts.size(); ++k) {
      if (def.news_govts[k] == -1) {
        continue;
      }
      if (NovaGovernment_AreGovtsAllied(
              state.scenario, def.news_govts[k], stellar_govt)) {
        slot_str = def.govt_news_strs[k];
      }
    }
    if (slot_str > 0) {
      local.push_back(slot_str);
    } else if (slot_str < -1 && def.independent_news_str > 0) {
      independent.push_back(def.independent_news_str);
    }
  }
  const std::vector<std::int16_t> &pool = !local.empty() ? local : independent;
  if (pool.empty()) {
    return std::nullopt;
  }
  std::uniform_int_distribution<std::size_t> pick(0, pool.size() - 1);
  return pool[pick(state.rng)];
}

// Ghidra 0x0047d180 NovaUi_RunTravelNewsWindow (prologue): the news PICT is
// the landed stellar's government news_pic_id, else the generic PICT 9000.
// Stellar::government_id is the loader's rebased zero-based faction index, so
// the lookup must use GovernmentByIndex; the 0x80-based Government() would
// subtract 0x80 again, return null for every stellar, and always fall back to
// 9000 (the generic ICN art).
std::uint16_t NovaBar_NewsPictId(const GameState &state,
                                 std::int16_t stellar_id) {
  const Stellar *stellar = state.scenario.Stellar(stellar_id);
  if (stellar == nullptr || stellar->government_id == -1) {
    return kNewsDefaultPict;
  }
  const Government *govt =
      state.scenario.GovernmentByIndex(stellar->government_id);
  if (govt == nullptr || govt->news_pic_id == -1) {
    return kNewsDefaultPict;
  }
  return static_cast<std::uint16_t>(govt->news_pic_id);
}

// Ghidra 0x0047d600 NovaUi_ComposeTravelNewsTexts, disaster-report arm. The
// original counts defined records with more than one remaining active day,
// tracking separately those bound to the current stellar (active_stellar ==
// the selected stellar, or target_stellar == -2 "everywhere"), then
// reject-samples the relevant subset; the clean-room table is shorter, so the
// candidate set is built explicitly. Returns the composed body when the drawn
// record is active and named, else nullopt so the caller falls through to the
// generic-news pool. Exposed for tests (see tests/docked_dialog_test.cpp).
std::optional<std::string>
Bar_ComposeDisasterReport(GameState &state, std::int16_t landed_stellar_id) {
  const std::int16_t stellar_index =
      landed_stellar_id >= kResourceIdBase
          ? static_cast<std::int16_t>(landed_stellar_id - kResourceIdBase)
          : static_cast<std::int16_t>(-1);
  std::vector<std::size_t> active_anywhere;
  std::vector<std::size_t> active_here;
  for (std::size_t i = 0; i < state.scenario.disaster_defs.size(); ++i) {
    const DisasterDef &disaster = state.scenario.disaster_defs[i];
    if (!disaster.present || disaster.days_remaining <= 1) {
      continue;
    }
    active_anywhere.push_back(i);
    if (disaster.active_stellar == stellar_index ||
        disaster.target_stellar == -2) {
      active_here.push_back(i);
    }
  }
  if (active_anywhere.empty()) {
    return std::nullopt;
  }
  const auto &candidates = active_here.empty() ? active_anywhere : active_here;
  std::uniform_int_distribution<std::size_t> pick(0, candidates.size() - 1);
  const DisasterDef &disaster =
      state.scenario.disaster_defs[candidates[pick(state.rng)]];
  if (disaster.active_stellar == -1 || disaster.display_name.empty()) {
    return std::nullopt;
  }
  const Stellar *stellar = state.scenario.Stellar(
      static_cast<std::int16_t>(disaster.active_stellar + kResourceIdBase));
  const auto misc = [](std::uint16_t entry) {
    return NovaHud_LoadStringEntry(0x7d2, entry).value_or("");
  };
  // "Today's top news: <disaster> has raised/lowered the price of <commodity>
  // on <stellar>." (price_delta < 1 -> lowered).
  std::string text = misc(0xbe);
  text += " ";
  text += disaster.display_name;
  text += " ";
  text += misc(0xc0); // has
  text += " ";
  text += misc(disaster.price_delta < 1 ? 0xc2 : 0xc1);
  text += " ";
  text += misc(0xb5); // the price of
  text += " ";
  text += BarCommodityName(disaster.commodity);
  text += " ";
  text += misc(0x3c); // on
  text += " ";
  if (stellar != nullptr) {
    text += stellar->name;
  }
  text += ".";
  return text;
}

} // namespace game
