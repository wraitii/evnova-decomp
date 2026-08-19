#include "docked_dialog.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../pict_image.hpp"
#include "../sdl_platform.hpp"
#include "landed_store.hpp"
#include "mission.hpp"
#include "nova_font.hpp"
#include "hud_overlay.hpp"
#include "scenario_data.hpp"
#include "services_buttons.hpp"
#include "ship_visual.hpp"
#include "sprite_world.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
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
  const auto items = definition
                         ? NovaResource_LoadDialogItems(definition->dialog_item_list_id)
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
  const std::string heading = NovaHud_LoadStringEntry(0x7d2, 0x167)
                                  .value_or(
                                      "The following missions are available here");
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
    for (std::size_t row = 0; row < rows.size() &&
                              layout.list.y + row * row_pitch <
                                  layout.list.y + layout.list.h;
         ++row) {
      const auto mission_id = rows[row];
      const auto *definition = state.scenario.Mission(
          static_cast<std::int16_t>(mission_id + 0x80));
      const std::string label =
          definition != nullptr && !definition->display_name.empty()
              ? definition->display_name
              : "Mission " + std::to_string(mission_id);
      const float row_top = layout.list.y + row * row_pitch;
      const float baseline = row_top + kMissionListFontSize;
      const SDL_FRect row_rect{layout.list.x, row_top, layout.list.w,
                               row_pitch};
      const SDL_Color row_color = row == selected ? kRowSelected : kRowNormal;
      SDL_SetRenderDrawColor(renderer,
                             row_color.r,
                             row_color.g,
                             row_color.b,
                             row_color.a);
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

  if (layout.selected_title.w > 0.0F && !rows.empty() && selected < rows.size()) {
    const auto *definition = state.scenario.Mission(
        static_cast<std::int16_t>(rows[selected] + 0x80));
    const std::string title = definition != nullptr
                                  ? definition->display_name
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
      const float x = layout.description.x + 6.0F;
      const float width = layout.description.w - 12.0F;
      const auto lines = WrapDescriptionLines(
          description->text,
          static_cast<int>(std::max(1.0F, width)),
          [&](std::string_view line) {
            return font_cache.TextWidth(NovaFontFamily::kGeneva,
                                         12.0F,
                                         kNovaFontStyleRegular,
                                         line);
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
    const SDL_FRect backdrop_rect{(output.x - width) / 2.0F,
                                  (output.y - height) / 2.0F,
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
  NovaLog::Info("mission BBS opened at stellar {}: {} available rows (first id {})",
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
      if (Mission_ActivateAtSlot(state, mission_id)) {
        status = "Mission accepted";
        missions = Mission_EvaluateMissionLists(state);
        if (selected >= missions.page_zero.size() && !missions.page_zero.empty()) {
          selected = missions.page_zero.size() - 1;
        }
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
          const auto row = static_cast<std::size_t>(
              (point.y - layout->list.y) / layout->list_row_pitch);
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
            : NovaLanded_ShipPurchasePrice(
                  state, stellar_id, session.selected_id);
    if (selected_image != nullptr) {
      SDL_RenderTexture(renderer, selected_image, nullptr, &layout.preview);
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
      const ShipClass *ship = state.scenario.Ship(session.selected_id);
      const std::array<std::string, 6> lines{
          "Price: " + std::to_string(price),
          "Shields: " + std::to_string(ship->base_shield),
          "Armor: " + std::to_string(ship->base_armor),
          "Cargo: " + std::to_string(ship->cargo_holds),
          "Mass: " + std::to_string(ship->mass_tons),
          "Crew: " + std::to_string(ship->crew)};
      for (std::size_t i = 0; i < lines.size(); ++i) {
        NovaText_DrawCentered(platform,
                              font_cache,
                              NovaFontFamily::kGeneva,
                              9.0F,
                              kNovaFontStyleRegular,
                              kMuted,
                              layout.details.x + 2.0F,
                              layout.details.x + layout.details.w - 2.0F,
                              layout.details.y + 14.0F +
                                  static_cast<float>(i) * 14.0F,
                              lines[i]);
      }
      const std::array<std::string, 5> comparison{
          "Trade-in: " +
              std::to_string(NovaLanded_ShipTradeInValue(state, stellar_id)),
          "Speed: " + std::to_string(static_cast<int>(ship->speed)),
          "Acceleration: " + std::to_string(static_cast<int>(ship->accel)),
          "Maneuver: " + std::to_string(static_cast<int>(ship->turn_rate)),
          "Fuel: " + std::to_string(ship->base_fuel)};
      for (std::size_t i = 0; i < comparison.size(); ++i) {
        NovaText_DrawCentered(
            platform,
            font_cache,
            NovaFontFamily::kGeneva,
            10.0F,
            i == 0 ? kNovaFontStyleBold : kNovaFontStyleRegular,
            kMuted,
            layout.description.x + 5.0F,
            layout.description.x + layout.description.w - 5.0F,
            layout.description.y + 48.0F + static_cast<float>(i) * 22.0F,
            comparison[i]);
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
    SDL_Texture *selected_image = StorePreviewTexture(
        platform, texture_cache, outfit_store, session.selected_id);
    if (outfit_store && session.selected_id != selected_description_id) {
      selected_description.clear();
      if (session.selected_id >= 0x80) {
        // The Outfitter (DLOG 0x3ea) keys the outfit 'd\x91sc' description
        // resource at its zero-based outfit index + 3000 (Ghidra
        // NovaUi_HandleTravelOutfitMenuInput 0x004903c0 calls
        // Ui_LoadSelectionDialogResource(..., g_travel_outfit_selected_id +
        // 3000) where that selected id is 0-based). selected_id here is the
        // raw 0x80+ resource id, so subtract the 0x80 base before the offset.
        const auto desc_id =
            static_cast<std::uint16_t>(session.selected_id - 0x80 + 3000);
        if (const auto description = NovaResource_LoadDescription(desc_id)) {
          selected_description = description->text;
        }
      }
      selected_description_id = session.selected_id;
    }
    DrawStoreBase(platform,
                  snapshot,
                  backdrop ? backdrop->get() : nullptr,
                  frame ? frame->get() : nullptr,
                  layout);
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
      if (outfit_store && Contains(layout.sell_or_info, point) &&
          session.selected_id >= 0) {
        (void)NovaLanded_SellOutfit(
            state, session, stellar_id, session.selected_id, 1);
        NovaLanded_RefreshStoreSession(state, session, stellar_id);
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
    return RunMissionBoardDialog(
        platform, state, stellar_id, docked_snapshot);
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

} // namespace game
