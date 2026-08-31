#include "docked_dialog.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../pict_image.hpp"
#include "../sdl_platform.hpp"
#include "hud_overlay.hpp"
#include "landed_store.hpp"
#include "mission.hpp"
#include "mission_script.hpp"
#include "nova_font.hpp"
#include "scenario_data.hpp"
#include "selection_text_dialog.hpp"
#include "services_buttons.hpp"
#include "ship_visual.hpp"
#include "sprite_world.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>

namespace game {
namespace {

// The docked backdrop re-layered behind every sub-window dialog. This is the
// full-width Spaceport PICT 0x2134 (618x517) draw across the whole 640x480
// playfield, exactly as the docked main menu presents it (Ghidra
// FUN_0048e970's base overlay; see landed_window.cpp for the 0x2137 fallback
// when 0x2134 is missing).
constexpr std::uint16_t kDockedBackdropPict = 0x2134;

// The dim scrim between the docked backdrop and the active dialog window, so
// the modal reads as popped off the docked screen (mirrors the game's modal
// sub-windows dimming/occluding the backing dialog).
constexpr SDL_Color kScrim{0, 0, 0, 150};

// Colours for the dialog heading/leave labels, matching the docked menu's
// palette (bright rows for the title, dim rows for the helper text).
constexpr SDL_Color kTitle{255, 255, 255, 255};
constexpr SDL_Color kDim{192, 192, 192, 255};

// The heading / leave-button label for each sub-window. Kept close to the
// game's service labels (ServiceLabel in landed_window.cpp).
std::string_view SubWindowHeading(LandedService service) {
  switch (service) {
  case LandedService::kBuySellCargo:
    return "Trade center";
  case LandedService::kOutfit:
    return "Outfitter";
  case LandedService::kShipyard:
    return "Shipyard";
  case LandedService::kBar:
    return "Bar";
  case LandedService::kMissionBoard:
    return "Mission BBS";
  default:
    return "Services";
  }
}

// Loads one PICT resource into a texture (null on failure to locate or decode).
std::unique_ptr<SdlTexture> LoadPictTexture(SdlPlatform &platform,
                                            std::uint16_t pict_id) {
  const auto data = NovaResource_LoadPictData(pict_id);
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

// Capture the dock after it has been presented and before a nested dialog
// changes the renderer presentation. This is the same modal-background
// strategy used by the main-menu pilot dialog and retains all dock-owned
// content (planet art, labels, status text, and service buttons).
[[nodiscard]] std::unique_ptr<SdlTexture>
CaptureDockedBackground(SDL_Renderer *renderer) {
  SDL_Surface *const surface = SDL_RenderReadPixels(renderer, nullptr);
  if (surface == nullptr) {
    NovaLog::Warn("could not snapshot dock before nested dialog: {}",
                  SDL_GetError());
    return nullptr;
  }
  SDL_Texture *const texture = SDL_CreateTextureFromSurface(renderer, surface);
  SDL_DestroySurface(surface);
  if (texture == nullptr) {
    NovaLog::Warn("could not create dock snapshot texture: {}", SDL_GetError());
    return nullptr;
  }
  SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
  return std::make_unique<SdlTexture>(texture);
}

// Draws one frame of the sub-window dialog: the docked backdrop across the
// full 640x480 playfield, the dim scrim, then the frame PICT centred at its
// natural size with the heading and Leave button.
void DrawSubWindowDialog(SdlPlatform &platform,
                         NovaFontCache &font_cache,
                         const ServicesButtonArt &button_art,
                         SDL_Texture *backdrop,
                         SDL_Texture *frame,
                         LandedService service,
                         const SDL_FRect &panel) {
  SDL_Renderer *renderer = platform.renderer();
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(renderer);
  platform.SetCenteredPlayfield();
  if (backdrop != nullptr) {
    SDL_RenderTexture(renderer, backdrop, nullptr, &panel);
  }

  // Dim the docked screen so the modal reads as a separate window.
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer, kScrim.r, kScrim.g, kScrim.b, kScrim.a);
  SDL_RenderFillRect(renderer, &panel);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

  if (frame == nullptr) {
    // No frame art: draw a bordered placeholder panel centred on the playfield
    // so the dialog is still legible, then treat it as the frame bounds.
    SDL_SetRenderDrawColor(renderer, 16, 40, 72, SDL_ALPHA_OPAQUE);
    const SDL_FRect placeholder{
        (panel.w - 480.0F) / 2.0F, 88.0F, 480.0F, 300.0F};
    SDL_RenderFillRect(renderer, &placeholder);
    SDL_SetRenderDrawColor(renderer, 80, 140, 190, SDL_ALPHA_OPAQUE);
    SDL_RenderRect(renderer, &placeholder);
    const SDL_FRect dst = placeholder;
    const SDL_FRect leaveRect(
        panel.x + panel.w / 2.0F - 72.0F,
        std::min(dst.y + dst.h + 10.0F, panel.y + panel.h - 40.0F),
        144.0F,
        25.0F);
    button_art.Draw(platform, leaveRect, ButtonState::kNormal);
    NovaText_DrawCentered(platform,
                          font_cache,
                          kThreeStateButtonFontFamily,
                          kThreeStateButtonFontSize,
                          kNovaFontStyleRegular,
                          kTitle,
                          leaveRect.x,
                          leaveRect.x + leaveRect.w,
                          ThreeStateButtonLabelBaseline(leaveRect),
                          "LEAVE");
    return;
  }

  // Centre the frame at its natural size on the 640x480 playfield (nudged up
  // a touch so the heading and Leave button below it sit comfortably).
  float frame_w = 0.0F;
  float frame_h = 0.0F;
  SDL_GetTextureSize(frame, &frame_w, &frame_h);
  float dst_y = (panel.h - frame_h) / 2.0F - 18.0F;
  // Keep the Leave button on-screen even for a frame that nearly fills the
  // playfield.
  dst_y = std::max(0.0F, dst_y);
  const SDL_FRect dst{(panel.w - frame_w) / 2.0F, dst_y, frame_w, frame_h};
  SDL_RenderTexture(renderer, frame, nullptr, &dst);

  // The Leave button sits just under the frame, clamped into the playfield.
  const SDL_FRect leaveRect(
      panel.x + panel.w / 2.0F - 72.0F,
      std::min(dst.y + dst.h + 10.0F, panel.y + panel.h - 40.0F),
      144.0F,
      25.0F);

  // Heading in the title band above the frame.
  std::string heading{SubWindowHeading(service)};
  if (!heading.empty()) {
    heading[0] =
        static_cast<char>(std::toupper(static_cast<unsigned char>(heading[0])));
  }
  const SDL_FRect headingBand{panel.x, dst.y - 30.0F, panel.w, 26.0F};
  NovaText_DrawCentered(platform,
                        font_cache,
                        NovaFontFamily::kChicago,
                        18.0F,
                        kNovaFontStyleRegular,
                        kTitle,
                        headingBand.x,
                        headingBand.x + headingBand.w,
                        headingBand.y + headingBand.h / 2.0F + 6.0F,
                        heading);

  // A grey-backed "Leave" button centred under the frame. Clicking it (or Esc /
  // Enter / a primary press anywhere) closes the dialog back to the dock.
  button_art.Draw(platform, leaveRect, ButtonState::kNormal);
  NovaText_DrawCentered(platform,
                        font_cache,
                        kThreeStateButtonFontFamily,
                        kThreeStateButtonFontSize,
                        kNovaFontStyleRegular,
                        kTitle,
                        leaveRect.x,
                        leaveRect.x + leaveRect.w,
                        ThreeStateButtonLabelBaseline(leaveRect),
                        "LEAVE");

  // Footer hint just above the Leave button.
  NovaText_DrawCentered(platform,
                        font_cache,
                        NovaFontFamily::kGeneva,
                        12.0F,
                        kNovaFontStyleRegular,
                        kDim,
                        panel.x,
                        panel.x + panel.w,
                        leaveRect.y - 16.0F,
                        "Esc / Enter / click to close");
}

struct MissionBoardLayout {
  SDL_FRect frame{};
  SDL_FRect list{};
  SDL_FRect selected_title{};
  SDL_FRect description{};
  SDL_FRect take{};
  SDL_FRect decline{};
  SDL_FRect header{};
  SDL_FRect date{};
  // Stellar_RebuildTravelDestinationList creates an eight-row native list
  // control. Its row height is the list control's DITL height divided by that
  // row count; the old clean-room pass used an unrelated 18-pixel constant.
  float list_row_pitch = 0.0F;
};

// NovaUi_DrawListRowCallback (0x00448a30) receives the native row rectangle,
// places text at row_top + DAT_00735686, and uses a four-pixel left inset.
// The destination list is configured with the shared 12-point screen font.
constexpr float kMissionListFontSize = 12.0F;

[[nodiscard]] std::optional<MissionBoardLayout>
LayoutMissionBoard(const SdlPlatform &platform) {
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
  const SDL_FPoint output = platform.logical_playfield_size();
  const SDL_FPoint origin{(output.x - width) / 2.0F,
                          (output.y - height) / 2.0F};
  MissionBoardLayout layout;
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

  constexpr float kNativeMissionListRows = 8.0F;
  if (layout.list.w <= 0.0F || layout.list.h <= 0.0F ||
      layout.description.w <= 0.0F || layout.take.w <= 0.0F ||
      layout.decline.w <= 0.0F || layout.header.w <= 0.0F ||
      layout.date.w <= 0.0F) {
    NovaLog::Todo("Mission BBS DITL 0x3ee is missing a required control rect");
    return std::nullopt;
  }
  layout.list_row_pitch = layout.list.h / kNativeMissionListRows;
  return layout;
}

void DrawMissionBoardContents(SdlPlatform &platform,
                              NovaFontCache &font_cache,
                              const ServicesButtonArt &button_art,
                              const GameState &state,
                              const MissionBoardLayout &layout,
                              const MissionListEvaluation &missions,
                              std::size_t selected,
                              std::string_view status) {
  SDL_Renderer *renderer = platform.renderer();
  constexpr SDL_Color kText{255, 255, 255, 255};
  constexpr SDL_Color kMissionDim{192, 192, 192, 255};
  constexpr SDL_Color kSelected{255, 255, 255, 255};
  // NovaUi_DrawListRowCallback (0x00448a30) fills every row from the c.lr
  // palette: DAT_0073566a is black and DAT_00735670 is 50% red.
  constexpr SDL_Color kRowNormal{0, 0, 0, 255};
  constexpr SDL_Color kRowSelected{128, 0, 0, 255};

  const auto &rows = missions.page_zero;
  const std::string heading =
      // 0x167 is the original's 1-based entry -> "The following missions are
      // available here:".
      NovaHud_LoadStringEntry(0x7d2, 0x167)
          .value_or("The following missions are available here");
  NovaText_Draw(platform,
                font_cache,
                NovaFontFamily::kGeneva,
                12.0F,
                kNovaFontStyleRegular,
                kText,
                layout.header.x,
                layout.header.y + layout.header.h - 2.0F,
                heading);
  if (layout.list.w > 0.0F && !rows.empty()) {
    const float row_pitch = layout.list_row_pitch;
    const float text_x = layout.list.x + 4.0F;
    for (std::size_t row = 0;
         row < rows.size() &&
         layout.list.y + row * row_pitch < layout.list.y + layout.list.h;
         ++row) {
      const auto mission_id = rows[row];
      const auto *definition =
          state.scenario.Mission(static_cast<std::int16_t>(mission_id + 0x80));
      const std::string label =
          definition != nullptr && !definition->display_name.empty()
              ? Mission_ExpandMissionWildcards(
                    state, definition->display_name, true, rows[row])
              : "Mission " + std::to_string(mission_id);
      const float row_top = layout.list.y + row * row_pitch;
      const float baseline = row_top + kMissionListFontSize;
      const SDL_FRect row_rect{
          layout.list.x, row_top, layout.list.w, row_pitch};
      const SDL_Color row_color = row == selected ? kRowSelected : kRowNormal;
      SDL_SetRenderDrawColor(
          renderer, row_color.r, row_color.g, row_color.b, row_color.a);
      SDL_RenderFillRect(renderer, &row_rect);
      NovaText_Draw(platform,
                    font_cache,
                    NovaFontFamily::kGeneva,
                    kMissionListFontSize,
                    kNovaFontStyleRegular,
                    row == selected ? kSelected : kText,
                    text_x,
                    baseline,
                    label);
    }
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
                  NovaFontFamily::kGeneva,
                  12.0F,
                  kNovaFontStyleBold,
                  kText,
                  layout.selected_title.x,
                  layout.selected_title.y + 15.0F,
                  title);
  }

  // DITL items 1 and 7 are the small action buttons. DITL item 3 is the list
  // scrollbar, not a button; painting it as ACCEPT caused the giant overlay.
  for (const auto &[rect, label] :
       std::array<std::pair<SDL_FRect, std::string_view>, 2>{
           {{layout.take, "Accept"}, {layout.decline, "Leave"}}}) {
    if (rect.w <= 0.0F) {
      continue;
    }
    button_art.Draw(platform, rect, ButtonState::kNormal);
    NovaText_DrawCentered(platform,
                          font_cache,
                          kThreeStateButtonFontFamily,
                          kThreeStateButtonFontSize,
                          kNovaFontStyleRegular,
                          kText,
                          rect.x,
                          rect.x + rect.w,
                          ThreeStateButtonLabelBaseline(rect),
                          label);
  }
  if (layout.date.w > 0.0F) {
    // GameState does not yet track g_current_game_year_month/day. Keep the
    // DITL date field visible with the original new-pilot baseline until that
    // calendar state is reconstructed.
    NovaText_DrawCentered(platform,
                          font_cache,
                          NovaFontFamily::kGeneva,
                          12.0F,
                          kNovaFontStyleRegular,
                          kText,
                          layout.date.x,
                          layout.date.x + layout.date.w,
                          layout.date.y + layout.date.h - 2.0F,
                          "1/1/1999");
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
  if (layout.description.w > 0.0F && !rows.empty() && selected < rows.size()) {
    if (const auto description = NovaResource_LoadDescription(
            static_cast<std::uint16_t>(rows[selected] + 4000));
        description && !description->text.empty()) {
      // The original loads the desc into the shared scratch (running the
      // placeholder pass at load, 0x004c6d50) and runs the same wildcard pass
      // as the list rows (NovaUi_RunTravelDestinationMainWindow 0x0043c470 ->
      // Stellar_BuildTravelDestinationDescription).
      std::string loaded_text = description->text;
      Mission_ExpandStringPlaceholders(state, loaded_text);
      const std::string expanded_text = Mission_ExpandMissionWildcards(
          state, loaded_text, true, rows[selected]);
      const float x = layout.description.x + 6.0F;
      const float width = layout.description.w - 12.0F;
      const auto lines = WrapDescriptionLines(
          expanded_text,
          static_cast<int>(std::max(1.0F, width)),
          [&](std::string_view line) {
            return font_cache.TextWidth(
                NovaFontFamily::kGeneva, 12.0F, kNovaFontStyleRegular, line);
          });
      float y = layout.description.y + 15.0F;
      for (const auto &line : lines) {
        if (y > layout.description.y + layout.description.h) {
          break;
        }
        NovaText_Draw(platform,
                      font_cache,
                      NovaFontFamily::kGeneva,
                      12.0F,
                      kNovaFontStyleRegular,
                      kText,
                      x,
                      y,
                      line);
        y += 14.0F;
      }
    }
  }
}

void DrawMissionBoardBase(SdlPlatform &platform,
                          SDL_Texture *snapshot,
                          SDL_Texture *backdrop,
                          SDL_Texture *frame,
                          const MissionBoardLayout &layout) {
  SDL_Renderer *renderer = platform.renderer();
  platform.SetFullscreenPlayfield();
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(renderer);
  if (snapshot != nullptr) {
    const SDL_FPoint output = platform.logical_playfield_size();
    const SDL_FRect snapshot_rect{0.0F, 0.0F, output.x, output.y};
    SDL_RenderTexture(renderer, snapshot, nullptr, &snapshot_rect);
  } else if (backdrop != nullptr) {
    const SDL_FPoint output = platform.logical_playfield_size();
    float width = 0.0F;
    float height = 0.0F;
    SDL_GetTextureSize(backdrop, &width, &height);
    const SDL_FRect backdrop_rect{
        (output.x - width) / 2.0F, (output.y - height) / 2.0F, width, height};
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
void NovaMission_RunAcceptanceDialogs(SdlPlatform &platform,
                                      GameState &state,
                                      std::int16_t mission_def,
                                      SDL_Texture *docked_snapshot);

// Ghidra 0x0043c470 NovaUi_RunTravelDestinationMainWindow (partial port of the
// landed Mission BBS: layout, list/description rendering, selection, accept).
LandedExit RunMissionBoardDialog(SdlPlatform &platform,
                                 GameState &state,
                                 std::int16_t stellar_id,
                                 SDL_Texture *docked_snapshot) {
  (void)stellar_id;
  const auto contains = [](const SDL_FRect &rect, SDL_FPoint point) {
    return point.x >= rect.x && point.x < rect.x + rect.w &&
           point.y >= rect.y && point.y < rect.y + rect.h;
  };
  auto backdrop = LoadPictTexture(platform, kDockedBackdropPict);
  auto frame = LoadPictTexture(platform, 0x2139);
  SDL_Texture *snapshot = docked_snapshot;
  ServicesButtonArt button_art;
  (void)button_art.Initialize(platform);
  NovaFontCache font_cache;
  const auto layout = LayoutMissionBoard(platform);
  if (!layout) {
    return LandedExit::kServiceComplete;
  }
  MissionListEvaluation missions = Mission_EvaluateMissionLists(state);
  std::size_t selected = 0;
  std::string status;
  NovaLog::Info(
      "mission BBS opened at stellar {}: {} available rows (first id {})",
      static_cast<int>(stellar_id),
      missions.page_zero.size(),
      missions.page_zero.empty()
          ? -1
          : static_cast<int>(missions.page_zero.front()));

  while (!platform.quit_requested()) {
    DrawMissionBoardBase(platform,
                         snapshot,
                         backdrop ? backdrop->get() : nullptr,
                         frame ? frame->get() : nullptr,
                         *layout);
    DrawMissionBoardContents(platform,
                             font_cache,
                             button_art,
                             state,
                             *layout,
                             missions,
                             selected,
                             status);
    SDL_RenderPresent(platform.renderer());

    auto accept = [&]() {
      if (missions.page_zero.empty() || selected >= missions.page_zero.size()) {
        return;
      }
      const auto mission_id = missions.page_zero[selected];
      if (Mission_ActivateAtSlot(state, mission_id, stellar_id)) {
        status = "Mission accepted";
        missions = Mission_EvaluateMissionLists(state);
        if (selected >= missions.page_zero.size() &&
            !missions.page_zero.empty()) {
          selected = missions.page_zero.size() - 1;
        }
        // Mission_ActivateMissionAtSlot (0x0043f100) shows the Brief/
        // LoadCarg dialogs after slot population; the state-only port
        // activates above, so run the UI chain here.
        NovaMission_RunAcceptanceDialogs(
            platform, state, mission_id, docked_snapshot);
      } else {
        status = "Mission could not be accepted";
      }
    };

    for (std::optional<TextInput> input; (input = platform.PollTextEvent());) {
      if (input->key == TextKey::escape) {
        return LandedExit::kServiceComplete;
      }
      if (input->key == TextKey::enter) {
        accept();
        continue;
      }
      if (input->key == TextKey::primary) {
        const SDL_FPoint point = platform.mouse_position();
        bool handled = false;
        if (contains(layout->list, point) && !missions.page_zero.empty()) {
          const auto row = static_cast<std::size_t>((point.y - layout->list.y) /
                                                    layout->list_row_pitch);
          if (row < missions.page_zero.size()) {
            selected = row;
            handled = true;
          }
        }
        if (!handled && contains(layout->take, point)) {
          accept();
        } else if (!handled && contains(layout->decline, point)) {
          return LandedExit::kServiceComplete;
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
      if (key == 'j' && !missions.page_zero.empty()) {
        selected = (selected + 1) % missions.page_zero.size();
      } else if (key == 'k' && !missions.page_zero.empty()) {
        selected = selected == 0 ? missions.page_zero.size() - 1 : selected - 1;
      } else if (key == 'a') {
        accept();
      }
    }
    SDL_Delay(16);
  }
  return LandedExit::kQuit;
}

[[nodiscard]] bool Contains(const SDL_FRect &rect, SDL_FPoint point) {
  return point.x >= rect.x && point.x < rect.x + rect.w && point.y >= rect.y &&
         point.y < rect.y + rect.h;
}

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

[[nodiscard]] SDL_FRect OffsetRect(SDL_FRect rect, SDL_FPoint origin) {
  rect.x += origin.x;
  rect.y += origin.y;
  return rect;
}

[[nodiscard]] StoreLayout LayoutStore(const SdlPlatform &platform,
                                      bool outfit_store) {
  const SDL_FPoint output = platform.logical_playfield_size();
  constexpr float kWidth = 765.0F;
  const float height = outfit_store ? 321.0F : 323.0F;
  const SDL_FPoint origin{std::max(0.0F, (output.x - kWidth) / 2.0F),
                          std::max(0.0F, (output.y - height) / 2.0F)};
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
                   SDL_Texture *snapshot,
                   SDL_Texture *backdrop,
                   SDL_Texture *frame,
                   const StoreLayout &layout) {
  SDL_Renderer *renderer = platform.renderer();
  platform.SetFullscreenPlayfield();
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(renderer);
  const SDL_FPoint output = platform.logical_playfield_size();
  if (snapshot != nullptr) {
    // The store opens over the already-rendered Spaceport DLOG. Replaying the
    // dock snapshot preserves the planet, title, status text, and service
    // buttons; rebuilding only PICT 0x2134 loses that complete backdrop.
    const SDL_FRect snapshot_rect{0.0F, 0.0F, output.x, output.y};
    SDL_RenderTexture(renderer, snapshot, nullptr, &snapshot_rect);
  } else if (backdrop != nullptr) {
    float width = 0.0F;
    float height = 0.0F;
    SDL_GetTextureSize(backdrop, &width, &height);
    const SDL_FRect dst{
        (output.x - width) / 2.0F, (output.y - height) / 2.0F, width, height};
    SDL_RenderTexture(renderer, backdrop, nullptr, &dst);
  }
  // Ghidra's NovaUi_RedrawTravelOutfitMenu (0x00490c70) and the analogous
  // shipyard redraw fill and draw their modal window surface, then composite
  // it over the existing travel scene. There is no full-screen dim/scrim.
  if (frame != nullptr) {
    SDL_RenderTexture(renderer, frame, nullptr, &layout.frame);
  }
}

// Ghidra 0x00497b70 NovaUi_BlitPictThumbnailCached: per-modal thumbnail cache.
// The original's shared 128-entry atlas LRU and three-load-per-redraw cadence
// are not reproduced.
struct StoreTextureCache {
  std::unordered_map<std::int16_t, std::unique_ptr<SdlTexture>> pictures;
  std::unordered_map<std::int16_t, std::unique_ptr<SpriteAsset>> ship_sprites;
};

[[nodiscard]] SDL_Texture *StorePreviewTexture(SdlPlatform &platform,
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
  const std::int32_t pict_id =
      outfit_store ? static_cast<std::int32_t>(id - 0x80) + 6000
                   : static_cast<std::int32_t>(id - 0x80) + 5000;
  auto picture = LoadPictTexture(platform, static_cast<std::uint16_t>(pict_id));
  if (picture) {
    SDL_Texture *result = picture->get();
    cache.pictures.emplace(id, std::move(picture));
    return result;
  }
  cache.pictures.emplace(id, nullptr);
  if (outfit_store) {
    return nullptr;
  }

  // NovaData_LoadAllShipClassVisualAndLaunchData prefers the class PICT at
  // (zero-based class + 5000), then falls back to the cloned ship sprite set.
  // We do the same for plug-in ships whose preview PICT is absent.
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

// Ghidra DrawContext_DrawGroupedUInt (see boarding_plunder.cpp): decimal
// digits grouped in threes with commas.
[[nodiscard]] std::string GroupedUInt(std::int32_t value) {
  const std::string digits = std::to_string(value);
  std::string out;
  out.reserve(digits.size() + digits.size() / 3);
  for (std::size_t i = 0; i < digits.size(); ++i) {
    if (i > 0 && (digits.size() - i) % 3 == 0) {
      out.push_back(',');
    }
    out.push_back(digits[i]);
  }
  return out;
}

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

void DrawStoreContents(SdlPlatform &platform,
                       NovaFontCache &font_cache,
                       const ServicesButtonArt &button_art,
                       const GameState &state,
                       const LandedStoreSession &session,
                       std::int16_t stellar_id,
                       const StoreLayout &layout,
                       StoreTextureCache &texture_cache,
                       SDL_Texture *selected_image,
                       std::string_view selected_description) {
  constexpr SDL_Color kText{255, 255, 255, 255};
  constexpr SDL_Color kMuted{192, 192, 192, 255};
  const bool outfit_store = session.kind == LandedStoreKind::kOutfitter;
  SDL_Renderer *renderer = platform.renderer();
  for (std::size_t slot = 0; slot < LandedStoreSession::kPageSlots; ++slot) {
    const std::size_t index = session.page_base + slot;
    if (index >= session.available_ids.size())
      break;
    const std::int16_t id = session.available_ids[index];
    const SDL_FRect rect = StoreCell(layout, slot);
    const bool selected = id == session.selected_id;
    SDL_SetRenderDrawColor(renderer,
                           selected ? 220 : 80,
                           selected ? 235 : 140,
                           selected ? 255 : 190,
                           SDL_ALPHA_OPAQUE);
    SDL_RenderRect(renderer, &rect);
    if (SDL_Texture *thumbnail =
            StorePreviewTexture(platform, texture_cache, outfit_store, id)) {
      const SDL_FRect thumbnail_rect{
          rect.x + (rect.w - 32.0F) / 2.0F, rect.y + 3.0F, 32.0F, 32.0F};
      SDL_RenderTexture(renderer, thumbnail, nullptr, &thumbnail_rect);
    }
    const std::string_view name =
        outfit_store ? std::string_view{state.scenario.Outfit(id)->short_name}
                     : std::string_view{state.scenario.Ship(id)->short_name};
    const StoreLabelLines label = SplitStoreLabel(name);
    // NovaUi_RedrawTravelOutfitMenu (0x00490c70) and
    // NovaUi_DrawShipyardShipList (0x004948b0) place a single line at
    // bottom-6, or split labels at bottom-14 and bottom-3.
    const float first_baseline =
        rect.y + (label.second.empty() ? 49.0F : 41.0F);
    NovaText_DrawCentered(platform,
                          font_cache,
                          NovaFontFamily::kGeneva,
                          10.0F,
                          kNovaFontStyleRegular,
                          kText,
                          rect.x + 2.0F,
                          rect.x + rect.w - 2.0F,
                          first_baseline,
                          label.first);
    if (!label.second.empty()) {
      NovaText_DrawCentered(platform,
                            font_cache,
                            NovaFontFamily::kGeneva,
                            10.0F,
                            kNovaFontStyleRegular,
                            kText,
                            rect.x + 2.0F,
                            rect.x + rect.w - 2.0F,
                            rect.y + 52.0F,
                            label.second);
    }
    if (outfit_store) {
      const std::int16_t count = state.inventory.outfit_owned_count[id - 0x80];
      NovaText_DrawCentered(platform,
                            font_cache,
                            NovaFontFamily::kGeneva,
                            9.0F,
                            kNovaFontStyleRegular,
                            kMuted,
                            rect.x + 2.0F,
                            rect.x + rect.w - 2.0F,
                            rect.y + 12.0F,
                            std::to_string(count));
    }
  }
  const bool buy_allowed =
      session.selected_id >= 0 &&
      (outfit_store
           ? NovaLanded_CanBuyOutfit(state, stellar_id, session.selected_id)
           : NovaLanded_CanBuyShip(state, stellar_id, session.selected_id));
  const bool sell_allowed =
      outfit_store && session.selected_id >= 0 &&
      state.inventory.outfit_owned_count[session.selected_id - 0x80] > 0 &&
      (state.scenario.Outfit(session.selected_id)->flags & 0x0008U) == 0U;
  const std::array<std::tuple<SDL_FRect, std::string_view, bool>, 5> controls{
      {{layout.leave, "LEAVE", true},
       {layout.buy, "BUY", buy_allowed},
       {layout.sell_or_info,
        outfit_store ? "SELL" : "INFO",
        outfit_store ? sell_allowed : session.selected_id >= 0},
       {layout.previous, "<", session.CanPagePrevious()},
       {layout.next, ">", session.CanPageNext()}}};
  for (const auto &[rect, label, enabled] : controls) {
    button_art.Draw(platform,
                    rect,
                    enabled ? ButtonState::kNormal : ButtonState::kDisabled);
    // Three-state button labels: white enabled, 50% grey disabled, in the
    // plain (non-bold) screen font -- the original's shared renderer
    // (NovaUi_DrawThreeStateButton, label colours DAT_007d8350).
    const SDL_Color kLabelGrey{128, 128, 128, 255};
    NovaText_DrawCentered(platform,
                          font_cache,
                          kThreeStateButtonFontFamily,
                          kThreeStateButtonFontSize,
                          kNovaFontStyleRegular,
                          enabled ? kText : kLabelGrey,
                          rect.x,
                          rect.x + rect.w,
                          ThreeStateButtonLabelBaseline(rect),
                          label);
  }
  if (session.selected_id >= 0) {
    const std::string title =
        outfit_store ? state.scenario.Outfit(session.selected_id)->name
                     : state.scenario.Ship(session.selected_id)->display_name;
    const std::int32_t price =
        outfit_store
            ? NovaLanded_OutfitPrice(state, stellar_id, session.selected_id)
            : 0; // Ship price rows render in the details-panel block below.
    const std::int32_t ship_price =
        outfit_store ? 0
                     : NovaLanded_ShipPurchasePrice(
                           state, stellar_id, session.selected_id);
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
    NovaText_DrawCentered(platform,
                          font_cache,
                          NovaFontFamily::kGeneva,
                          12.0F,
                          kNovaFontStyleBold,
                          kText,
                          layout.description.x + 4.0F,
                          layout.description.x + layout.description.w - 4.0F,
                          layout.description.y + 18.0F,
                          title);
    if (outfit_store) {
      const Outfit *outfit = state.scenario.Outfit(session.selected_id);
      const ShipClass *player_ship = state.scenario.Ship(
          static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
      const std::int32_t mass =
          player_ship == nullptr ? 0
                                 : outfit->PurchaseMass(player_ship->mass_tons);
      NovaText_DrawCentered(platform,
                            font_cache,
                            NovaFontFamily::kGeneva,
                            10.0F,
                            kNovaFontStyleRegular,
                            kMuted,
                            layout.details.x + 2.0F,
                            layout.details.x + layout.details.w - 2.0F,
                            layout.details.y + 15.0F,
                            "Price: " + std::to_string(price));
      NovaText_DrawCentered(platform,
                            font_cache,
                            NovaFontFamily::kGeneva,
                            9.0F,
                            kNovaFontStyleRegular,
                            kMuted,
                            layout.details.x + 2.0F,
                            layout.details.x + layout.details.w - 2.0F,
                            layout.details.y + 30.0F,
                            "Mass: " + std::to_string(mass));
      NovaText_DrawCentered(
          platform,
          font_cache,
          NovaFontFamily::kGeneva,
          9.0F,
          kNovaFontStyleRegular,
          kMuted,
          layout.details.x + 2.0F,
          layout.details.x + layout.details.w - 2.0F,
          layout.details.y + 45.0F,
          "Free: " + std::to_string(std::max(0, NovaLanded_FreeMass(state))));
      const auto lines = WrapDescriptionLines(
          selected_description, 28, [](std::string_view text) {
            return static_cast<int>(text.size());
          });
      for (std::size_t i = 0; i < std::min<std::size_t>(lines.size(), 16);
           ++i) {
        NovaText_DrawCentered(
            platform,
            font_cache,
            NovaFontFamily::kGeneva,
            9.0F,
            kNovaFontStyleRegular,
            kMuted,
            layout.description.x + 5.0F,
            layout.description.x + layout.description.w - 5.0F,
            layout.description.y + 39.0F + static_cast<float>(i) * 13.0F,
            lines[i]);
      }
    } else {
      // Ghidra NovaUi_DrawShipyardShipList 0x004948b0: the right-hand panels
      // show the desc text (DITL entry 6) and the Ship Price / Trade-In /
      // Final Price / You Have block (entry 9, values left-aligned at +0x46);
      // the full stat block lives in the Info sub-modal (0x00495c80).
      const ShipClass *ship = state.scenario.Ship(session.selected_id);
      const std::int32_t trade_in =
          NovaLanded_ShipTradeInValue(state, stellar_id);
      const std::int16_t player_class =
          static_cast<std::int16_t>(state.player.ship_class_id + 0x80);
      if (session.selected_id != player_class) {
        const auto price_row =
            [&](float dy, std::uint16_t label, std::int32_t value) {
              NovaText_Draw(platform,
                            font_cache,
                            NovaFontFamily::kGeneva,
                            10.0F,
                            kNovaFontStyleRegular,
                            kMuted,
                            layout.details.x + 2.0F,
                            layout.details.y + dy,
                            InfoString(label));
              NovaText_Draw(platform,
                            font_cache,
                            NovaFontFamily::kGeneva,
                            10.0F,
                            kNovaFontStyleRegular,
                            kText,
                            layout.details.x + 72.0F,
                            layout.details.y + dy,
                            GroupedUInt(value) + " " + InfoString(0x21));
            };
        price_row(12.0F, 0xe0, ship_price); // Ship Price:
        price_row(24.0F, 0xe1, trade_in);   // Trade-In:
        price_row(
            48.0F,
            0xe2,
            std::max<std::int32_t>(0, ship_price - trade_in)); // Final Price:
        price_row(72.0F, 0xd8, state.player.credits);          // You Have:
      }
      const std::string title_text =
          ship == nullptr ? std::string{} : ship->display_name;
      NovaText_DrawCentered(platform,
                            font_cache,
                            NovaFontFamily::kGeneva,
                            12.0F,
                            kNovaFontStyleBold,
                            kText,
                            layout.description.x + 4.0F,
                            layout.description.x + layout.description.w - 4.0F,
                            layout.description.y + 18.0F,
                            title_text);
      const auto lines = WrapDescriptionLines(
          selected_description, 28, [](std::string_view text) {
            return static_cast<int>(text.size());
          });
      for (std::size_t i = 0; i < std::min<std::size_t>(lines.size(), 16);
           ++i) {
        NovaText_DrawCentered(
            platform,
            font_cache,
            NovaFontFamily::kGeneva,
            9.0F,
            kNovaFontStyleRegular,
            kMuted,
            layout.description.x + 5.0F,
            layout.description.x + layout.description.w - 5.0F,
            layout.description.y + 39.0F + static_cast<float>(i) * 13.0F,
            lines[i]);
      }
    }
  }
  NovaText_DrawCentered(platform,
                        font_cache,
                        NovaFontFamily::kGeneva,
                        10.0F,
                        kNovaFontStyleRegular,
                        kMuted,
                        layout.description.x + 5.0F,
                        layout.description.x + layout.description.w - 5.0F,
                        layout.description.y + layout.description.h - 9.0F,
                        "Credits: " + std::to_string(state.player.credits));
}

// ---------------------------------------------------------------------------
// Ghidra 0x004956a0 NovaUi_RunShipyardDetailWindow + 0x00495c80
// NovaUi_DrawShipyardDetailPanel: the Shipyard's Info sub-modal.
//
// The window is DLOG 0x3ed (DITL 0x3ed, backdrop PICT 0x213a "Ship
// Description", 250x285) unless the selected ship's desc resource (ship class
// id + 13000) carries a Graphic PICT that actually loads, in which case it is
// DLOG 0x3fb (backdrop PICT 0x213b "Ship description + pict", 614x537) and the
// Graphic is blitted into DITL entry 7 (the shipped descs point at the 600x400
// PICT 20128+ set, matching the entry rect exactly).
struct ShipyardInfoLayout {
  SDL_FRect window{};
  SDL_FRect button{};  // entry 1: "Done" (STR# 0x96 entry 5)
  SDL_FRect title{};   // entry 3: ship display name
  SDL_FRect stats{};   // entry 5: two-column stat block
  SDL_FRect picture{}; // entry 7: desc Graphic PICT (custom variant only)
  SDL_FRect weapons{}; // entry 8: stock weapons block (custom variant only)
  bool custom_picture = false;
};

[[nodiscard]] ShipyardInfoLayout LayoutShipyardInfo(const SdlPlatform &platform,
                                                    bool custom_picture) {
  const SDL_FPoint output = platform.logical_playfield_size();
  const float width = custom_picture ? 614.0F : 250.0F;
  const float height = custom_picture ? 537.0F : 285.0F;
  const SDL_FPoint origin{std::max(0.0F, (output.x - width) / 2.0F),
                          std::max(0.0F, (output.y - height) / 2.0F)};
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
  // FreeMass: the original recomputes it per draw by re-subtracting the stock
  // outfit masses that the loader (0x004bd3c0 post-pass) folded into
  // ShipClassDef+0x4; that round trip always lands back on the payload value
  // clamped at zero, which the port keeps as free_mass.
  constexpr float kRightLabelDx = 130.0F;
  constexpr float kRightValueDx = 175.0F;
  row(stats_left + kRightLabelDx,
      stats_left + kRightValueDx,
      12.0F,
      InfoString(0xe9),
      quantity(std::max(0, static_cast<int>(ship->free_mass)), ton, tons));
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
  NovaText_DrawCentered(platform,
                        font_cache,
                        kThreeStateButtonFontFamily,
                        kThreeStateButtonFontSize,
                        kNovaFontStyleRegular,
                        kValue,
                        layout.button.x,
                        layout.button.x + layout.button.w,
                        ThreeStateButtonLabelBaseline(layout.button),
                        NovaHud_LoadStringEntry(0x96, 0x5).value_or("Done"));
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
                       SDL_Texture *docked_snapshot,
                       SDL_Texture *dock_backdrop,
                       SDL_Texture *store_frame,
                       std::string_view selected_description) {
  const bool outfit_store = session.kind == LandedStoreKind::kOutfitter;
  const StoreLayout layout = LayoutStore(platform, outfit_store);
  SDL_Texture *selected_image = StorePreviewTexture(
      platform, texture_cache, outfit_store, session.selected_id);
  DrawStoreBase(platform, docked_snapshot, dock_backdrop, store_frame, layout);
  DrawStoreContents(platform,
                    font_cache,
                    button_art,
                    state,
                    session,
                    stellar_id,
                    layout,
                    texture_cache,
                    selected_image,
                    selected_description);
}

// Ghidra 0x004956a0 NovaUi_RunShipyardDetailWindow: modal loop over the
// shipyard store. The window closes on the Done button (the original's only
// exit) or Escape. TODO(decomp(0x004956a0)) skipped: the original re-enters
// the starmap (action 4), player special interaction (action 2) and mission
// computer (action 6) windows from inside this loop; the store modal does not
// host those nested interactions yet, so the Info window only offers Done.
void RunShipyardInfoDialog(SdlPlatform &platform,
                           GameState &state,
                           const LandedStoreSession &session,
                           std::int16_t stellar_id,
                           SDL_Texture *docked_snapshot,
                           SDL_Texture *dock_backdrop,
                           SDL_Texture *store_frame,
                           StoreTextureCache &texture_cache,
                           NovaFontCache &font_cache,
                           const ServicesButtonArt &button_art,
                           std::string_view selected_description) {
  std::unique_ptr<SdlTexture> custom_picture;
  bool custom = false;
  std::uint16_t backdrop_pict = 0x213a;
  if (session.selected_id >= 0x80) {
    // The desc key is the zero-based ship class + 13000 (the original's
    // g_shipyard_selected_ship_class_id is the 0-based defs index; the port's
    // selected_id is the raw 0x80-based resource id).
    if (const auto desc = NovaResource_LoadDescription(
            static_cast<std::uint16_t>(session.selected_id - 0x80 + 13000));
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
                      docked_snapshot,
                      dock_backdrop,
                      store_frame,
                      selected_description);
    const ShipyardInfoLayout layout = LayoutShipyardInfo(platform, custom);
    DrawShipyardInfoPanel(platform,
                          font_cache,
                          button_art,
                          state,
                          session,
                          layout,
                          backdrop ? backdrop->get() : nullptr,
                          custom_picture ? custom_picture->get() : nullptr);
    SDL_RenderPresent(platform.renderer());
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
    SDL_Delay(16);
  }
}

// Ghidra 0x0048ea70 NovaUi_RunTravelOutfitInteractionLoop and
// 0x00492f30 NovaUi_RunShipyardPurchaseLoop: one generic store loop replaces
// both (service is a parameter). The 0x00493fc0 NovaUi_ShipyardHandleSelection
// Input / 0x0049f3f0 HitTestAndTrackShipyardActionButtons /
// 0x0049f6f0 DrawShipyardActionButtons / 0x0049f8f0
// HitTestAndTrackTravelOutfitActionButtons / 0x0049fbb0
// DrawTravelOutfitActionButtons and the 0x00497b70 BlitPictThumbnailCached
// cache all run inline within this function and the DrawStore* helpers below;
// the 0x004956a0/0x00495c80 detail window runs in RunShipyardInfoDialog above
// (opened by the Info action button).
LandedExit RunStoreDialog(SdlPlatform &platform,
                          GameState &state,
                          LandedService service,
                          std::int16_t stellar_id,
                          SDL_Texture *docked_snapshot) {
  const bool outfit_store = service == LandedService::kOutfit;
  LandedStoreSession session =
      outfit_store ? NovaLanded_OpenOutfitterSession(state, stellar_id)
                   : NovaLanded_OpenShipyardSession(state, stellar_id);
  auto backdrop = LoadPictTexture(platform, kDockedBackdropPict);
  auto frame =
      LoadPictTexture(platform, NovaDocked_SubWindowFramePict(service));
  SDL_Texture *snapshot = docked_snapshot;
  StoreTextureCache texture_cache;
  std::string selected_description;
  std::int16_t selected_description_id = -1;
  ServicesButtonArt button_art;
  (void)button_art.Initialize(platform);
  NovaFontCache font_cache;
  while (!platform.quit_requested()) {
    const StoreLayout layout = LayoutStore(platform, outfit_store);
    if (session.selected_id != selected_description_id) {
      selected_description.clear();
      if (session.selected_id >= 0x80) {
        // The selection-desc resource is keyed at the zero-based item index
        // plus the family base: outfits (DLOG 0x3ea) at +3000 (Ghidra
        // NovaUi_HandleTravelOutfitMenuInput 0x004903c0), ship classes at
        // +13000 (NovaUi_ShipyardHandleSelectionInput 0x00493fc0). The port's
        // selected_id is the raw 0x80+ resource id, so subtract 0x80 first.
        const auto desc_id = static_cast<std::uint16_t>(
            (outfit_store ? 3000 : 13000) + (session.selected_id - 0x80));
        if (const auto description = NovaResource_LoadDescription(desc_id)) {
          selected_description = description->text;
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
                      snapshot,
                      backdrop ? backdrop->get() : nullptr,
                      frame ? frame->get() : nullptr,
                      selected_description);
    SDL_RenderPresent(platform.renderer());
    for (std::optional<TextInput> input; (input = platform.PollTextEvent());) {
      if (input->key == TextKey::escape) {
        if (outfit_store)
          NovaLanded_CloseOutfitterSession(state);
        return LandedExit::kServiceComplete;
      }
      if (input->key == TextKey::character) {
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
          if (outfit_store) {
            (void)NovaLanded_BuyOutfit(
                state, stellar_id, session.selected_id, 1);
            NovaLanded_RefreshStoreSession(state, session, stellar_id);
          } else {
            const ShipClass *ship = state.scenario.Ship(session.selected_id);
            (void)NovaLanded_ReplacePlayerShip(
                state,
                stellar_id,
                session.selected_id,
                ship == nullptr ? "" : ship->short_name);
            session = NovaLanded_OpenShipyardSession(state, stellar_id);
          }
          continue;
        }
        if (key == 's' && outfit_store && session.selected_id >= 0) {
          (void)NovaLanded_SellOutfit(
              state, session, stellar_id, session.selected_id, 1);
          NovaLanded_RefreshStoreSession(state, session, stellar_id);
          continue;
        }
        if (key == 'i' && !outfit_store && session.selected_id >= 0) {
          RunShipyardInfoDialog(platform,
                                state,
                                session,
                                stellar_id,
                                snapshot,
                                backdrop ? backdrop->get() : nullptr,
                                frame ? frame->get() : nullptr,
                                texture_cache,
                                font_cache,
                                button_art,
                                selected_description);
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
      if (Contains(layout.buy, point) && session.selected_id >= 0) {
        if (outfit_store) {
          (void)NovaLanded_BuyOutfit(state, stellar_id, session.selected_id, 1);
          NovaLanded_RefreshStoreSession(state, session, stellar_id);
        } else {
          const ShipClass *ship = state.scenario.Ship(session.selected_id);
          (void)NovaLanded_ReplacePlayerShip(
              state,
              stellar_id,
              session.selected_id,
              ship == nullptr ? "" : ship->short_name);
          session = NovaLanded_OpenShipyardSession(state, stellar_id);
        }
        continue;
      }
      if (Contains(layout.sell_or_info, point) && session.selected_id >= 0) {
        if (outfit_store) {
          (void)NovaLanded_SellOutfit(
              state, session, stellar_id, session.selected_id, 1);
          NovaLanded_RefreshStoreSession(state, session, stellar_id);
        } else {
          // The shipyard's third action button is Info
          // (0x0049f3f0 HitTestAndTrackShipyardActionButtons index 2), which
          // opens the detail window (0x004956a0).
          RunShipyardInfoDialog(platform,
                                state,
                                session,
                                stellar_id,
                                snapshot,
                                backdrop ? backdrop->get() : nullptr,
                                frame ? frame->get() : nullptr,
                                texture_cache,
                                font_cache,
                                button_art,
                                selected_description);
        }
      }
    }
    SDL_Delay(16);
  }
  return LandedExit::kQuit;
}

} // namespace

std::unique_ptr<SdlTexture>
NovaLanded_CaptureDockedBackground(SdlPlatform &platform) {
  return CaptureDockedBackground(platform.renderer());
}

std::uint16_t NovaDocked_SubWindowFramePict(LandedService service) {
  switch (service) {
  case LandedService::kShipyard:
    return 0x2135;
  case LandedService::kOutfit:
    return 0x2136;
  case LandedService::kBar:
    return 0x2137;
  case LandedService::kMissionBoard:
    return 0x2139;
  case LandedService::kBuySellCargo:
    return 0x213e;
  // Communications (0x213f) has no docked slot in the MVP; never reached.
  default:
    return 0;
  }
}

LandedExit NovaLanded_RunSubWindowDialog(SdlPlatform &platform,
                                         GameState &state,
                                         LandedService service,
                                         std::int16_t stellar_id,
                                         SDL_Texture *docked_snapshot) {
  if (service == LandedService::kMissionBoard) {
    return RunMissionBoardDialog(platform, state, stellar_id, docked_snapshot);
  }
  if (service == LandedService::kOutfit ||
      service == LandedService::kShipyard) {
    return RunStoreDialog(
        platform, state, service, stellar_id, docked_snapshot);
  }
  NovaLog::Info("opening docked sub-window dialog '{}' at stellar {}",
                SubWindowHeading(service),
                static_cast<int>(stellar_id));

  const std::uint16_t frame_id = NovaDocked_SubWindowFramePict(service);
  if (frame_id == 0) {
    NovaLog::Todo("service '{}' has no sub-window frame art; returning to the "
                  "dock menu",
                  SubWindowHeading(service));
    return LandedExit::kServiceComplete;
  }

  // Load the dialog artwork: the docked backdrop re-layered underneath and the
  // service's sub-window frame PICT.
  auto backdrop = LoadPictTexture(platform, kDockedBackdropPict);
  if (!backdrop) {
    NovaLog::Todo("docked backdrop PICT 0x2134 unavailable for sub-window "
                  "dialog; drawing a flat dim background");
  }
  auto frame = LoadPictTexture(platform, frame_id);
  if (!frame) {
    NovaLog::Todo("sub-window frame PICT 0x{:04x} unavailable for '{}'; "
                  "drawing a bordered placeholder",
                  frame_id,
                  SubWindowHeading(service));
  }

  ServicesButtonArt button_art;
  if (!button_art.Initialize(platform)) {
    NovaLog::Warn("service button art unavailable for the sub-window Leave "
                  "button");
  }
  NovaFontCache font_cache;
  const SDL_FRect panel{0.0F, 0.0F, 640.0F, 480.0F};
  platform.SetCenteredPlayfield();

  while (!platform.quit_requested()) {
    // Draw the dialog frame over the re-layered docked backdrop.
    DrawSubWindowDialog(platform,
                        font_cache,
                        button_art,
                        backdrop ? backdrop->get() : nullptr,
                        frame ? frame->get() : nullptr,
                        service,
                        panel);
    SDL_RenderPresent(platform.renderer());

    // Close the dialog on Esc / Enter / a left click or primary press
    // anywhere on the window (the original's sub-windows close via their
    // Leave control / window close, with Esc as the universal cancel).
    for (std::optional<TextInput> in; (in = platform.PollTextEvent());) {
      switch (in->key) {
      case TextKey::escape:
      case TextKey::enter:
      case TextKey::primary:
        return LandedExit::kServiceComplete;
      default:
        break;
      }
    }
    SDL_Delay(16);
  }
  return LandedExit::kQuit;
}

// ---------------------------------------------------------------------------
// Ghidra 0x00442510 NovaUi_RunMissionShipInteractionWindow (partial port: the
// text-offer arm). Window DLOG 0x3f8 (DITL 1016: entry 1 = accept button,
// entry 2 = decline button, entry 3 = read-only text view); background =
// PICT 0x214a (main art, top-anchored) with strips 0x2149 (top) and 0x214b
// (bottom). Button captions come from the mïsn payload +0x75f/+0x77f, which
// the original truncates at the first non-lowercase byte and replaces with
// STR# 0x96 entries 0x32/0x1b (accept) and 0x33 (decline) when empty.
namespace {

// Fills the reader text the way the original's callers fill
// g_selection_dialog_text: desc load (which runs the placeholder pass at
// load time, 0x004c6d50) + the wildcard pass. `active_slot >= 0` selects the
// active arm of the wildcard pass (mission_id = active slot).
[[nodiscard]] std::string LoadMissionText(const GameState &state,
                                          std::uint16_t desc_id,
                                          bool offering_arm,
                                          std::int16_t mission_id) {
  std::string text;
  if (const auto desc = NovaResource_LoadDescription(desc_id)) {
    text = desc->text;
    Mission_ExpandStringPlaceholders(state, text);
    text =
        Mission_ExpandMissionWildcards(state, text, offering_arm, mission_id);
  }
  return text;
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
// Mission_ActivateAtSlot state-only, so this UI slice lives here and every
// accept path (mission-offer window, Mission BBS) invokes it.
void NovaMission_RunAcceptanceDialogs(SdlPlatform &platform,
                                      GameState &state,
                                      std::int16_t mission_def,
                                      SDL_Texture *docked_snapshot) {
  const MissionDef *def =
      state.scenario.Mission(static_cast<std::int16_t>(mission_def + 0x80));
  if (def == nullptr) {
    return;
  }
  const std::optional<std::size_t> slot =
      FindActiveMissionSlot(state, mission_def);
  if (def->initial_briefing_id >= 0x80) {
    const std::string text =
        LoadMissionText(state,
                        static_cast<std::uint16_t>(def->initial_briefing_id),
                        false,
                        slot ? static_cast<std::int16_t>(*slot) : -1);
    if (!text.empty()) {
      NovaUi_RunTextReaderDialog(platform, state, text, true, docked_snapshot);
    }
  }
  if (def->pickup_mode == 0 && def->text_description_ids[2] >= 0x80) {
    const std::string text = LoadMissionText(
        state,
        static_cast<std::uint16_t>(def->text_description_ids[2]),
        false,
        slot ? static_cast<std::int16_t>(*slot) : -1);
    if (!text.empty()) {
      NovaUi_RunTextReaderDialog(platform, state, text, false, docked_snapshot);
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

MissionOfferResult NovaMission_RunOfferWindow(SdlPlatform &platform,
                                              GameState &state,
                                              std::int16_t mission_def,
                                              std::int16_t landed_stellar_id,
                                              SDL_Texture *docked_snapshot) {
  if (mission_def < 0 || mission_def >= 1000) {
    return MissionOfferResult::kDeclined;
  }
  const MissionDef *def =
      state.scenario.Mission(static_cast<std::int16_t>(mission_def + 0x80));
  if (def == nullptr) {
    return MissionOfferResult::kDeclined;
  }
  // DAT_00773ee9: (MisnDef +0x18 & 4) == 0. When the bit is set and the offer
  // text is empty the original activates the mission without showing a window.
  const bool normal_arm = (def->scan_mask & 4) == 0;

  // dësc (def + 4000): the original expands the {g}/{G}/{p}/{b} placeholder
  // blocks at load (Ui_LoadSelectionDialogResource 0x004c6d50 ->
  // Ship_ExpandStringPlaceholders 0x0044a4d0), then the wildcard pass composes
  // the final offer text (Stellar_BuildTravelDestinationDescription).
  std::string text;
  const std::uint16_t desc_id = static_cast<std::uint16_t>(mission_def + 4000);
  if (const auto desc = NovaResource_LoadDescription(desc_id)) {
    text = desc->text;
    Mission_ExpandStringPlaceholders(state, text);
    text = Mission_ExpandMissionWildcards(state, text, true, mission_def);
  }
  if (!normal_arm && text.empty()) {
    if (!Mission_ActivateAtSlot(state, mission_def, landed_stellar_id)) {
      return MissionOfferResult::kActivationFailed;
    }
    NovaMission_RunAcceptanceDialogs(
        platform, state, mission_def, docked_snapshot);
    return MissionOfferResult::kAccepted;
  }

  const auto dlog = NovaResource_LoadDialogDefinition(0x3f8);
  const auto items =
      dlog ? NovaResource_LoadDialogItems(dlog->dialog_item_list_id)
           : std::nullopt;
  if (!dlog || !items) {
    // The original bails with return 0 (declined) when the window resource is
    // unusable.
    NovaLog::Todo("mission offer DLOG/DITL 0x3f8 unavailable; declining offer "
                  "for misn def {}",
                  mission_def);
    return MissionOfferResult::kDeclined;
  }
  // TODO(decomp(0x00442510)) skipped: variant >= 0x80 switches to DLOG 0x3fc
  // + PICT 0x2150. dësc 4123 (the tutorial offer) has variant 0.
  if (const auto desc = NovaResource_LoadDescription(desc_id);
      desc && desc->dialog_variant >= 0x80) {
    NovaLog::Todo("mission offer dësc {} uses the >= 0x80 art variant "
                  "(DLOG 0x3fc path); rendering the 0x3f8 window instead",
                  desc_id);
  }

  const float win_w = static_cast<float>(dlog->right - dlog->left);
  const float win_h = static_cast<float>(dlog->bottom - dlog->top);
  const SDL_FPoint output = platform.logical_playfield_size();
  const SDL_FPoint origin{(output.x - win_w) / 2.0F, (output.y - win_h) / 2.0F};
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
  const SDL_FRect text_rect = item_rect(2);    // UiPanel entry 3: text view
  // Entries 9/10 (DITL items 8/9) are the text-view scroll arrows. Their
  // glyphs are part of the bottom-strip art (PICT 0x214b); only the hit-test
  // rects are used here. Action 9 (entry 9) = scroll +10px (down), action 10
  // (entry 10) = -10px (up) in NovaUi_ScrollSelectionText.
  const SDL_FRect scroll_down_rect = item_rect(8);
  const SDL_FRect scroll_up_rect = item_rect(9);

  // Button captions: payload +0x75f/+0x77f C-strings truncated at the first
  // non-lowercase byte (0x00442510 caption-normalisation loop), else the STR#
  // 0x96 defaults (0x32 "Yes", or 0x1b "Okay" in the +0x18-&4 arm; 0x33 "No").
  const auto payload_caption = [&](std::size_t offset) {
    const auto *bytes = def->raw_payload.data();
    std::string out;
    for (std::size_t i = offset; i < def->raw_payload.size(); ++i) {
      const char c = static_cast<char>(std::to_integer<unsigned>(bytes[i]));
      if (c < 'a' || c > 'z') {
        break;
      }
      out += c;
    }
    return out;
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
  auto art_main = LoadPictTexture(platform, 0x214a);
  auto art_top = LoadPictTexture(platform, 0x2149);
  auto art_bottom = LoadPictTexture(platform, 0x214b);
  if (art_main == nullptr || art_top == nullptr || art_bottom == nullptr) {
    NovaLog::Todo("mission offer window art PICTs 0x2149/0x214a/0x214b "
                  "incomplete; using a flat window fill");
  }
  ServicesButtonArt button_art;
  (void)button_art.Initialize(platform);
  NovaFontCache font_cache;
  // Shared read-only text view (NovaTextView 0x004bcd90) over DITL entry 3.
  NovaTextScrollView view(font_cache, text, text_rect);
  // One window frame over the docked backing store. The original's draw
  // callback (NovaUi_DrawTravelOutfitMenu 0x00447680) fills the window, blits
  // the main art top-anchored (clipped), then the top and bottom strips.
  auto draw_frame = [&]() {
    platform.SetFullscreenPlayfield();
    SDL_SetRenderDrawColor(platform.renderer(), 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(platform.renderer());
    if (docked_snapshot != nullptr) {
      const SDL_FRect snapshot_rect{0.0F, 0.0F, output.x, output.y};
      SDL_RenderTexture(
          platform.renderer(), docked_snapshot, nullptr, &snapshot_rect);
    } else if (backdrop != nullptr) {
      const SDL_FRect backdrop_rect{origin.x - (640.0F - win_w) / 2.0F,
                                    origin.y - (480.0F - win_h) / 2.0F,
                                    640.0F,
                                    480.0F};
      SDL_RenderTexture(
          platform.renderer(), backdrop->get(), nullptr, &backdrop_rect);
    }
    const SDL_FRect window{origin.x, origin.y, win_w, win_h};
    SDL_SetRenderDrawColor(platform.renderer(), 16, 40, 72, SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(platform.renderer(), &window);
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

    // Text view: dark fill + wrapped offer text, scrolled inside a clip to
    // the view rect; the arrow buttons are runtime-drawn (0x004a1820).
    view.Draw(platform);
    NovaUi_DrawScrollArrow(
        platform, scroll_up_rect, true, view.scroll_offset() > 0.0F);
    NovaUi_DrawScrollArrow(platform,
                           scroll_down_rect,
                           false,
                           view.scroll_offset() < view.max_scroll());

    for (const auto &[rect, caption] :
         std::array<std::pair<SDL_FRect, const std::string &>, 2>{
             {{accept_rect, accept_caption},
              {decline_rect, decline_caption}}}) {
      button_art.Draw(platform, rect, ButtonState::kNormal);
      NovaText_DrawCentered(platform,
                            font_cache,
                            kThreeStateButtonFontFamily,
                            kThreeStateButtonFontSize,
                            kNovaFontStyleRegular,
                            SDL_Color{255, 255, 255, 255},
                            rect.x,
                            rect.x + rect.w,
                            ThreeStateButtonLabelBaseline(rect),
                            caption);
    }
    SDL_RenderPresent(platform.renderer());
  };

  draw_frame();
  while (!platform.quit_requested()) {
    for (std::optional<TextInput> input; (input = platform.PollTextEvent());) {
      if (input->key == TextKey::escape) {
        // Divergence: the original window exits only through its two buttons;
        // Esc is the port's universal modal cancel and counts as a decline.
        NovaLog::Todo("offer window Esc counts as decline; the original has "
                      "no Esc exit (0x00442510)");
        return MissionOfferResult::kDeclined;
      }
      if (input->key == TextKey::primary) {
        const SDL_FPoint point = platform.mouse_position();
        if (Contains(accept_rect, point)) {
          if (!Mission_ActivateAtSlot(state, mission_def, landed_stellar_id)) {
            return MissionOfferResult::kActivationFailed;
          }
          // 0x00442510's accept arm activates, then Mission_ActivateMission-
          // AtSlot (0x0043f100) shows the Brief/LoadCarg dialogs inline.
          NovaMission_RunAcceptanceDialogs(
              platform, state, mission_def, docked_snapshot);
          return MissionOfferResult::kAccepted;
        }
        if (Contains(decline_rect, point)) {
          // 0x00442510's decline arm: the payload +0x58 desc (slot_aux_text)
          // opens the text reader when present, then the decline reaction
          // script (payload +0x25a) runs either way. The original composes
          // the follow-up text with the active-arm wildcard pass against
          // slot -1 (nothing resolves); the port keeps the load-time
          // placeholder pass only.
          if (def->slot_aux_text_id >= 0x80) {
            const std::string followup_text = LoadMissionText(
                state,
                static_cast<std::uint16_t>(def->slot_aux_text_id),
                false,
                -1);
            if (!followup_text.empty()) {
              NovaUi_RunTextReaderDialog(
                  platform, state, followup_text, false, docked_snapshot);
            }
          }
          (void)Mission_ExecuteReactionScript(state,
                                              PayloadCString(*def, 0x25a));
          return MissionOfferResult::kDeclined;
        }
        // Arrow buttons (entries 9/10), NovaUi_ScrollSelectionText ±10px per
        // action in 0x00442510. The original gates action 9 on the maxed
        // latch and action 10 on the scrolled latch; the clamp covers both.
        if (Contains(scroll_down_rect, point)) {
          view.ScrollBy(10.0F);
        } else if (Contains(scroll_up_rect, point)) {
          view.ScrollBy(-10.0F);
        }
        continue;
      }
      if (input->key == TextKey::physical) {
        // Port convenience: DIK arrows scroll the view (the original scrolls
        // only via the two arrow buttons).
        if (input->key_code == 0xc8) { // DIK_UP
          view.ScrollBy(-10.0F);
        } else if (input->key_code == 0xd0) { // DIK_DOWN
          view.ScrollBy(10.0F);
        }
      }
    }
    draw_frame();
    SDL_Delay(16);
  }
  return MissionOfferResult::kDeclined;
}

} // namespace game
