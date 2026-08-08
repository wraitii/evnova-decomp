#include "docked_dialog.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../pict_image.hpp"
#include "../sdl_platform.hpp"
#include "nova_font.hpp"
#include "landed_store.hpp"
#include "scenario_data.hpp"
#include "services_buttons.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

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
constexpr SDL_Color kTitle{202, 224, 255, 255};
constexpr SDL_Color kDim{128, 170, 210, 255};

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
                          NovaFontFamily::kGeneva,
                          12.0F,
                          kNovaFontStyleBold,
                          kDim,
                          leaveRect.x,
                          leaveRect.x + leaveRect.w,
                          leaveRect.y + leaveRect.h / 2.0F + 4.0F,
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
                        NovaFontFamily::kGeneva,
                        12.0F,
                        kNovaFontStyleBold,
                        kDim,
                        leaveRect.x,
                        leaveRect.x + leaveRect.w,
                        leaveRect.y + leaveRect.h / 2.0F + 4.0F,
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

[[nodiscard]] bool Contains(const SDL_FRect &rect, SDL_FPoint point) {
  return point.x >= rect.x && point.x < rect.x + rect.w && point.y >= rect.y &&
         point.y < rect.y + rect.h;
}

void DrawStoreContents(SdlPlatform &platform, NovaFontCache &font_cache,
                       const ServicesButtonArt &button_art, const GameState &state,
                       const LandedStoreSession &session, std::int16_t stellar_id) {
  constexpr SDL_Color kText{202, 224, 255, 255};
  constexpr SDL_Color kMuted{128, 170, 210, 255};
  const bool outfit_store = session.kind == LandedStoreKind::kOutfitter;
  constexpr float kGridX = 48.0F;
  constexpr float kGridY = 122.0F;
  constexpr float kCellW = 108.0F;
  constexpr float kCellH = 37.0F;
  SDL_Renderer *renderer = platform.renderer();
  for (std::size_t slot = 0; slot < LandedStoreSession::kPageSlots; ++slot) {
    const std::size_t index = session.page_base + slot;
    if (index >= session.available_ids.size()) break;
    const std::int16_t id = session.available_ids[index];
    const std::size_t col = slot % 5;
    const std::size_t row = slot / 5;
    const SDL_FRect rect{kGridX + static_cast<float>(col) * kCellW,
                         kGridY + static_cast<float>(row) * kCellH,
                         kCellW - 3.0F, kCellH - 3.0F};
    const bool selected = id == session.selected_id;
    SDL_SetRenderDrawColor(renderer, selected ? 66 : 16, selected ? 108 : 40,
                           selected ? 154 : 72, SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(renderer, &rect);
    SDL_SetRenderDrawColor(renderer, selected ? 220 : 80, selected ? 235 : 140,
                           selected ? 255 : 190, SDL_ALPHA_OPAQUE);
    SDL_RenderRect(renderer, &rect);
    const std::string_view name = outfit_store
                                      ? std::string_view{state.scenario.Outfit(id)->short_name}
                                      : std::string_view{state.scenario.Ship(id)->short_name};
    NovaText_DrawCentered(platform, font_cache, NovaFontFamily::kGeneva, 10.0F,
                          kNovaFontStyleRegular, kText, rect.x + 2.0F,
                          rect.x + rect.w - 2.0F, rect.y + 15.0F, name);
    if (outfit_store) {
      const std::int16_t count = state.inventory.outfit_owned_count[id - 0x80];
      NovaText_DrawCentered(platform, font_cache, NovaFontFamily::kGeneva, 9.0F,
                            kNovaFontStyleRegular, kMuted, rect.x, rect.x + rect.w,
                            rect.y + 29.0F, std::to_string(count));
    }
  }
  const SDL_FRect leave{55.0F, 400.0F, 90.0F, 25.0F};
  const SDL_FRect buy{165.0F, 400.0F, 90.0F, 25.0F};
  const SDL_FRect sell{275.0F, 400.0F, 90.0F, 25.0F};
  const SDL_FRect previous{385.0F, 400.0F, 90.0F, 25.0F};
  const SDL_FRect next{495.0F, 400.0F, 90.0F, 25.0F};
  const std::array<std::pair<SDL_FRect, std::string_view>, 5> controls{{
      {leave, "LEAVE"}, {buy, "BUY"}, {sell, outfit_store ? "SELL" : "INFO"},
      {previous, "PREVIOUS"}, {next, "NEXT"}}};
  for (const auto &[rect, label] : controls) {
    button_art.Draw(platform, rect, ButtonState::kNormal);
    NovaText_DrawCentered(platform, font_cache, NovaFontFamily::kGeneva, 11.0F,
                          kNovaFontStyleBold, kText, rect.x, rect.x + rect.w,
                          rect.y + 16.0F, label);
  }
  if (session.selected_id >= 0) {
    const std::string title = outfit_store
        ? state.scenario.Outfit(session.selected_id)->name
        : state.scenario.Ship(session.selected_id)->display_name;
    const std::int32_t price = outfit_store
        ? NovaLanded_OutfitPrice(state, stellar_id, session.selected_id)
        : NovaLanded_ShipPurchasePrice(state, stellar_id, session.selected_id);
    NovaText_DrawCentered(platform, font_cache, NovaFontFamily::kGeneva, 12.0F,
                          kNovaFontStyleBold, kText, 35.0F, 605.0F, 310.0F, title);
    NovaText_DrawCentered(platform, font_cache, NovaFontFamily::kGeneva, 11.0F,
                          kNovaFontStyleRegular, kMuted, 35.0F, 605.0F, 327.0F,
                          std::to_string(price) + " credits");
  }
  NovaText_DrawCentered(platform, font_cache, NovaFontFamily::kGeneva, 10.0F,
                        kNovaFontStyleRegular, kMuted, 35.0F, 605.0F, 365.0F,
                        "Credits: " + std::to_string(state.player.credits));
}

LandedExit RunStoreDialog(SdlPlatform &platform, GameState &state,
                          LandedService service, std::int16_t stellar_id) {
  const bool outfit_store = service == LandedService::kOutfit;
  LandedStoreSession session = outfit_store
      ? NovaLanded_OpenOutfitterSession(state, stellar_id)
      : NovaLanded_OpenShipyardSession(state, stellar_id);
  auto backdrop = LoadPictTexture(platform, kDockedBackdropPict);
  auto frame = LoadPictTexture(platform, NovaDocked_SubWindowFramePict(service));
  ServicesButtonArt button_art;
  (void)button_art.Initialize(platform);
  NovaFontCache font_cache;
  const SDL_FRect panel{0.0F, 0.0F, 640.0F, 480.0F};
  constexpr SDL_FRect kGrid{48.0F, 122.0F, 5.0F * 108.0F, 4.0F * 37.0F};
  constexpr SDL_FRect kLeave{55.0F, 400.0F, 90.0F, 25.0F};
  constexpr SDL_FRect kBuy{165.0F, 400.0F, 90.0F, 25.0F};
  constexpr SDL_FRect kSell{275.0F, 400.0F, 90.0F, 25.0F};
  constexpr SDL_FRect kPrevious{385.0F, 400.0F, 90.0F, 25.0F};
  constexpr SDL_FRect kNext{495.0F, 400.0F, 90.0F, 25.0F};
  while (!platform.quit_requested()) {
    DrawSubWindowDialog(platform, font_cache, button_art,
                        backdrop ? backdrop->get() : nullptr,
                        frame ? frame->get() : nullptr, service, panel);
    DrawStoreContents(platform, font_cache, button_art, state, session, stellar_id);
    SDL_RenderPresent(platform.renderer());
    for (std::optional<TextInput> input; (input = platform.PollTextEvent());) {
      if (input->key == TextKey::escape || input->key == TextKey::enter) return LandedExit::kServiceComplete;
      if (input->key != TextKey::primary) continue;
      const SDL_FPoint point = platform.mouse_position();
      if (Contains(kLeave, point)) return LandedExit::kServiceComplete;
      if (Contains(kPrevious, point)) { session.PagePrevious(); continue; }
      if (Contains(kNext, point)) { session.PageNext(); continue; }
      if (Contains(kGrid, point)) {
        const std::size_t col = static_cast<std::size_t>((point.x - kGrid.x) / 108.0F);
        const std::size_t row = static_cast<std::size_t>((point.y - kGrid.y) / 37.0F);
        session.SelectSlot(row * 5 + col);
        continue;
      }
      if (Contains(kBuy, point) && session.selected_id >= 0) {
        if (outfit_store) {
          (void)NovaLanded_BuyOutfit(state, stellar_id, session.selected_id, 1);
        } else {
          const ShipClass *ship = state.scenario.Ship(session.selected_id);
          (void)NovaLanded_ReplacePlayerShip(state, stellar_id, session.selected_id,
                                              ship == nullptr ? "" : ship->short_name);
        }
        session = outfit_store ? NovaLanded_OpenOutfitterSession(state, stellar_id)
                               : NovaLanded_OpenShipyardSession(state, stellar_id);
        continue;
      }
      if (outfit_store && Contains(kSell, point) && session.selected_id >= 0) {
        (void)NovaLanded_SellOutfit(state, session, session.selected_id, 1);
      }
    }
    SDL_Delay(16);
  }
  return LandedExit::kQuit;
}

} // namespace

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
                                         std::int16_t stellar_id) {
  if (service == LandedService::kOutfit || service == LandedService::kShipyard) {
    return RunStoreDialog(platform, state, service, stellar_id);
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
