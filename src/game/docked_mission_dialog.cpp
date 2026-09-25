// Mission windows: the docked Mission BBS, the mission-ship offer window,
// and the in-flight mission computer (default key I).
//
// Split out of the original docked_dialog.cpp.

#include "docked_dialog_internal.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../sdl_audio.hpp"
#include "../sdl_platform.hpp"
#include "../util/color.hpp"
#include "command_input.hpp"
#include "hud_overlay.hpp"
#include "mission.hpp"
#include "mission_script.hpp"
#include "nova_font.hpp"
#include "nova_list_control.hpp"
#include "player_info_window.hpp"
#include "scenario_data.hpp"
#include "selection_text_dialog.hpp"
#include "services_buttons.hpp"
#include "spaceflight.hpp"
#include "starmap.hpp"
#include "travel.hpp"
#include "ui_dialog.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace game {

namespace {

struct MissionBbsLayout {
  SDL_FRect frame{};
  SDL_FRect list{};
  SDL_FRect scrollbar{};
  SDL_FRect selected_title{};
  SDL_FRect description{};
  SDL_FRect take{};
  SDL_FRect decline{};
  SDL_FRect header{};
  SDL_FRect date{};
};

// NovaUi_DrawListRowCallback (0x00448a30) receives the native row rectangle,
// places text at row_top + DAT_00735686, and uses a four-pixel left inset.
// Both mission lists render with the shared screen font DAT_00735684/86 —
// Geneva 9, initialised in Ship_InitGameplayDataTables (0x004b0c20).
constexpr float kMissionListFontSize = 9.0F;
// Native list-control row pitch: the rebuild passes
// (Stellar_RebuildTravelDestinationList 0x0043cc20 and
// NovaUi_RebuildSpecialInteractionList 0x00445dc0) size rows as
// DAT_0088c01c (8) x the 1.5 UI-scale double (0x005754f8) — 12px,
// independent of the DITL rect height.
constexpr float kMissionListRowPitch = 12.0F;

// Shared rebindable command slots polled by the mission windows (the same ones
// the flight loop uses): slot 9 = galaxy map (default M), slot 0x19 = Player
// Info (default P), slot 0x28 = mission computer (default I). Ghidra's BBS
// poller reads the corresponding g_nova_control_bits channels (_44_2_, _76_2_,
// _106_2_); the port resolves them through the one binding table.
constexpr std::size_t kStarmapCommand = 0x09;
constexpr std::size_t kPlayerInfoCommand = 0x19;
constexpr std::size_t kMissionComputerCommand = 0x28;

[[nodiscard]] std::optional<MissionBbsLayout> LayoutMissionBbs() {
  const auto definition = NovaResource_LoadDialogDefinition(0x3ee);
  const auto items =
      definition ? NovaResource_LoadDialogItems(definition->dialog_item_list_id)
                 : std::nullopt;
  if (!definition || !items) {
    NovaLog::Todo("Mission BBS DLOG/DITL 0x3ee unavailable; refusing to use "
                  "synthetic layout");
    return std::nullopt;
  }

  const float width = static_cast<float>(definition->right - definition->left);
  const float height = static_cast<float>(definition->bottom - definition->top);
  const SDL_FPoint origin{0.0F, 0.0F};
  MissionBbsLayout layout;
  layout.frame = {origin.x, origin.y, width, height};
  const auto item_rect = [origin](const NovaDialogItem &item) {
    return SDL_FRect{origin.x + static_cast<float>(item.left),
                     origin.y + static_cast<float>(item.top),
                     static_cast<float>(item.right - item.left),
                     static_cast<float>(item.bottom - item.top)};
  };
  for (const auto &item : *items) {
    switch (item.index) {
    case 1: // UiPanel_GetEntryInfo entry 2: native mission list
      layout.list = item_rect(item);
      break;
    case 2: // UiPanel_GetEntryInfo entry 3: native list scrollbar
      layout.scrollbar = item_rect(item);
      break;
    case 0: // UiPanel_GetEntryInfo entry 1: accept/take button
      layout.take = item_rect(item);
      break;
    case 6: // UiPanel_GetEntryInfo entry 7: leave button
      layout.decline = item_rect(item);
      break;
    case 3: // UiPanel_GetEntryInfo entry 4: destination description
      layout.description = item_rect(item);
      break;
    case 4: // UiPanel_GetEntryInfo entry 5: selected mission title
      layout.selected_title = item_rect(item);
      break;
    case 7: // UiPanel_GetEntryInfo entry 8: heading band
      layout.header = item_rect(item);
      break;
    case 10: // UiPanel_GetEntryInfo entry 11: current date
      layout.date = item_rect(item);
      break;
    default:
      break;
    }
  }

  if (layout.list.w <= 0.0F || layout.list.h <= 0.0F ||
      layout.description.w <= 0.0F || layout.take.w <= 0.0F ||
      layout.decline.w <= 0.0F || layout.header.w <= 0.0F ||
      layout.date.w <= 0.0F) {
    NovaLog::Todo("Mission BBS DITL 0x3ee is missing a required control rect");
    return std::nullopt;
  }
  return layout;
}

// @port 0x00441620 65% ui,rendering
// @port 0x004A1290 80% ui
// Ghidra 0x004a1290 NovaUi_DrawMissionBbsActionButtons: DITL entries 1/7 are
// drawn inline below with the shared three-state art and STR# 0x96 labels.
void DrawMissionBbsContents(SdlPlatform &platform,
                            NovaFontCache &font_cache,
                            const ServicesButtonArt &button_art,
                            const GameState &state,
                            const MissionBbsLayout &layout,
                            const MissionListEvaluation &missions,
                            const NovaListControl &list_control,
                            std::size_t selected,
                            std::string_view status,
                            NovaRgbColor list_text,
                            NovaRgbColor list_background,
                            NovaRgbColor list_hilite) {
  SDL_Renderer *renderer = platform.renderer();
  constexpr SDL_Color kText{255, 255, 255, 255};
  constexpr SDL_Color kMissionDim{192, 192, 192, 255};
  // Rows come from the c.lr list palette via NovaUi_DrawListRowCallback
  // (0x00448a30): list_background/list_hilite fill the row and list_text is
  // the label. The title/description panels keep the fill+InvertRect black.
  constexpr SDL_Color kPanelBlack{0, 0, 0, 255};
  // Window furniture colours (Settings_InitColors 0x004ad7c0, triples
  // consumed by NovaUi_DrawMissionBbsWindow 0x00441620): the heading
  // band uses the 0xc000 grey DAT_00733b50, the date the 0x4000 grey
  // DAT_00733b5c, and the selected title white PTR_DAT_00575ad8.
  constexpr SDL_Color kHeadingGrey{192, 192, 192, 255};
  constexpr SDL_Color kDateGrey{64, 64, 64, 255};
  // The selected-mission title panel uses the Times 18 face
  // (DrawContext_SetFontId of the "Times" family + StoreScaledValue 0x12).
  constexpr float kMissionTitleFontSize = 18.0F;

  const auto &rows = missions.page_zero;
  const std::string heading =
      // 0x167 is the original's 1-based entry -> "The following missions are
      // available here:". The heading cursor helper (FUN_00874124) bakes in a
      // +12px baseline offset from the entry-8 rect top.
      NovaHud_LoadStringEntry(0x7d2, 0x167)
          .value_or("The following missions are available here");
  NovaText_Draw(platform,
                font_cache,
                NovaFontFamily::kGeneva,
                kMissionListFontSize,
                kNovaFontStyleRegular,
                kHeadingGrey,
                layout.header.x,
                layout.header.y + 12.0F,
                heading);
  if (!rows.empty()) {
    const SDL_Rect list_clip{static_cast<int>(list_control.content_rect().x),
                             static_cast<int>(list_control.content_rect().y),
                             static_cast<int>(list_control.content_rect().w),
                             static_cast<int>(list_control.content_rect().h)};
    SDL_SetRenderClipRect(renderer, &list_clip);
    const SDL_FRect content = list_control.content_rect();
    for (std::size_t row = 0; row < rows.size(); ++row) {
      const SDL_FRect row_rect = list_control.RowRect(row);
      if (row_rect.y >= content.y + content.h ||
          row_rect.y + row_rect.h <= content.y) {
        continue;
      }
      const auto mission_id = rows[row];
      const auto *definition =
          state.scenario.Mission(static_cast<std::int16_t>(mission_id + 0x80));
      const std::string label =
          definition != nullptr && !definition->display_name.empty()
              ? Mission_ExpandMissionWildcards(
                    state, definition->display_name, true, rows[row])
              : "Mission " + std::to_string(mission_id);
      // NovaUi_DrawListRowCallback (0x00448a30) shared with the mission
      // computer: c.lr fill + Geneva-9 label at the row baseline.
      NovaUi_DrawListRow(platform,
                         font_cache,
                         row_rect,
                         label,
                         row == selected,
                         list_text,
                         list_background,
                         list_hilite);
    }
    SDL_SetRenderClipRect(renderer, nullptr);
  }
  // Native list scrollbar (FUN_004d2010) in the DITL 0x3ee UiPanel entry-3
  // strip; hidden while the whole list fits the view.
  NovaUi_DrawListScrollbar(platform, list_control, layout.scrollbar);

  // Selected-title panel (entry 5): with no selection the original fills the
  // rect black (0x00441620's g_selected_misn_slot_index == -1 arm); with a
  // selection it draws the wildcard-expanded mission name in white Times 18,
  // single line, baseline at the rect top + 18 (FUN_00872560 cursor model).
  if (layout.selected_title.w > 0.0F) {
    SDL_SetRenderDrawColor(
        renderer, kPanelBlack.r, kPanelBlack.g, kPanelBlack.b, kPanelBlack.a);
    SDL_RenderFillRect(renderer, &layout.selected_title);
  }
  if (layout.selected_title.w > 0.0F && !rows.empty() &&
      selected < rows.size()) {
    const auto *definition = state.scenario.Mission(
        static_cast<std::int16_t>(rows[selected] + 0x80));
    const std::string title =
        definition != nullptr
            ? Mission_ExpandMissionWildcards(
                  state, definition->display_name, true, rows[selected])
            : "Mission " + std::to_string(rows[selected]);
    NovaText_Draw(platform,
                  font_cache,
                  NovaFontFamily::kTimes,
                  kMissionTitleFontSize,
                  kNovaFontStyleRegular,
                  kText,
                  layout.selected_title.x,
                  layout.selected_title.y + kMissionTitleFontSize,
                  title);
  }

  // DITL items 1 and 7 are the small action buttons (0x004a1290); item 3 is
  // the list scrollbar, not a button.
  const std::array<std::pair<SDL_FRect, std::string>, 2> bbs_buttons{
      {{layout.take, NovaHud_LoadStringEntry(0x96, 26).value_or("Accept")},
       {layout.decline, NovaHud_LoadStringEntry(0x96, 1).value_or("Leave")}}};
  for (const auto &[rect, label] : bbs_buttons) {
    if (rect.w <= 0.0F) {
      continue;
    }
    button_art.Draw(platform, rect, ButtonState::kNormal);
    DrawThreeStateButtonLabel(platform, font_cache, rect, label, kText);
  }
  if (layout.date.w > 0.0F) {
    // 0x00441620 sets the date cursor to the entry-11 rect's bottom-left and
    // draws it left-aligned; the rect bottom matches the heading's top+12
    // baseline (both 15 in DITL 0x3ee), so the lines align. 0x4000 grey.
    NovaText_Draw(platform,
                  font_cache,
                  NovaFontFamily::kGeneva,
                  kMissionListFontSize,
                  kNovaFontStyleRegular,
                  kDateGrey,
                  layout.date.x,
                  layout.date.y + layout.date.h,
                  NovaText_FormatDateString(
                      state.date, true, state.date_prefix, state.date_suffix));
  }
  if (!status.empty()) {
    NovaText_DrawCentered(platform,
                          font_cache,
                          NovaFontFamily::kGeneva,
                          11.0F,
                          kNovaFontStyleRegular,
                          kMissionDim,
                          layout.description.x,
                          layout.description.x + layout.description.w,
                          layout.description.y + layout.description.h + 14.0F,
                          status);
  }
  if (layout.description.w > 0.0F) {
    // Description panel (entry 4, 0x00441620): the selected mission's desc
    // (misn id + 4000) drawn with the fill + InvertRect cancel pattern — net
    // look is a BLACK panel with white left-aligned wrapped Geneva-9 text,
    // baseline at rect.top + 9, wrapping across the full rect width. With no
    // selection the original fills the rect black directly.
    SDL_SetRenderDrawColor(
        renderer, kPanelBlack.r, kPanelBlack.g, kPanelBlack.b, kPanelBlack.a);
    SDL_RenderFillRect(renderer, &layout.description);
    if (!rows.empty() && selected < rows.size()) {
      if (const auto description = NovaResource_LoadDescription(
              static_cast<std::uint16_t>(rows[selected] + 4000));
          description && !description->text.empty()) {
        // The original loads the desc into the shared scratch (running the
        // placeholder pass at load, 0x004c6d50) and runs the same wildcard
        // pass as the list rows (NovaUi_RunMissionBbsWindow
        // 0x0043c470 -> Stellar_BuildTravelDestinationDescription).
        std::string loaded_text = description->text;
        Mission_ExpandStringPlaceholders(state, loaded_text);
        const std::string expanded_text = Mission_ExpandMissionWildcards(
            state, loaded_text, true, rows[selected]);
        const auto lines = WrapDescriptionLines(
            expanded_text,
            static_cast<int>(std::max(1.0F, layout.description.w)),
            [&](std::string_view line) {
              return font_cache.TextWidth(NovaFontFamily::kGeneva,
                                          kMissionListFontSize,
                                          kNovaFontStyleRegular,
                                          line);
            });
        const SDL_Rect desc_clip{static_cast<int>(layout.description.x),
                                 static_cast<int>(layout.description.y),
                                 static_cast<int>(layout.description.w),
                                 static_cast<int>(layout.description.h)};
        SDL_SetRenderClipRect(renderer, &desc_clip);
        const float line_pitch = static_cast<float>(font_cache.LineHeight(
            NovaFontFamily::kGeneva, kMissionListFontSize));
        float y = layout.description.y + kMissionListFontSize;
        for (const auto &line : lines) {
          if (y > layout.description.y + layout.description.h) {
            break;
          }
          NovaText_Draw(platform,
                        font_cache,
                        NovaFontFamily::kGeneva,
                        kMissionListFontSize,
                        kNovaFontStyleRegular,
                        kText,
                        layout.description.x,
                        y,
                        line);
          y += line_pitch;
        }
        SDL_SetRenderClipRect(renderer, nullptr);
      }
    }
  }
}

void DrawMissionBbsBase(SdlPlatform &platform,
                        const std::function<void()> &render_background,
                        SDL_Texture *backdrop,
                        SDL_Texture *frame,
                        const MissionBbsLayout &layout) {
  SDL_Renderer *renderer = platform.renderer();
  if (render_background) {
    // Re-render the preserved docked menu and layer the BBS window on top
    // (deliberate divergence, see docs/dlog_ditl_dialog_format.md).
    render_background();
  } else {
    platform.SetPlacement(PlaceWindow(platform.logical_playfield_size()));
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);
  }
  platform.SetPlacement(PlaceContained({layout.frame.w, layout.frame.h},
                                       platform.logical_playfield_size()));
  if (render_background == nullptr && backdrop != nullptr) {
    float width = 0.0F;
    float height = 0.0F;
    SDL_GetTextureSize(backdrop, &width, &height);
    const SDL_FRect backdrop_rect{(layout.frame.w - width) / 2.0F,
                                  (layout.frame.h - height) / 2.0F,
                                  width,
                                  height};
    SDL_RenderTexture(renderer, backdrop, nullptr, &backdrop_rect);
  }
  if (frame != nullptr) {
    SDL_RenderTexture(renderer, frame, nullptr, &layout.frame);
  } else {
    SDL_SetRenderDrawColor(renderer, 16, 40, 72, SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(renderer, &layout.frame);
    SDL_SetRenderDrawColor(renderer, 80, 140, 190, SDL_ALPHA_OPAQUE);
    SDL_RenderRect(renderer, &layout.frame);
  }
  // The original leaves the travel scene behind DLOG 0x3ee and paints the
  // 0x2139 frame into the dialog bounds. Keep the frame's native dimensions;
  // stretching it into the 640x480 landed canvas was the source of the old
  // clipped/offset appearance.
}

// Acceptance UI chain shared by every accept path; defined below.
void NovaMission_RunAcceptanceDialogs(
    SdlPlatform &platform,
    GameState &state,
    std::int16_t mission_def,
    const std::function<void()> &render_background);

// Binds the acceptance-dialog sink. Mission_ActivateAtSlot invokes it for the
// mission it activated (and recurses into it for script-S activations, so a
// nested mission's readers precede the outer mission's, as in the original);
// Mission_ClearMisnSlotAssignments forwards it so an abort's on-abort payload
// can present the readers for any mission it starts.
[[nodiscard]] MissionAcceptanceSink
MakeAcceptanceSink(SdlPlatform &platform,
                   GameState &state,
                   const std::function<void()> &render_background) {
  return [&platform, &state, &render_background](std::int16_t mission_def) {
    NovaMission_RunAcceptanceDialogs(
        platform, state, mission_def, render_background);
  };
}

// Resolves a mission's flags-0x100 destination into a zero-based system id for
// the nested starmap preselect, or -1 when the mission does not request one.
// `travel_stellar`/`return_stellar` are the resolved stellar ids
// (ScenarioData::mission_target_resolutions for an offered mission; the active
// slot's fields for an active one). Ghidra 0x0043c470 action 6 / 0x00442510
// action 4: prefer TravelStel, else ReturnStel.
[[nodiscard]] std::int16_t
MissionDestinationPreselect(const GameState &state,
                            std::uint16_t flags_primary,
                            std::int16_t travel_stellar,
                            std::int16_t return_stellar) {
  if ((flags_primary & 0x0100U) == 0U) {
    return -1;
  }
  std::int16_t stellar = travel_stellar;
  if (stellar == -1) {
    stellar = return_stellar;
  }
  if (stellar < 0 ||
      static_cast<std::size_t>(stellar) >= state.scenario.stellars.size()) {
    return -1;
  }
  return state.scenario.stellars[static_cast<std::size_t>(stellar)].system_id;
}

// Opens the nested starmap over the current background, saving and restoring
// the player's travel selection around it (Ghidra NovaUi_RunMissionBbsWindow
// 0x0043c470 action 6, NovaUi_RunMissionOfferWindow 0x00442510
// action 4, NovaUi_RunMissionComputerWindow 0x00446150: g_ship_states->
// ai_secondary_target_slot + travel_transfer_mode). The map-selected
// destination is plotted so the HUD keeps the planned jump. Returns true when
// the app is quitting.
[[nodiscard]] bool RunNestedMissionStarmap(SdlPlatform &platform,
                                           GameState &state,
                                           std::int16_t preselect_system) {
  const std::int16_t saved_target = state.player.ai_secondary_target_slot;
  const std::int16_t saved_mode = state.player.travel_transfer_mode;
  const StarmapResult map_result =
      NovaStarmap_RunWindow(platform, state, preselect_system);
  state.player.ai_secondary_target_slot = saved_target;
  state.player.travel_transfer_mode = saved_mode;
  if (map_result.exit == StarmapExit::kQuit) {
    return true;
  }
  if (map_result.destination_system_id >= 0) {
    NovaTravel_PlotStarmapDestination(state, map_result.destination_system_id);
  }
  NovaUi_MarkTravelAndStatusPanelsDirty(state);
  return false;
}

// True when at least one active mission shows in the mission list (its
// flags_at_accept lacks the 0x400 "hidden" bit). The original gates the
// mission-computer sub-window on this count (0x0043c470 action 10, 0x00442510
// action 7, and the in-flight command 0x28).
[[nodiscard]] bool HasVisibleActiveMission(const GameState &state) {
  for (std::size_t slot = 0; slot < state.active_mission_runtime_flags.size();
       ++slot) {
    const auto &flags = state.active_mission_runtime_flags[slot];
    if (flags.is_active && (flags.flags_primary_at_accept & 0x400) == 0U) {
      return true;
    }
  }
  return false;
}

} // namespace

// @port 0x0043C470 78% ui,gameplay
// @port 0x004A1130 70% ui
// Ghidra 0x004a1130 NovaUi_HitTestMissionBbsActionButtons: the Accept (DITL
// entry 1) / Leave (entry 7) hit-test runs inline in the mouse arm below.
// Ghidra 0x0043c470 NovaUi_RunMissionBbsWindow (partial port of the landed
// Mission BBS: layout, list/description rendering, selection, accept). The
// 0x00440c90 NovaUi_PollMissionBbsWindow selection/navigation slice runs
// inline in the input loop below.
LandedExit RunMissionBbsWindow(SdlPlatform &platform,
                               SdlAudio &audio,
                               GameState &state,
                               std::int16_t stellar_id,
                               const std::function<void()> &render_background) {
  SdlPlatform::ScopedPlacement placement_guard(platform,
                                               platform.current_placement());
  // Ghidra 0x0043c470 creates the shared mission-interaction window handle
  // (g_mission_interaction_window) for the BBS lifetime; Mission_ActivateAtSlot
  // reads it for the TravelStel predicate.
  MissionInteractionWindowScope interaction_window_scope(state);
  const auto contains = [](const SDL_FRect &rect, SDL_FPoint point) {
    return point.x >= rect.x && point.x < rect.x + rect.w &&
           point.y >= rect.y && point.y < rect.y + rect.h;
  };
  auto backdrop = LoadPictTexture(platform, kDockedBackdropPict);
  auto frame = LoadPictTexture(platform, 0x2139);
  ServicesButtonArt button_art;
  (void)button_art.Initialize(platform);
  NovaFontCache font_cache;
  const auto layout = LayoutMissionBbs();
  const auto ui_style = NovaResource_LoadMainMenuStyle();
  // c.lr list palette read by NovaUi_DrawListRowCallback (0x00448a30); the
  // fallbacks match the shipped white/black/red defaults.
  const NovaRgbColor list_text =
      ui_style ? ui_style->list_text : NovaRgbColor{255, 255, 255};
  const NovaRgbColor list_background =
      ui_style ? ui_style->list_background : NovaRgbColor{0, 0, 0};
  const NovaRgbColor list_hilite =
      ui_style ? ui_style->list_hilite : NovaRgbColor{128, 0, 0};
  if (!layout) {
    return LandedExit::kServiceComplete;
  }
  platform.SetPlacement(PlaceContained({layout->frame.w, layout->frame.h},
                                       platform.logical_playfield_size()));
  ProbeUiAutoClear probe_ui_guard(platform);
  MissionListEvaluation missions = Mission_EvaluateMissionLists(state);
  std::size_t selected = 0;
  std::string status;
  // Native list control (Ghidra 0x004d1a60 / 0x0043cc20): owns the scroll
  // offset and derives the visible row count from the DITL list rect. The
  // original rebuilds it only on window entry
  // (Stellar_RebuildTravelDestinationList), so the row set is fixed for the
  // window's lifetime.
  NovaListControl list_control(
      layout->list, kMissionListRowPitch, missions.page_zero.size());
  const auto move_selection = [&](std::ptrdiff_t delta) {
    if (missions.page_zero.empty()) {
      return;
    }
    const auto count = static_cast<std::ptrdiff_t>(missions.page_zero.size());
    auto row = static_cast<std::ptrdiff_t>(selected) + delta;
    if (row < 0) {
      row = count - 1;
    } else if (row >= count) {
      row = 0;
    }
    selected = static_cast<std::size_t>(row);
    list_control.EnsureVisible(selected);
  };
  // Edge latches for the rebindable sub-window commands (0x00440c90 actions
  // 6/9/10).
  bool starmap_command_was_held = false;
  bool player_info_command_was_held = false;
  bool mission_computer_command_was_held = false;
  // Each available row is addressable by its zero-based template id (matching
  // the `missions.missions.N.template_id` state field) so a probe scenario can
  // click a specific mission instead of relying on the default first row.
  const auto publish_probe_ui = [&]() {
    const auto probe_rect = [&platform](SDL_FRect rect) {
      return platform.current_placement().ToWindowRect(rect);
    };
    std::vector<std::pair<std::string, SDL_FRect>> rects{
        {"window", probe_rect(layout->frame)},
        {"list", probe_rect(layout->list)},
        {"take", probe_rect(layout->take)},
        {"decline", probe_rect(layout->decline)},
        {"description", probe_rect(layout->description)}};
    const SDL_FRect content = list_control.content_rect();
    for (std::size_t row = 0; row < missions.page_zero.size(); ++row) {
      const SDL_FRect row_rect = list_control.RowRect(row);
      if (row_rect.y >= content.y + content.h ||
          row_rect.y + row_rect.h <= content.y) {
        continue;
      }
      const float row_top = std::max(row_rect.y, content.y);
      const float row_bottom =
          std::min(row_rect.y + row_rect.h, content.y + content.h);
      rects.push_back(
          {"mission." + std::to_string(missions.page_zero[row]),
           probe_rect(
               {row_rect.x, row_top, row_rect.w, row_bottom - row_top})});
    }
    platform.PublishProbeUi("mission_bbs", std::move(rects));
  };
  NovaLog::Info(
      "mission BBS opened at stellar {}: {} available rows (first id {})",
      static_cast<int>(stellar_id),
      missions.page_zero.size(),
      missions.page_zero.empty()
          ? -1
          : static_cast<int>(missions.page_zero.front()));

  while (!platform.quit_requested()) {
    // @port 0x00440C90 72% ui
    // Ghidra 0x00440c90 (top of NovaUi_PollMissionBbsWindow): a mission
    // script's 'Q' stages g_pending_overlay_message; while it is non-empty
    // the BBS poll returns action 7 and the run loop leaves without drawing
    // the message here. The launch tail shows and clears it later.
    if (!state.pending_overlay_message.empty()) {
      return LandedExit::kServiceComplete;
    }
    DrawMissionBbsBase(platform,
                       render_background,
                       backdrop ? backdrop->get() : nullptr,
                       frame ? frame->get() : nullptr,
                       *layout);
    DrawMissionBbsContents(platform,
                           font_cache,
                           button_art,
                           state,
                           *layout,
                           missions,
                           list_control,
                           selected,
                           status,
                           list_text,
                           list_background,
                           list_hilite);
    publish_probe_ui();
    platform.Present();

    // Ghidra 0x0043c470: after a successful Mission_ActivateMissionAtSlot
    // (whose acceptance UI runs synchronously) the original sets the
    // window-exit flag, so the BBS closes. It never rebuilds the list here; the
    // previous re-evaluation rerolled every definition's random destination.
    auto accept = [&]() -> bool {
      if (missions.page_zero.empty() || selected >= missions.page_zero.size()) {
        return false;
      }
      const auto mission_id = missions.page_zero[selected];
      if (Mission_ActivateAtSlot(
              state,
              mission_id,
              MakeAcceptanceSink(platform, state, render_background))) {
        return true;
      }
      status = "Mission could not be accepted";
      return false;
    };

    for (std::optional<TextInput> input; (input = platform.PollTextEvent());) {
      if (input->key == TextKey::escape) {
        return LandedExit::kServiceComplete;
      }
      if (input->key == TextKey::enter) {
        if (accept()) {
          return LandedExit::kServiceComplete;
        }
        continue;
      }
      if (input->key == TextKey::primary) {
        const SDL_FPoint point = platform.mouse_position();
        bool handled = false;
        // Scrollbar arm of NovaList_HitTestPoint (0x004d1db0): the arrow caps
        // step one row and a click above/below the thumb pages by the visible
        // row count. A click on the thumb itself is a no-op -- the original
        // has no drag tracking.
        switch (
            NovaListScrollbarHitTest(list_control, layout->scrollbar, point)) {
        case NovaListScrollbarPart::kUp:
          list_control.ScrollRows(-1);
          handled = true;
          break;
        case NovaListScrollbarPart::kDown:
          list_control.ScrollRows(1);
          handled = true;
          break;
        case NovaListScrollbarPart::kPageUp:
          list_control.ScrollRows(-list_control.visible_rows());
          handled = true;
          break;
        case NovaListScrollbarPart::kPageDown:
          list_control.ScrollRows(list_control.visible_rows());
          handled = true;
          break;
        case NovaListScrollbarPart::kThumb:
        case NovaListScrollbarPart::kNone:
          break;
        }
        // Content arm: (y - list.top) / pitch + scroll offset.
        if (!handled && contains(layout->list, point)) {
          if (const auto row = list_control.RowAt(point)) {
            selected = *row;
            handled = true;
          }
        }
        // Accept = window entry 1, Leave = entry 7 (0x004a1130).
        if (!handled && contains(layout->take, point)) {
          if (accept()) {
            return LandedExit::kServiceComplete;
          }
        } else if (!handled && contains(layout->decline, point)) {
          return LandedExit::kServiceComplete;
        }
        continue;
      }
      if (input->key == TextKey::physical) {
        // Mac list navigation (0x00440c90: key codes 9/10/11 = Tab/Down/Up),
        // mapped through the port's normalized key codes.
        if (input->key_code == 0x0f) { // Tab: next row
          move_selection(1);
        } else if (input->key_code == 0x66) { // Down
          move_selection(1);
        } else if (input->key_code == 0x61) { // Up
          move_selection(-1);
        }
        continue;
      }
      if (input->key != TextKey::character) {
        continue;
      }
      const char key = static_cast<char>(
          std::tolower(static_cast<unsigned char>(input->character)));
      if (key == 'l' || key == 'q') {
        return LandedExit::kServiceComplete;
      }
      if (key == 'j') {
        move_selection(1);
      } else if (key == 'k') {
        move_selection(-1);
      } else if (key == 'a') {
        if (accept()) {
          return LandedExit::kServiceComplete;
        }
      }
    }

    // Ghidra 0x00440c90 actions 6/9/10: the travel window's command channels
    // open the starmap, the Player Info window, and the mission computer over
    // the BBS. The mission computer only opens when a visible active mission
    // exists (the 0x0043c470 action-10 count).
    const bool starmap_held =
        NovaInput_IsCommandActive(platform, kStarmapCommand);
    if (starmap_held && !starmap_command_was_held) {
      std::int16_t preselect = -1;
      if (!missions.page_zero.empty() && selected < missions.page_zero.size()) {
        const auto mission_id = missions.page_zero[selected];
        const auto *definition = state.scenario.Mission(
            static_cast<std::int16_t>(mission_id + 0x80));
        if (definition != nullptr) {
          const auto &target =
              state.mission_target_resolutions[static_cast<std::size_t>(
                  mission_id)];
          preselect = MissionDestinationPreselect(state,
                                                  definition->flags_primary,
                                                  target.travel_stellar_id,
                                                  target.return_stellar_id);
        }
      }
      if (RunNestedMissionStarmap(platform, state, preselect)) {
        return LandedExit::kQuit;
      }
      // Ghidra 0x0043c470 action 6 does not rebuild the list (and no time
      // advances while the modal map is open). The port previously
      // re-evaluated here, which rerolled every definition's random
      // destination.
    }
    starmap_command_was_held = starmap_held;
    const bool player_info_held =
        NovaInput_IsCommandActive(platform, kPlayerInfoCommand);
    if (player_info_held && !player_info_command_was_held) {
      (void)NovaPlayerInfo_RunWindow(platform, state, render_background);
    }
    player_info_command_was_held = player_info_held;
    const bool mission_computer_held =
        NovaInput_IsCommandActive(platform, kMissionComputerCommand);
    if (mission_computer_held && !mission_computer_command_was_held &&
        HasVisibleActiveMission(state)) {
      NovaMission_RunMissionInfoWindow(
          platform, audio, state, render_background);
    }
    mission_computer_command_was_held = mission_computer_held;
    platform.PaceFrame();
  }
  return LandedExit::kQuit;
}

// ---------------------------------------------------------------------------
// @port 0x00442510 90% ui,rendering
// Ghidra 0x00442510 NovaUi_RunMissionOfferWindow. The 0x00447170
// NovaUi_PollMissionOfferWindow input slice and 0x00447680
// NovaUi_DrawMissionOfferWindow draw slice run inline in this function and its
// draw_frame lambda below. Window DLOG 0x3f8
// (DITL 1016: entry 1 = accept button, entry 2 = decline button, entry 3 =
// read-only text view, entries 9/10 = scroll arrows); background = PICT 0x214a
// (main art, top-anchored) with strips 0x2149 (top) and 0x214b (bottom). When
// the dësc variant is >= 0x80 the original switches to DLOG 0x3fc (DITL 1020)
// with single backdrop PICT 0x2150 and blits the variant PICT into entry 8.
// Flags 0x0004 ("can't refuse"): both poll actions activate the mission and
// 0x004a1820 paints one accept button from DITL entry 6 with the decline slot
// suppressed (0x004a1670 leaves the decline rect unhittable). The port mirrors
// that by drawing/hit-testing entry 6 and omitting decline.
// Button captions come from the mïsn
// payload +0x75f/+0x77f, which the original truncates at the first
// non-lowercase byte and replaces with STR# 0x96 entries 0x32/0x1b (accept) and
// 0x33 (decline) when empty.

namespace {

// Fills the reader text the way the original's callers fill
// g_selection_dialog_text: desc load (which runs the placeholder pass at
// load time, 0x004c6d50) + the wildcard pass. `active_slot >= 0` selects the
// active arm of the wildcard pass (mission_id = active slot).
[[nodiscard]] MissionDialogText LoadMissionText(const GameState &state,
                                                std::uint16_t desc_id,
                                                bool offering_arm,
                                                std::int16_t mission_id) {
  return Mission_LoadSelectionDialogText(
      state, desc_id, offering_arm, mission_id);
}

[[nodiscard]] std::optional<std::size_t>
FindActiveMissionSlot(const GameState &state, std::int16_t mission_def) {
  for (std::size_t slot = 0; slot < state.active_missions.size(); ++slot) {
    if (state.active_mission_runtime_flags[slot].is_active &&
        state.active_missions[slot].mission_template_id == mission_def) {
      return slot;
    }
  }
  return std::nullopt;
}

// Ghidra 0x0043f100 Mission_ActivateMissionAtSlot runs the acceptance UI
// chain after slot population: the Brief dialog (payload +0x34 desc) with
// starmap access, and the LoadCarg dialog (payload +0x38 desc) when
// PickupMode 0 puts cargo on board at accept. The port keeps
// Mission_ActivateAtSlot state-only; the UI layer binds this via
// MakeAcceptanceSink, so the activation and abort paths (mission-offer window,
// Mission BBS, mission computer abort) present it through the same sink, and
// a mission a payload starts with the S opcode presents it too. Note: every
// shipped S-opcode target is a silent helper (BriefText 0, LoadCargText 0),
// so no Nova mïsn exercises the S arm -- only plug-in/synthetic data does
// (tests/mission_test.cpp "acceptance dialogs follow script activation
// order" pins the ordering).
void NovaMission_RunAcceptanceDialogs(
    SdlPlatform &platform,
    GameState &state,
    std::int16_t mission_def,
    const std::function<void()> &render_background) {
  const MissionDef *def =
      state.scenario.Mission(static_cast<std::int16_t>(mission_def + 0x80));
  if (def == nullptr) {
    return;
  }
  const std::optional<std::size_t> slot =
      FindActiveMissionSlot(state, mission_def);
  if (def->initial_briefing_id >= 0x80) {
    const MissionDialogText message =
        LoadMissionText(state,
                        static_cast<std::uint16_t>(def->initial_briefing_id),
                        false,
                        slot ? static_cast<std::int16_t>(*slot) : -1);
    if (!message.text.empty()) {
      NovaUi_RunTextReaderDialog(platform,
                                 state,
                                 message.text,
                                 true,
                                 render_background,
                                 message.dialog_variant);
    }
  }
  if (def->pickup_mode == 0 && def->text_description_ids[2] >= 0x80) {
    const MissionDialogText message = LoadMissionText(
        state,
        static_cast<std::uint16_t>(def->text_description_ids[2]),
        false,
        slot ? static_cast<std::int16_t>(*slot) : -1);
    if (!message.text.empty()) {
      NovaUi_RunTextReaderDialog(platform,
                                 state,
                                 message.text,
                                 false,
                                 render_background,
                                 message.dialog_variant);
    }
  }
}

// Reads a NUL-terminated C string at a mïsn payload offset.
[[nodiscard]] std::string_view PayloadCString(const MissionDef &def,
                                              std::size_t offset) {
  const auto *bytes = def.raw_payload.data();
  std::size_t length = 0;
  while (offset + length < def.raw_payload.size() &&
         std::to_integer<unsigned>(bytes[offset + length]) != 0U) {
    ++length;
  }
  return {reinterpret_cast<const char *>(bytes + offset), length};
}

} // namespace

// @port 0x004A1670 70% ui
// Ghidra 0x004a1670 NovaUi_HitTestMissionOfferButtons: the accept/decline and
// up/down scroll slots are hit-tested inline in the mouse arm below; Flags
// 0x0004 maps the accept slot to DITL entry 6 with decline unhittable.
MissionOfferResult
NovaMission_RunOfferWindow(SdlPlatform &platform,
                           SdlAudio &audio,
                           GameState &state,
                           std::int16_t mission_def,
                           const std::function<void()> &render_background) {
  SdlPlatform::ScopedPlacement placement_guard(platform,
                                               platform.current_placement());
  state.gameplay_now_ms = platform.gameplay_ticks_ms();
  if (mission_def < 0 || mission_def >= 1000) {
    return MissionOfferResult::kDeclined;
  }
  const MissionDef *def =
      state.scenario.Mission(static_cast<std::int16_t>(mission_def + 0x80));
  if (def == nullptr) {
    return MissionOfferResult::kDeclined;
  }
  // g_mission_offer_refusable = (Flags & 4) == 0. The original tests the
  // runtime g_misn_resource_defs entry's flags field; NovaResources_LoadMisn-
  // ResourceDefs (0x0043bbb0) copies the raw mïsn Flags at +0x50 into runtime
  // MisnDef +0x18, so this is MissionDef::flags_primary (raw +0x50), not the
  // raw +0x18 ScanMask. When Flags 0x0004 ("can't refuse") is set and the
  // offer text is empty the original activates the mission without a window.
  const bool normal_arm = (def->flags_primary & 4U) == 0;

  // dësc (def + 4000): the original expands the {g}/{G}/{p}/{b} placeholder
  // blocks at load (Ui_LoadSelectionDialogResource 0x004c6d50 ->
  // Ship_ExpandStringPlaceholders 0x0044a4d0), then the wildcard pass composes
  // the final offer text (Stellar_BuildTravelDestinationDescription).
  std::string text;
  std::int16_t desc_variant = 0;
  const std::uint16_t desc_id = static_cast<std::uint16_t>(mission_def + 4000);
  if (const auto desc = NovaResource_LoadDescription(desc_id)) {
    text = desc->text;
    desc_variant = desc->dialog_variant;
    Mission_ExpandStringPlaceholders(state, text);
    text = Mission_ExpandMissionWildcards(state, text, true, mission_def);
  }
  if (!normal_arm && text.empty()) {
    if (!Mission_ActivateAtSlot(
            state,
            mission_def,
            MakeAcceptanceSink(platform, state, render_background))) {
      return MissionOfferResult::kActivationFailed;
    }
    return MissionOfferResult::kAccepted;
  }

  // Art variant (0x00442510): variant >= 0x80 selects DLOG 0x3fc and the
  // single backdrop PICT 0x2150, and the draw callback (0x00447680) blits the
  // variant PICT into DITL entry 8. Variant < 0x80 uses DLOG 0x3f8 with the
  // 0x2149/0x214a/0x214b strips. g_selection_dialog_variant is an unsigned
  // 16-bit PICT id, so the comparison is unsigned.
  const bool art_variant_mode =
      static_cast<std::uint16_t>(desc_variant) >= 0x80;
  const auto dlog =
      NovaResource_LoadDialogDefinition(art_variant_mode ? 0x3fc : 0x3f8);
  const auto items =
      dlog ? NovaResource_LoadDialogItems(dlog->dialog_item_list_id)
           : std::nullopt;
  if (!dlog || !items) {
    // The original bails with return 0 (declined) when the window resource is
    // unusable.
    NovaLog::Todo("mission offer DLOG/DITL {:#x} unavailable; declining offer "
                  "for misn def {}",
                  art_variant_mode ? 0x3fc : 0x3f8,
                  mission_def);
    return MissionOfferResult::kDeclined;
  }
  // Ghidra 0x00442510 creates the shared mission-interaction window handle
  // (g_mission_interaction_window) here; the can't-refuse direct-activation arm
  // above runs before window creation and therefore sees the ambient scope.
  MissionInteractionWindowScope interaction_window_scope(state);

  const float win_w = static_cast<float>(dlog->right - dlog->left);
  const float win_h = static_cast<float>(dlog->bottom - dlog->top);
  const SDL_FPoint origin{0.0F, 0.0F};
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
  const SDL_FRect accept_rect = item_rect(0);  // UiPanel entry 1
  const SDL_FRect decline_rect = item_rect(1); // UiPanel entry 2
  // Entry 6 (DITL item 5): single accept button in the Flags 0x0004 arm
  // (0x004a1820 reads UiPanel entry 6; 0x004a1670 hit-tests it as slot 0).
  const SDL_FRect cannot_refuse_rect = item_rect(5);
  // The can't-refuse arm moves the accept button to entry 6 and drops
  // decline, so this is the rect the draw, probe registry, and hit-test use.
  const SDL_FRect accept_button_rect =
      normal_arm ? accept_rect : cannot_refuse_rect;
  const SDL_FRect text_rect = item_rect(2); // UiPanel entry 3: text view
  // Entries 9/10 (DITL items 8/9) are the text-view scroll arrows. Entry 9 is
  // label 0x12 (STR# 0x96 entry 19 '^' = up) and entry 10 label 0x13 (entry 20
  // '&' = down) per 0x004a1820. Actions 9/10 pass +/-10 as a content
  // translation
  // (+ = up); this port's ScrollBy offset grows downward, so up maps to item 8
  // and down to item 9.
  const SDL_FRect scroll_up_rect = item_rect(8);
  const SDL_FRect scroll_down_rect = item_rect(9);
  // Entry 8 (DITL item 7) is the variant PICT target in the 0x3fc arm
  // (0x00447680: UiPanel_GetEntryInfo(window, 8)).
  const SDL_FRect variant_rect = item_rect(7);

  platform.SetPlacement(
      PlaceContained({win_w, win_h}, platform.logical_playfield_size()));

  // Publish the window's control rects to the probe harness (window-point
  // space) so the harness can click by intent; cleared when this modal exits.
  ProbeUiAutoClear probe_ui_guard(platform);
  const auto publish_probe_ui = [&]() {
    const auto probe_rect = [&](SDL_FRect rect) {
      return platform.current_placement().ToWindowRect(rect);
    };
    // The active accept rect moves to entry 6 in the can't-refuse arm; the
    // decline slot does not exist there, so it is not published.
    std::vector<std::pair<std::string, SDL_FRect>> rects = {
        {"window", probe_rect({0.0F, 0.0F, win_w, win_h})},
        {"accept", probe_rect(accept_button_rect)},
        {"text", probe_rect(text_rect)},
        {"scroll_up", probe_rect(scroll_up_rect)},
        {"scroll_down", probe_rect(scroll_down_rect)}};
    if (normal_arm) {
      rects.emplace_back("decline", probe_rect(decline_rect));
    }
    platform.PublishProbeUi("mission_offer", std::move(rects));
  };

  // Button captions: payload +0x75f/+0x77f C-strings. 0x00442510's
  // normalisation loop increments its index by 0x100, so it walks the two
  // 256-byte caption buffers rather than the characters and only inspects each
  // caption's first byte: when that byte is not an ASCII letter (checked via
  // MWRuntime_ToLower, so either case passes) the whole caption is blanked and
  // the STR# 0x96 default is used instead. The front letter is also the
  // keyboard mnemonic.
  const auto payload_caption = [&](std::size_t offset) {
    const std::string_view caption = PayloadCString(*def, offset);
    if (caption.empty()) {
      return std::string{};
    }
    const auto lower = [](unsigned char c) { return std::tolower(c); };
    const unsigned char first = static_cast<unsigned char>(caption.front());
    if (lower(first) < 'a' || lower(first) > 'z') {
      return std::string{};
    }
    return std::string{caption};
  };
  std::string accept_caption = payload_caption(0x75f);
  if (accept_caption.empty()) {
    accept_caption = NovaHud_LoadStringEntry(0x96, normal_arm ? 0x32 : 0x1b)
                         .value_or(normal_arm ? "Yes" : "Okay");
  }
  std::string decline_caption = payload_caption(0x77f);
  if (decline_caption.empty()) {
    decline_caption = NovaHud_LoadStringEntry(0x96, 0x33).value_or("No");
  }

  auto backdrop = LoadPictTexture(platform, kDockedBackdropPict);
  // Variant < 0x80: main 0x214a + top strip 0x2149 + bottom strip 0x214b.
  // Variant >= 0x80: single backdrop 0x2150 + the variant PICT into entry 8.
  std::unique_ptr<SdlTexture> art_main;
  std::unique_ptr<SdlTexture> art_top;
  std::unique_ptr<SdlTexture> art_bottom;
  std::unique_ptr<SdlTexture> art_variant;
  std::unique_ptr<SdlTexture> art_variant_pict;
  if (art_variant_mode) {
    art_variant = LoadPictTexture(platform, 0x2150);
    art_variant_pict =
        LoadPictTexture(platform, static_cast<std::uint16_t>(desc_variant));
    if (art_variant == nullptr) {
      NovaLog::Todo("mission offer art backdrop PICT 0x2150 missing; using a "
                    "flat window fill");
    }
    if (art_variant_pict == nullptr) {
      NovaLog::Todo("mission offer variant PICT {:#x} missing; entry 8 stays "
                    "unpainted",
                    static_cast<std::uint16_t>(desc_variant));
    }
  } else {
    art_main = LoadPictTexture(platform, 0x214a);
    art_top = LoadPictTexture(platform, 0x2149);
    art_bottom = LoadPictTexture(platform, 0x214b);
    if (art_main == nullptr || art_top == nullptr || art_bottom == nullptr) {
      NovaLog::Todo("mission offer window art PICTs 0x2149/0x214a/0x214b "
                    "incomplete; using a flat window fill");
    }
  }
  ServicesButtonArt button_art;
  (void)button_art.Initialize(platform);
  NovaFontCache font_cache;
  // Shared read-only text view (NovaTextView 0x004bcd90) over DITL entry 3.
  NovaTextScrollView view(font_cache, text, text_rect);
  NovaTextScrollHold scroll_hold;
  // 0x00442510's accept arm (action 1): activate, then
  // Mission_ActivateMissionAtSlot (0x0043f100) shows the Brief/LoadCarg
  // dialogs inline via the sink. Shared by the accept-button click and Return.
  const auto accept_offer = [&]() {
    if (!Mission_ActivateAtSlot(
            state,
            mission_def,
            MakeAcceptanceSink(platform, state, render_background))) {
      return MissionOfferResult::kActivationFailed;
    }
    return MissionOfferResult::kAccepted;
  };
  // 0x00442510's decline arm (action 2): the payload +0x58 desc
  // (slot_aux_text) opens the text reader when present, then the decline
  // reaction script (payload +0x25a) runs either way. Text composition mirrors
  // the original (Ui_LoadSelectionDialogResource placeholder pass +
  // Stellar_BuildTravelDestinationDescription('\0', -1)): the active-arm
  // wildcard pass against slot -1, so mission-specific tokens fall back to
  // their [Error] sentinels while player tokens still resolve. Shared by the
  // decline-button click and the caption mnemonic.
  const auto decline_offer = [&]() {
    if (def->slot_aux_text_id >= 0x80) {
      const MissionDialogText followup = LoadMissionText(
          state, static_cast<std::uint16_t>(def->slot_aux_text_id), false, -1);
      if (!followup.text.empty()) {
        NovaUi_RunTextReaderDialog(platform,
                                   state,
                                   followup.text,
                                   false,
                                   render_background,
                                   followup.dialog_variant);
      }
    }
    Mission_ExecuteReactionScript(
        state,
        PayloadCString(*def, 0x25a),
        MissionScriptContext{"offer decline"},
        MakeAcceptanceSink(platform, state, render_background));
    return MissionOfferResult::kDeclined;
  };
  // @port 0x00447680 68% ui,rendering
  // One window frame over the docked backing store. The original's draw
  // callback (NovaUi_DrawMissionOfferWindow 0x00447680) fills the
  // window, blits the main art top-anchored (clipped), then the top and
  // bottom strips.
  auto draw_frame = [&]() {
    platform.SetPlacement(PlaceWindow(platform.logical_playfield_size()));
    SDL_SetRenderDrawColor(platform.renderer(), 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(platform.renderer());
    if (render_background) {
      // Re-render the preserved docked menu and layer the offer window on
      // top (deliberate divergence, see docs/dlog_ditl_dialog_format.md).
      render_background();
    }
    if (render_background == nullptr && backdrop != nullptr) {
      DrawContainedPict(platform, backdrop->get());
    }
    platform.SetPlacement(
        PlaceContained({win_w, win_h}, platform.logical_playfield_size()));
    const SDL_FRect window{origin.x, origin.y, win_w, win_h};
    SDL_SetRenderDrawColor(platform.renderer(), 16, 40, 72, SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(platform.renderer(), &window);
    if (art_variant_mode) {
      // Single art backdrop 0x2150 filling the window, then the variant PICT
      // into entry 8 (0x00447680).
      if (art_variant != nullptr) {
        float w = 0.0F;
        float h = 0.0F;
        SDL_GetTextureSize(art_variant->get(), &w, &h);
        const SDL_FRect variant_backdrop{
            origin.x, origin.y, std::min(w, win_w), std::min(h, win_h)};
        SDL_RenderTexture(platform.renderer(),
                          art_variant->get(),
                          nullptr,
                          &variant_backdrop);
      }
      if (art_variant_pict != nullptr) {
        SDL_RenderTexture(platform.renderer(),
                          art_variant_pict->get(),
                          nullptr,
                          &variant_rect);
      }
    } else {
      if (art_main != nullptr) {
        float w = 0.0F;
        float h = 0.0F;
        SDL_GetTextureSize(art_main->get(), &w, &h);
        const SDL_FRect main_rect{
            origin.x, origin.y, std::min(w, win_w), std::min(h, win_h)};
        SDL_RenderTexture(
            platform.renderer(), art_main->get(), nullptr, &main_rect);
      }
      if (art_top != nullptr) {
        float w = 0.0F;
        float h = 0.0F;
        SDL_GetTextureSize(art_top->get(), &w, &h);
        const SDL_FRect top_rect{
            origin.x, origin.y, std::min(w, win_w), std::min(h, win_h)};
        SDL_RenderTexture(
            platform.renderer(), art_top->get(), nullptr, &top_rect);
      }
      if (art_bottom != nullptr) {
        float w = 0.0F;
        float h = 0.0F;
        SDL_GetTextureSize(art_bottom->get(), &w, &h);
        const SDL_FRect bottom_rect{origin.x,
                                    origin.y + win_h - std::min(h, win_h),
                                    std::min(w, win_w),
                                    std::min(h, win_h)};
        SDL_RenderTexture(
            platform.renderer(), art_bottom->get(), nullptr, &bottom_rect);
      }
    }

    // Text view: dark fill + wrapped offer text, scrolled inside a clip to
    // the view rect; the arrow buttons are runtime-drawn (0x004a1820).
    view.Draw(platform);
    // 0x004a1820 gives both arrow states 0xfffe (not drawn) when neither
    // direction can scroll; only show the pair when the view actually scrolls.
    if (view.scroll_offset() > 0.0F ||
        view.scroll_offset() < view.max_scroll()) {
      NovaUi_DrawScrollArrow(platform,
                             button_art,
                             scroll_up_rect,
                             true,
                             view.scroll_offset() > 0.0F);
      NovaUi_DrawScrollArrow(platform,
                             button_art,
                             scroll_down_rect,
                             false,
                             view.scroll_offset() < view.max_scroll());
    }

    // Flags 0x0004 arm: one accept button at entry 6, decline suppressed
    // (0x004a1820).
    button_art.Draw(platform, accept_button_rect, ButtonState::kNormal);
    DrawThreeStateButtonLabel(platform,
                              font_cache,
                              accept_button_rect,
                              accept_caption,
                              SDL_Color{255, 255, 255, 255});
    if (normal_arm) {
      button_art.Draw(platform, decline_rect, ButtonState::kNormal);
      DrawThreeStateButtonLabel(platform,
                                font_cache,
                                decline_rect,
                                decline_caption,
                                SDL_Color{255, 255, 255, 255});
    }
    publish_probe_ui();
    platform.Present();
  };

  draw_frame();
  // Edge latches for the rebindable sub-window commands of the offer window
  // (0x00442510 actions 4/5/7).
  bool starmap_command_was_held = false;
  bool player_info_command_was_held = false;
  bool mission_computer_command_was_held = false;
  while (!platform.quit_requested()) {
    for (std::optional<TextInput> input; (input = platform.PollTextEvent());) {
      // 0x00447170 has no Esc arm: unlike the BBS poller (0x00440c90, which
      // maps 0x1b to exit) the offer window closes only through its buttons,
      // Return, or the caption mnemonic, so Esc is deliberately ignored.
      if (input->key == TextKey::character) {
        // @port 0x00447170 85% ui
        // Ghidra 0x00447170 key arms: Return (0xd) is action 1 (accept, same
        // in both arms); otherwise the first lower-case letter of a caption is
        // that button's mnemonic. The original honours the mnemonics only when
        // the two captions start with different letters; the decline mnemonic
        // additionally requires the refusable arm.
        const char key = static_cast<char>(
            std::tolower(static_cast<unsigned char>(input->character)));
        const auto first_letter = [](const std::string &caption) {
          return caption.empty()
                     ? '\0'
                     : static_cast<char>(std::tolower(
                           static_cast<unsigned char>(caption.front())));
        };
        const char accept_key = first_letter(accept_caption);
        const char decline_key = first_letter(decline_caption);
        if (accept_key != '\0' && accept_key != decline_key &&
            key == accept_key) {
          return accept_offer();
        }
        if (normal_arm && decline_key != '\0' && accept_key != decline_key &&
            key == decline_key) {
          return decline_offer();
        }
        continue;
      }
      if (input->key == TextKey::enter) {
        // Ghidra 0x00447170 (sVar1 == 3/5, keycode 0xd): Return is action 1
        // (accept), the same in both arms; the can't-refuse arm is unaffected.
        return accept_offer();
      }
      if (input->key == TextKey::primary) {
        const SDL_FPoint point = platform.mouse_position();
        // Action 1: activate. In the can't-refuse arm the same single entry-6
        // button activates and 0x004a1670 never yields action 2.
        if (Contains(accept_button_rect, point)) {
          return accept_offer();
        }
        if (normal_arm && Contains(decline_rect, point)) {
          return decline_offer();
        }
        // Arrow buttons (entries 9/10): Ghidra 0x00447170 enters its
        // hold-to-repeat loop when the press lands on the arrow, gated on the
        // maxed (up) / active (down) latch.
        if (Contains(scroll_up_rect, point)) {
          if (view.can_scroll_up()) {
            scroll_hold.Press(view, true, platform.wall_ticks_ms());
          }
        } else if (Contains(scroll_down_rect, point)) {
          if (view.can_scroll_down()) {
            scroll_hold.Press(view, false, platform.wall_ticks_ms());
          }
        }
        continue;
      }
      if (input->key == TextKey::physical) {
        // Ghidra 0x00447170: Up/Down scroll +/-10px (actions 9/10) and
        // Home/End/PageUp/PageDown jump to an end / move 250px, gated on the
        // maxed/active latches. The old DIK 0xc8/0xd0 codes never matched a
        // TextInput (the normalized Up/Down are 0x61/0x66).
        (void)view.ApplyScrollKey(MapTextScrollKey(input->key_code));
      }
    }

    // Ghidra 0x00442510 actions 4/5/7: the starmap, Player Info window, and
    // mission computer are opened over the offer window. The mission computer
    // only opens when a visible active mission exists (the action-7 count).
    const bool starmap_held =
        NovaInput_IsCommandActive(platform, kStarmapCommand);
    if (starmap_held && !starmap_command_was_held) {
      const auto &target =
          state.mission_target_resolutions[static_cast<std::size_t>(
              mission_def)];
      const std::int16_t preselect =
          MissionDestinationPreselect(state,
                                      def->flags_primary,
                                      target.travel_stellar_id,
                                      target.return_stellar_id);
      if (RunNestedMissionStarmap(platform, state, preselect)) {
        return MissionOfferResult::kDeclined;
      }
    }
    starmap_command_was_held = starmap_held;
    const bool player_info_held =
        NovaInput_IsCommandActive(platform, kPlayerInfoCommand);
    if (player_info_held && !player_info_command_was_held) {
      (void)NovaPlayerInfo_RunWindow(platform, state, render_background);
    }
    player_info_command_was_held = player_info_held;
    const bool mission_computer_held =
        NovaInput_IsCommandActive(platform, kMissionComputerCommand);
    if (mission_computer_held && !mission_computer_command_was_held &&
        HasVisibleActiveMission(state)) {
      NovaMission_RunMissionInfoWindow(
          platform, audio, state, render_background);
    }
    mission_computer_command_was_held = mission_computer_held;
    // Hold-to-repeat pump (0x00447170): 1px per 60Hz tick while the button
    // stays down over an arrow.
    (void)scroll_hold.Update(
        view, platform.PrimaryMouseDown(), platform.wall_ticks_ms());
    draw_frame();
    platform.PaceFrame();
  }
  return MissionOfferResult::kDeclined;
}

namespace {

// ---- Active-missions info window (the in-flight `I` key) -------------------

constexpr std::uint16_t kMissionInfoFramePict = 0x2145;

// One active-mission row: the mission's slot and its composed list text.
struct MissionInfoRow {
  std::int16_t slot = -1;
  std::string text;
  bool failed = false;
};

struct MissionInfoLayout {
  SDL_FRect frame{};
  SDL_FRect list{};
  SDL_FRect header{};
  SDL_FRect description{};
  SDL_FRect abort_button{};
  SDL_FRect done_button{};
  SDL_FRect date{};
};

// DLOG 0x3f4 (471x155, DITL 0x3f4): entry 1 Done button, entry 2 native list,
// entry 3 heading, entry 4 description panel, entry 5 Abort button, entry 7
// date. Entry 6 (item 5) sits offscreen in this dialog and is unused.
[[nodiscard]] std::optional<MissionInfoLayout> LayoutMissionInfo() {
  const auto definition = NovaResource_LoadDialogDefinition(0x3f4);
  const auto items =
      definition ? NovaResource_LoadDialogItems(definition->dialog_item_list_id)
                 : std::nullopt;
  if (!definition || !items) {
    NovaLog::Todo("mission info DLOG/DITL 0x3f4 unavailable");
    return std::nullopt;
  }
  const float width = static_cast<float>(definition->right - definition->left);
  const float height = static_cast<float>(definition->bottom - definition->top);
  const SDL_FPoint origin{0.0F, 0.0F};
  MissionInfoLayout layout;
  layout.frame = {origin.x, origin.y, width, height};
  const auto item_rect = [origin](const NovaDialogItem &item) {
    return SDL_FRect{origin.x + static_cast<float>(item.left),
                     origin.y + static_cast<float>(item.top),
                     static_cast<float>(item.right - item.left),
                     static_cast<float>(item.bottom - item.top)};
  };
  for (const auto &item : *items) {
    switch (item.index) {
    case 0: // UiPanel_GetEntryInfo entry 1: Done button
      layout.done_button = item_rect(item);
      break;
    case 1: // UiPanel_GetEntryInfo entry 2: native mission list
      layout.list = item_rect(item);
      break;
    case 2: // UiPanel_GetEntryInfo entry 3: heading
      layout.header = item_rect(item);
      break;
    case 3: // UiPanel_GetEntryInfo entry 4: description panel
      layout.description = item_rect(item);
      break;
    case 4: // UiPanel_GetEntryInfo entry 5: Abort button
      layout.abort_button = item_rect(item);
      break;
    case 6: // UiPanel_GetEntryInfo entry 7: current date
      layout.date = item_rect(item);
      break;
    default:
      break;
    }
  }
  if (layout.list.w <= 0.0F || layout.list.h <= 0.0F ||
      layout.description.w <= 0.0F || layout.done_button.w <= 0.0F ||
      layout.abort_button.w <= 0.0F || layout.header.w <= 0.0F ||
      layout.date.w <= 0.0F) {
    NovaLog::Todo("mission info DITL 0x3f4 is missing a required control rect");
    return std::nullopt;
  }
  return layout;
}

// @port 0x00445DC0 85% ui,rendering
// Ghidra 0x00445dc0 NovaUi_RebuildSpecialInteractionList (mission-info arm):
// rows are the active missions whose flags_at_accept lack the 0x400
// "invisible in the mission info dialog" bit, in slot order. Each row is the
// failed marker (DAT_0056c328: byte 0xa5 + space, which the original's text
// renderer draws as-is) plus the mission title (the misn resource name,
// wildcard-expanded through the active arm of 0x004444f0).
[[nodiscard]] std::vector<MissionInfoRow>
BuildMissionInfoRows(const GameState &state) {
  std::vector<MissionInfoRow> rows;
  for (std::size_t slot = 0; slot < state.active_missions.size(); ++slot) {
    const auto &flags = state.active_mission_runtime_flags[slot];
    const auto &mission = state.active_missions[slot];
    if (!flags.is_active || (flags.flags_primary_at_accept & 0x400) != 0U) {
      continue;
    }
    MissionInfoRow row;
    row.slot = static_cast<std::int16_t>(slot);
    row.failed = flags.is_failed;
    std::string title;
    if (mission.mission_template_id >= 0) {
      const auto *definition = state.scenario.Mission(
          static_cast<std::int16_t>(mission.mission_template_id + 0x80));
      if (definition != nullptr) {
        title = Mission_ExpandMissionWildcards(
            state, definition->display_name, false, row.slot);
      }
    }
    row.text = (row.failed ? std::string("\xa5 ") : std::string()) + title;
    rows.push_back(std::move(row));
  }
  return rows;
}

// The description panel: the selected mission's QuickBrief desc (MisnActive
// +0x37), through the load-time placeholder pass and the active-arm wildcard
// pass (NovaUi_RunMissionComputerWindow 0x00446150 -> Ui_LoadSelectionDialog-
// Resource + Stellar_BuildTravelDestinationDescription 0x004444f0).
[[nodiscard]] std::string BuildMissionInfoDescription(const GameState &state,
                                                      std::int16_t slot) {
  if (slot < 0 ||
      static_cast<std::size_t>(slot) >= state.active_missions.size() ||
      !state.active_mission_runtime_flags[static_cast<std::size_t>(slot)]
           .is_active) {
    return {};
  }
  const auto &mission = state.active_missions[static_cast<std::size_t>(slot)];
  const auto quick_brief = mission.brief_description_ids[1];
  if (quick_brief < 1) {
    return {};
  }
  const auto description =
      NovaResource_LoadDescription(static_cast<std::uint16_t>(quick_brief));
  if (!description || description->text.empty()) {
    return {};
  }
  std::string text = description->text;
  Mission_ExpandStringPlaceholders(state, text);
  return Mission_ExpandMissionWildcards(state, text, false, slot);
}

} // namespace

// @port 0x00446150 88% ui,rendering
// Ghidra 0x00446150 NovaUi_RunMissionComputerWindow (the gameplay command
// 0x28 window, default key I). Modal over the flight scene: the active-
// mission list (0x00445dc0 NovaUi_RebuildMissionComputerList), the selected
// mission's quick-brief text panel (draw 0x00446e00
// NovaUi_DrawMissionComputerWindow), the Abort/Done button pair (0x004a1520
// art + 0x004a13c0 hit-test) with the poller's navigation (0x00446770
// NovaUi_PollMissionComputerWindow), the starmap action (poller action 6,
// raised by command slot 9 — the same rebindable map command as flight and
// the mission briefing reader — with the selected mission's destination
// preselect), and the player-abort arm (reputation penalty +
// Mission_ClearMisnSlotAssignments(slot, 1)).
// Port conveniences: Esc closes; the open/close/denied cues play through
// `audio` directly instead of the queued centered-sound channel.
// TODO(decomp) skipped: restoring ai_secondary_target_slot/
// travel_transfer_mode around the nested map and refreshing the cargo status
// panel afterwards (the map itself plots the selected destination, it is not
// inspect-only).
void NovaMission_RunMissionInfoWindow(
    SdlPlatform &platform,
    SdlAudio &audio,
    GameState &state,
    const std::function<void()> &render_background) {
  SdlPlatform::ScopedPlacement placement_guard(platform,
                                               platform.current_placement());
  state.gameplay_now_ms = platform.gameplay_ticks_ms();
  const auto contains = [](const SDL_FRect &rect, SDL_FPoint point) {
    return point.x >= rect.x && point.x < rect.x + rect.w &&
           point.y >= rect.y && point.y < rect.y + rect.h;
  };
  const auto play_cue = [&](std::int16_t transition_index) {
    if (transition_index < 0 ||
        transition_index >=
            static_cast<std::int16_t>(state.transition_sounds.size())) {
      return;
    }
    const auto &sound =
        state.transition_sounds[static_cast<std::size_t>(transition_index)];
    if (sound.has_value()) {
      audio.Play(*sound,
                 1.0F,
                 1.0F,
                 150 + transition_index,
                 /*priority_width=*/1);
    }
  };

  const auto layout = LayoutMissionInfo();
  if (!layout) {
    return;
  }
  platform.SetPlacement(PlaceContained({layout->frame.w, layout->frame.h},
                                       platform.logical_playfield_size()));
  ProbeUiAutoClear probe_ui_guard(platform);
  const auto publish_probe_ui = [&]() {
    const auto probe_rect = [&platform](SDL_FRect rect) {
      return platform.current_placement().ToWindowRect(rect);
    };
    platform.PublishProbeUi("mission_info",
                            {{"window", probe_rect(layout->frame)},
                             {"abort", probe_rect(layout->abort_button)},
                             {"done", probe_rect(layout->done_button)},
                             {"list", probe_rect(layout->list)},
                             {"description", probe_rect(layout->description)}});
  };
  auto frame = LoadPictTexture(platform, kMissionInfoFramePict);
  ServicesButtonArt button_art;
  (void)button_art.Initialize(platform);
  NovaFontCache font_cache;
  const auto ui_style = NovaResource_LoadMainMenuStyle();
  // c.lr list palette read by NovaUi_DrawListRowCallback (0x00448a30).
  const NovaRgbColor list_text =
      ui_style ? ui_style->list_text : NovaRgbColor{255, 255, 255};
  const NovaRgbColor list_background =
      ui_style ? ui_style->list_background : NovaRgbColor{0, 0, 0};
  const NovaRgbColor list_hilite =
      ui_style ? ui_style->list_hilite : NovaRgbColor{128, 0, 0};

  // NovaAudio_QueueCenteredSound(g_transition_sound_handle_table[1], 1, ...).
  play_cue(1);

  std::vector<MissionInfoRow> rows = BuildMissionInfoRows(state);
  // DAT_0077430c: list selection index; the window only opens with a visible
  // active mission, so row 0 is preselected.
  int selected = rows.empty() ? -1 : 0;
  std::string description =
      selected >= 0 ? BuildMissionInfoDescription(
                          state, rows[static_cast<std::size_t>(selected)].slot)
                    : std::string();

  constexpr SDL_Color kText{255, 255, 255, 255};
  constexpr SDL_Color kPanelBlack{0, 0, 0, 255};
  // Every string in this window uses the shared screen font DAT_00735684 /
  // DAT_00735686, initialised to Geneva 9 by Ship_InitGameplayDataTables
  // (0x004b0c20: MOV word ptr [0x00735686],0x9) -- not the cïlr-configured
  // Chicago 12 of the button captions.
  constexpr float kMissionInfoFontSize = 9.0F;
  // Native list row pitch: NovaUi_RebuildSpecialInteractionList (0x00445dc0)
  // sizes rows as DAT_0088c01c (8) x the 1.5 UI-scale double at 0x005754f8.
  constexpr float kMissionInfoRowPitch = 12.0F;
  // Native list-control scrollbar strip: the rebuild (0x00445dc0) narrows
  // the list's right edge by 0xf when the content is taller than the view.
  constexpr float kMissionInfoScrollbarWidth = 15.0F;
  const SDL_FRect info_scrollbar{layout->list.x + layout->list.w -
                                     kMissionInfoScrollbarWidth,
                                 layout->list.y,
                                 kMissionInfoScrollbarWidth,
                                 layout->list.h};
  NovaListControl list_control(layout->list, kMissionInfoRowPitch, rows.size());
  const auto update_list_control = [&]() {
    list_control.SetRowCount(rows.size());
    SDL_FRect content = layout->list;
    if (list_control.scrollable()) {
      content.w = std::max(1.0F, content.w - kMissionInfoScrollbarWidth);
    }
    list_control.SetContentRect(content);
    if (selected >= 0) {
      list_control.EnsureVisible(static_cast<std::size_t>(selected));
    }
  };
  update_list_control();

  const auto draw_frame = [&]() {
    SDL_Renderer *renderer = platform.renderer();
    // Deliberate divergence (see docs/dlog_ditl_dialog_format.md): the live
    // background is re-rendered every frame and the window is layered on
    // top (the boarding/comm-dialog pattern); the original composites the
    // DLOG over the single game surface. The sim is paused, so this redraws
    // the same world each frame.
    if (render_background) {
      render_background();
    } else {
      SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
      SDL_RenderClear(renderer);
    }
    platform.SetPlacement(PlaceContained({layout->frame.w, layout->frame.h},
                                         platform.logical_playfield_size()));
    // DrawContext_BlitImageToRect(DAT_007742e4, UiWindow_GetRect(...)).
    if (frame != nullptr) {
      SDL_RenderTexture(renderer, frame->get(), nullptr, &layout->frame);
    } else {
      SDL_SetRenderDrawColor(renderer, 16, 40, 72, SDL_ALPHA_OPAQUE);
      SDL_RenderFillRect(renderer, &layout->frame);
      SDL_SetRenderDrawColor(renderer, 80, 140, 190, SDL_ALPHA_OPAQUE);
      SDL_RenderRect(renderer, &layout->frame);
    }

    const std::string heading =
        // STR# 0x7d2 1-based entry 0x168, "Currently active missions:".
        NovaHud_LoadStringEntry(0x7d2, 0x168)
            .value_or("Currently active missions:");
    // Cursor set to the entry-3 rect top-left; the baseline lands at
    // rect.top + the scaled font size (9).
    NovaText_Draw(platform,
                  font_cache,
                  NovaFontFamily::kGeneva,
                  kMissionInfoFontSize,
                  kNovaFontStyleRegular,
                  kText,
                  layout->header.x,
                  layout->header.y + kMissionInfoFontSize,
                  heading);
    if (layout->date.w > 0.0F) {
      // NovaText_FormatDateString (0x00468450) over the current game date.
      NovaText_DrawCentered(
          platform,
          font_cache,
          NovaFontFamily::kGeneva,
          kMissionInfoFontSize,
          kNovaFontStyleRegular,
          kText,
          layout->date.x,
          layout->date.x + layout->date.w,
          layout->date.y + kMissionInfoFontSize,
          NovaText_FormatDateString(
              state.date, true, state.date_prefix, state.date_suffix));
    }

    // Rows: c.lr fill, selected row in the 50%-red highlight, white text
    // throughout (NovaUi_DrawListRowCallback 0x00448a30 draws the failed 0xa5
    // marker as part of the row string, no separate tint). The native 12px
    // pitch and scroll offset come from the shared list control; rows are
    // clipped to the content band.
    if (!rows.empty()) {
      const SDL_FRect content = list_control.content_rect();
      const SDL_Rect list_clip{static_cast<int>(content.x),
                               static_cast<int>(content.y),
                               static_cast<int>(content.w),
                               static_cast<int>(content.h)};
      SDL_SetRenderClipRect(renderer, &list_clip);
      for (std::size_t row = 0; row < rows.size(); ++row) {
        const SDL_FRect row_rect = list_control.RowRect(row);
        if (row_rect.y >= content.y + content.h ||
            row_rect.y + row_rect.h <= content.y) {
          continue;
        }
        NovaUi_DrawListRow(platform,
                           font_cache,
                           row_rect,
                           rows[row].text,
                           static_cast<int>(row) == selected,
                           list_text,
                           list_background,
                           list_hilite);
      }
      SDL_SetRenderClipRect(renderer, nullptr);
    }
    NovaUi_DrawListScrollbar(platform, list_control, info_scrollbar);

    // @port 0x00446E00 70% ui,rendering
    // Description panel (0x00446e00): the filled-rect draw paints a white
    // rect with black text and the follow-up DrawContext_InvertRect cancels
    // both, so the net panel is BLACK with white left-aligned wrapped text
    // (the same invert-video pattern as the status panels of 0x004812c0 /
    // 0x0047fb70). The no-selection arm fills the rect black directly.
    // Text starts at the rect's top-left with the 9pt baseline and wraps
    // across the full rect width, clipped to the rect.
    SDL_SetRenderDrawColor(
        renderer, kPanelBlack.r, kPanelBlack.g, kPanelBlack.b, kPanelBlack.a);
    SDL_RenderFillRect(renderer, &layout->description);
    if (selected >= 0 && !description.empty()) {
      const auto lines = WrapDescriptionLines(
          description,
          static_cast<int>(std::max(1.0F, layout->description.w)),
          [&](std::string_view line) {
            return font_cache.TextWidth(NovaFontFamily::kGeneva,
                                        kMissionInfoFontSize,
                                        kNovaFontStyleRegular,
                                        line);
          });
      const SDL_Rect desc_clip{static_cast<int>(layout->description.x),
                               static_cast<int>(layout->description.y),
                               static_cast<int>(layout->description.w),
                               static_cast<int>(layout->description.h)};
      SDL_SetRenderClipRect(renderer, &desc_clip);
      const float line_pitch = static_cast<float>(
          font_cache.LineHeight(NovaFontFamily::kGeneva, kMissionInfoFontSize));
      float y = layout->description.y + kMissionInfoFontSize;
      for (const auto &line : lines) {
        if (y > layout->description.y + layout->description.h) {
          break;
        }
        NovaText_Draw(platform,
                      font_cache,
                      NovaFontFamily::kGeneva,
                      kMissionInfoFontSize,
                      kNovaFontStyleRegular,
                      kText,
                      layout->description.x,
                      y,
                      line);
        y += line_pitch;
      }
      SDL_SetRenderClipRect(renderer, nullptr);
    }

    // Abort/Done (three-state strips 0x1d4c family; captions STR# 0x96
    // entries 0x5/0x23). Abort is disabled unless the selected mission has
    // the CanAbort latch (NovaUi_DrawTravelDestinationServicesButtons-style
    // grey art in the original's FUN_004a1520).
    const bool abort_enabled =
        selected >= 0 && state
                             .active_missions[static_cast<std::size_t>(
                                 rows[static_cast<std::size_t>(selected)].slot)]
                             .can_abort;
    button_art.Draw(platform,
                    layout->abort_button,
                    abort_enabled ? ButtonState::kNormal
                                  : ButtonState::kDisabled);
    DrawThreeStateButtonLabel(
        platform,
        font_cache,
        layout->abort_button,
        NovaHud_LoadStringEntry(0x96, 0x23).value_or("Abort"),
        kText);
    button_art.Draw(platform, layout->done_button, ButtonState::kNormal);
    DrawThreeStateButtonLabel(
        platform,
        font_cache,
        layout->done_button,
        NovaHud_LoadStringEntry(0x96, 0x5).value_or("Done"),
        kText);
    publish_probe_ui();
    platform.Present();
  };

  // Render callback for the acceptance readers a script S activation may
  // open during an abort (e.g. the tutorial's on-abort S758): re-draw the
  // paused background beneath the reader, matching the window's own frame.
  const auto acceptance_background = [&]() {
    if (render_background) {
      render_background();
    }
  };

  // Ghidra action 5 (0x00446150): the abort arm.
  const auto abort_selected = [&]() {
    if (selected < 0) {
      // Denied cue + button-strip redraw only.
      play_cue(3);
      return;
    }
    const auto slot = rows[static_cast<std::size_t>(selected)].slot;
    const auto &mission = state.active_missions[static_cast<std::size_t>(slot)];
    if (!mission.can_abort) {
      return;
    }
    // Flags 0x40 ("Apply -5x CompReward reversal on abort") with a
    // completion government: subtract 5x the reward from every system the
    // government owns (the same per-system walk the success path uses).
    if ((mission.flags_primary & 0x40) != 0U && mission.comp_govt_id != -1) {
      const auto count = std::min<std::size_t>(state.system_reputation.size(),
                                               state.scenario.systems.size());
      for (std::size_t i = 0; i < count; ++i) {
        if (state.scenario.systems[i].government_id == mission.comp_govt_id) {
          state.system_reputation[i] = static_cast<std::int16_t>(
              state.system_reputation[i] + mission.comp_reward_delta * -5);
        }
      }
    }
    Mission_ClearMisnSlotAssignments(
        state,
        slot,
        /*emit_completion_payload=*/true,
        static_cast<std::uint32_t>(platform.gameplay_ticks_ms()),
        MakeAcceptanceSink(platform, state, acceptance_background));
    // Rebuild the list; an empty mission set closes the window.
    rows = BuildMissionInfoRows(state);
    selected = -1;
    description.clear();
    update_list_control();
    bool any_active = false;
    for (const auto &flags : state.active_mission_runtime_flags) {
      any_active = any_active || flags.is_active;
    }
    if (!any_active) {
      // The run loop exits; the close cue below still plays (the original
      // plays it once at teardown).
    }
  };

  const auto done = [&]() { return rows.empty() && selected < 0; };

  bool exit_requested = false;
  bool starmap_command_was_held = false;
  // The mission-computer command (slot 0x28, default I) also closes the
  // window, matching the original poller's key-event test alongside
  // Esc/Enter.
  const std::uint16_t mission_info_key = NovaInput_CommandKey(0x28);
  // Poller action 6: open the map (command slot 9) with the selected mission's
  // flags-0x100 destination preselected, saving/restoring the player's travel
  // selection around the nested map (Ghidra 0x00446150).
  const auto open_starmap = [&]() {
    std::int16_t preselect = -1;
    if (selected >= 0) {
      const auto &mission = state.active_missions[static_cast<std::size_t>(
          rows[static_cast<std::size_t>(selected)].slot)];
      preselect = MissionDestinationPreselect(state,
                                              mission.flags_primary,
                                              mission.travel_stellar_id,
                                              mission.return_stellar_id);
    }
    if (RunNestedMissionStarmap(platform, state, preselect)) {
      exit_requested = true;
    }
  };

  while (!platform.quit_requested() && !exit_requested) {
    draw_frame();
    const bool starmap_command_held =
        NovaInput_IsCommandActive(platform, kStarmapCommand);
    if (starmap_command_held && !starmap_command_was_held) {
      open_starmap();
    }
    starmap_command_was_held = starmap_command_held;
    if (exit_requested) {
      break;
    }
    for (std::optional<TextInput> input; (input = platform.PollTextEvent());) {
      // @port 0x00446770 65% ui
      // PollSpecialInteractionWindow (0x00446770): Enter/Esc/I close, arrows
      // move the selection; the map command is polled above the loop.
      if (input->key_code != 0xffff && input->key_code == mission_info_key) {
        exit_requested = true;
        break;
      }
      if (input->key == TextKey::escape || input->key == TextKey::enter) {
        exit_requested = true;
        break;
      }
      if (input->key == TextKey::physical) {
        // 0x00446770: Up/Down move the selection and the list control scrolls
        // it into view. The original reads compact key codes 9/10/0xb; the
        // port's normalized channel reports Tab/Up/Down as 0x0f/0x61/0x66.
        const auto move = [&](int delta) {
          if (rows.empty()) {
            return;
          }
          selected += delta;
          if (selected < 0) {
            selected = static_cast<int>(rows.size()) - 1;
          } else if (selected >= static_cast<int>(rows.size())) {
            selected = 0;
          }
          list_control.EnsureVisible(static_cast<std::size_t>(selected));
          description = BuildMissionInfoDescription(
              state, rows[static_cast<std::size_t>(selected)].slot);
        };
        if (input->key_code == 0x61) { // Up
          move(-1);
        } else if (input->key_code == 0x66) { // Down
          move(1);
        } else if (input->key_code == 0x0f) { // Tab: next
          move(1);
        }
        continue;
      }
      if (input->key == TextKey::primary) {
        const SDL_FPoint point = platform.mouse_position();
        bool handled = false;
        // Scrollbar arm of NovaList_HitTestPoint (0x004d1db0): arrows step one
        // row, a click above/below the thumb pages by the visible row count,
        // and a click on the thumb does nothing (no drag tracking).
        switch (NovaListScrollbarHitTest(list_control, info_scrollbar, point)) {
        case NovaListScrollbarPart::kUp:
          list_control.ScrollRows(-1);
          handled = true;
          break;
        case NovaListScrollbarPart::kDown:
          list_control.ScrollRows(1);
          handled = true;
          break;
        case NovaListScrollbarPart::kPageUp:
          list_control.ScrollRows(-list_control.visible_rows());
          handled = true;
          break;
        case NovaListScrollbarPart::kPageDown:
          list_control.ScrollRows(list_control.visible_rows());
          handled = true;
          break;
        case NovaListScrollbarPart::kThumb:
        case NovaListScrollbarPart::kNone:
          break;
        }
        // FUN_004d1db0: the list control maps the click to row
        // (click_y - list.top) / native row pitch + scroll; a click past the
        // last row finds no matching list row and deselects (DAT_0077430c
        // resets to -1).
        if (!handled && contains(list_control.content_rect(), point)) {
          const auto row = list_control.RowAt(point);
          const int new_selected = row ? static_cast<int>(*row) : -1;
          if (new_selected != selected) {
            selected = new_selected;
            description =
                selected >= 0
                    ? BuildMissionInfoDescription(
                          state, rows[static_cast<std::size_t>(selected)].slot)
                    : std::string();
          }
          handled = true;
        }
        if (!handled && contains(layout->abort_button, point)) {
          abort_selected();
          if (done()) {
            exit_requested = true;
          }
        } else if (!handled && contains(layout->done_button, point)) {
          exit_requested = true;
        }
        continue;
      }
    }
    if (done()) {
      // The abort arm emptied the mission set: the original exits after the
      // rebuild instead of redrawing an empty window.
      exit_requested = true;
    }
    platform.PaceFrame();
  }
  play_cue(2);
}

} // namespace game
