// Docked sub-window infrastructure: the generic service frame and the modal
// orchestrator that dispatches to each service window (Mission BBS, store,
// bar, trade center).
//
// Split out of the original docked_dialog.cpp.

#include "docked_dialog_internal.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../pict_image.hpp"
#include "../sdl_platform.hpp"
#include "nova_font.hpp"
#include "services_buttons.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace game {
namespace {

// The dim scrim between the docked backdrop and the active dialog window, so
// the modal reads as popped off the docked screen (mirrors the game's modal
// sub-windows dimming/occluding the backing dialog).
constexpr SDL_Color kScrim{0, 0, 0, 150};

// Colours for the dialog heading/leave labels, matching the docked menu's
// palette (bright rows for the title, dim rows for the helper text).
constexpr SDL_Color kTitle{255, 255, 255, 255};

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
  case LandedService::kMissionBbs:
    return "Mission BBS";
  default:
    return "Services";
  }
}

// Draws one frame of the sub-window dialog: the re-rendered docked menu, a
// dim scrim, then the frame PICT centred at its natural size with the
// heading and Leave button.
void DrawSubWindowDialog(SdlPlatform &platform,
                         NovaFontCache &font_cache,
                         const ServicesButtonArt &button_art,
                         const std::function<void()> &render_background,
                         SDL_Texture *backdrop,
                         SDL_Texture *frame,
                         LandedService service,
                         const SDL_FRect &panel) {
  SDL_Renderer *renderer = platform.renderer();
  if (render_background) {
    // The callback leaves the renderer in the docked menu's fullscreen
    // presentation; dim the whole window before switching to the dialog's
    // centred 640x480 canvas.
    render_background();
    const SDL_FPoint output = platform.logical_playfield_size();
    const SDL_FRect output_rect{0.0F, 0.0F, output.x, output.y};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, kScrim.r, kScrim.g, kScrim.b, kScrim.a);
    SDL_RenderFillRect(renderer, &output_rect);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
  } else {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);
  }
  platform.SetCenteredPlayfield();
  if (render_background == nullptr && backdrop != nullptr) {
    SDL_RenderTexture(renderer, backdrop, nullptr, &panel);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, kScrim.r, kScrim.g, kScrim.b, kScrim.a);
    SDL_RenderFillRect(renderer, &panel);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
  }

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
    DrawThreeStateButtonLabel(platform, font_cache, leaveRect, "Done", kTitle);
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
  DrawThreeStateButtonLabel(platform, font_cache, leaveRect, "Done", kTitle);

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

} // namespace

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

std::uint16_t NovaDocked_SubWindowFramePict(LandedService service) {
  switch (service) {
  case LandedService::kShipyard:
    return 0x2135;
  case LandedService::kOutfit:
    return 0x2136;
  case LandedService::kBar:
    return 0x2137;
  case LandedService::kMissionBbs:
    return 0x2139;
  case LandedService::kBuySellCargo:
    return 0x213e;
  // Communications (0x213f) has no docked slot in the MVP; never reached.
  default:
    return 0;
  }
}

LandedExit
NovaLanded_RunSubWindowDialog(SdlPlatform &platform,
                              GameState &state,
                              LandedService service,
                              std::int16_t stellar_id,
                              const std::function<void()> &render_background) {
  if (service == LandedService::kMissionBbs) {
    return RunMissionBbsWindow(platform, state, stellar_id, render_background);
  }
  if (service == LandedService::kBar) {
    return RunBarDialog(platform, state, stellar_id, render_background);
  }
  if (service == LandedService::kBuySellCargo) {
    return RunTradeCenterDialog(platform, state, stellar_id, render_background);
  }
  if (service == LandedService::kOutfit ||
      service == LandedService::kShipyard) {
    return RunStoreDialog(
        platform, state, service, stellar_id, render_background);
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
                        render_background,
                        backdrop ? backdrop->get() : nullptr,
                        frame ? frame->get() : nullptr,
                        service,
                        panel);
    platform.Present();

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
