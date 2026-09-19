#include "locate_data_dialog.hpp"

#include "../log.hpp"
#include "../nova_paths.hpp"
#include "../util/geometry.hpp"
#include "../util/placement.hpp"
#include "nova_font.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <string>
#include <string_view>

namespace game {
namespace {

// Authored on the same 1024x768 canvas as the rest of the startup screens.
constexpr float kCanvasWidth = 1024.0F;
constexpr float kCanvasHeight = 768.0F;
constexpr float kButtonWidth = 320.0F;
constexpr float kButtonHeight = 48.0F;
constexpr float kButtonLeft = (kCanvasWidth - kButtonWidth) * 0.5F;
constexpr float kDebugGlyphWidth = 8.0F;

constexpr SDL_Color kTitleColor{255, 255, 255, SDL_ALPHA_OPAQUE};
constexpr SDL_Color kBodyColor{210, 216, 224, SDL_ALPHA_OPAQUE};
constexpr SDL_Color kHintColor{150, 156, 166, SDL_ALPHA_OPAQUE};
constexpr SDL_Color kErrorColor{255, 120, 120, SDL_ALPHA_OPAQUE};

struct LocateButton {
  SDL_FRect rect;
  std::string_view label;
};

// True when a usable face resolved for the body family. Without one (a machine
// with none of the bundled/native/substitute faces) the screen falls back to
// SDL's built-in debug font so the message is never invisible.
[[nodiscard]] bool UseFallbackFont(NovaFontCache &fonts) {
  return !fonts.IsFamilyAvailable(NovaFontFamily::kHelvetica);
}

void DrawCenteredLine(SdlPlatform &platform,
                      NovaFontCache &fonts,
                      bool fallback_font,
                      std::string_view text,
                      float baseline_y,
                      float point_size,
                      SDL_Color color,
                      bool bold) {
  if (fallback_font) {
    const float width = static_cast<float>(text.size()) * kDebugGlyphWidth;
    SDL_RenderDebugText(platform.renderer(),
                        (kCanvasWidth - width) * 0.5F,
                        baseline_y,
                        std::string{text}.c_str());
    return;
  }
  NovaText_DrawCentered(platform,
                        fonts,
                        NovaFontFamily::kHelvetica,
                        point_size,
                        bold ? kNovaFontStyleBold : kNovaFontStyleRegular,
                        color,
                        0.0F,
                        kCanvasWidth,
                        baseline_y,
                        text);
}

void DrawButton(SdlPlatform &platform,
                NovaFontCache &fonts,
                bool fallback_font,
                const LocateButton &button,
                bool hovered,
                bool armed) {
  SDL_Renderer *const renderer = platform.renderer();
  const SDL_FRect &rect = button.rect;
  const bool highlighted = hovered || armed;
  if (highlighted) {
    SDL_SetRenderDrawColor(renderer, 52, 74, 104, SDL_ALPHA_OPAQUE);
  } else {
    SDL_SetRenderDrawColor(renderer, 28, 38, 56, SDL_ALPHA_OPAQUE);
  }
  SDL_RenderFillRect(renderer, &rect);
  if (highlighted) {
    SDL_SetRenderDrawColor(renderer, 150, 190, 235, SDL_ALPHA_OPAQUE);
  } else {
    SDL_SetRenderDrawColor(renderer, 96, 120, 160, SDL_ALPHA_OPAQUE);
  }
  SDL_RenderRect(renderer, &rect);

  constexpr float kButtonFontSize = 20.0F;
  const float baseline = rect.y + rect.h * 0.5F + kButtonFontSize * 0.35F;
  if (fallback_font) {
    const float width =
        static_cast<float>(button.label.size()) * kDebugGlyphWidth;
    SDL_RenderDebugText(renderer,
                        rect.x + (rect.w - width) * 0.5F,
                        rect.y + rect.h * 0.5F - kDebugGlyphWidth * 0.5F,
                        std::string{button.label}.c_str());
    return;
  }
  NovaText_DrawCentered(platform,
                        fonts,
                        NovaFontFamily::kHelvetica,
                        kButtonFontSize,
                        kNovaFontStyleRegular,
                        kTitleColor,
                        rect.x,
                        rect.x + rect.w,
                        baseline,
                        button.label);
}

} // namespace

std::optional<std::filesystem::path>
NovaUi_RunLocateDataDialog(SdlPlatform &platform) {
  NovaFontCache fonts;
  const bool fallback_font = UseFallbackFont(fonts);
  if (fallback_font) {
    NovaLog::Warn("locate-data screen: no UI font resolved; using SDL's debug "
                  "font");
  }

  const std::array<LocateButton, 2> buttons{{
      {{kButtonLeft, 470.0F, kButtonWidth, kButtonHeight},
       "Locate EV Nova.exe..."},
      {{kButtonLeft, 538.0F, kButtonWidth, kButtonHeight}, "Quit"},
  }};

  // True while the native file chooser is open; further clicks are ignored
  // and the locate button stays highlighted.
  bool chooser_open = false;
  int armed_button = -1;
  std::string error_message;

  const auto draw_frame = [&] {
    SDL_Renderer *const renderer = platform.renderer();
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);

    DrawCenteredLine(platform,
                     fonts,
                     fallback_font,
                     "EV Nova data not found",
                     168.0F,
                     34.0F,
                     kTitleColor,
                     true);
    DrawCenteredLine(
        platform,
        fonts,
        fallback_font,
        "The game could not locate the EV Nova Community Edition data.",
        236.0F,
        19.0F,
        kBodyColor,
        false);
    DrawCenteredLine(
        platform,
        fonts,
        fallback_font,
        "It looks for Nova.rez and a Nova Files folder next to the",
        270.0F,
        19.0F,
        kBodyColor,
        false);
    DrawCenteredLine(platform,
                     fonts,
                     fallback_font,
                     "game executable and in the working directory.",
                     304.0F,
                     19.0F,
                     kBodyColor,
                     false);
    DrawCenteredLine(platform,
                     fonts,
                     fallback_font,
                     "Select EV Nova.exe from inside your EV Nova folder",
                     360.0F,
                     19.0F,
                     kBodyColor,
                     false);
    DrawCenteredLine(platform,
                     fonts,
                     fallback_font,
                     "(the Community Edition download).",
                     394.0F,
                     19.0F,
                     kBodyColor,
                     false);

    if (!error_message.empty()) {
      DrawCenteredLine(platform,
                       fonts,
                       fallback_font,
                       error_message,
                       438.0F,
                       18.0F,
                       kErrorColor,
                       false);
    }

    const SDL_FPoint mouse = platform.mouse_position();
    for (std::size_t index = 0; index < buttons.size(); ++index) {
      const bool hovered =
          !chooser_open && evnova::util::Contains(buttons[index].rect, mouse);
      DrawButton(platform,
                 fonts,
                 fallback_font,
                 buttons[index],
                 hovered,
                 static_cast<int>(index) == armed_button);
    }

    DrawCenteredLine(platform,
                     fonts,
                     fallback_font,
                     "This location is remembered in EV Nova Extra Prefs.ini.",
                     690.0F,
                     16.0F,
                     kHintColor,
                     false);
    platform.Present();
  };

  platform.SetPlacement(PlaceContained({kCanvasWidth, kCanvasHeight},
                                       platform.logical_playfield_size()));
  // Present once before opening a native dialog so the window is mapped.
  draw_frame();

  while (!platform.quit_requested()) {
    for (std::optional<TextInput> input; (input = platform.PollTextEvent());) {
      if (input->key == TextKey::escape) {
        return std::nullopt;
      }
      if (input->key != TextKey::primary || chooser_open) {
        continue;
      }
      const SDL_FPoint point = platform.mouse_position();
      for (std::size_t index = 0; index < buttons.size(); ++index) {
        if (!evnova::util::Contains(buttons[index].rect, point)) {
          continue;
        }
        if (index == 0) {
          if (platform.ShowOpenInstallFileDialog()) {
            chooser_open = true;
            armed_button = static_cast<int>(index);
          } else {
            error_message = "The file chooser could not be opened.";
          }
        } else {
          return std::nullopt;
        }
      }
    }

    if (chooser_open) {
      if (auto result = platform.PollOpenFileDialogResult()) {
        chooser_open = false;
        armed_button = -1;
        if (!result->error.empty()) {
          error_message = "The file chooser failed: " + result->error;
        } else if (result->path) {
          const auto resolved =
              NovaPaths::ResolveUserSelectedInstallRoot(*result->path);
          if (resolved) {
            return resolved;
          }
          error_message =
              "That file is not inside an EV Nova folder (no Nova.rez or "
              "Nova Files nearby). Select EV Nova.exe.";
          NovaLog::Warn("locate-data: '{}' is not an EV Nova install root",
                        result->path->string());
        }
      }
    }

    draw_frame();
    platform.PaceFrame();
  }
  return std::nullopt;
}

} // namespace game
