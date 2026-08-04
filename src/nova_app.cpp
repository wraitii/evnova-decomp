#include "nova_app.hpp"

#include "brgr_archive.hpp"
#include "log.hpp"
#include "pict_image.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cmath>
#include <string_view>

namespace {

struct MenuEntry {
  GameModeAction action;
  std::string_view label;
};

constexpr std::array kMenuEntries{
    MenuEntry{GameModeAction::new_game, "NEW GAME  [N]"},
    MenuEntry{GameModeAction::open_pilot, "OPEN PILOT [O]"},
    MenuEntry{GameModeAction::preferences, "PREFERENCES [P]"},
    MenuEntry{GameModeAction::starmap, "STAR MAP    [A]"},
    MenuEntry{GameModeAction::quit, "QUIT        [Q]"},
};

constexpr float kMenuCoordinateScale = 640.0F / 1024.0F;
constexpr float kFallbackMenuLeft = 52.0F;
constexpr float kFallbackMenuTop = 238.0F;
constexpr float kFallbackMenuWidth = 220.0F;
constexpr float kFallbackMenuHeight = 24.0F;
constexpr float kFallbackMenuGap = 8.0F;
constexpr std::uint64_t kLoadingSplashDurationMs = 850;
constexpr std::uint64_t kStartupSplashDurationMs = 1'850;

[[nodiscard]] SDL_FRect MenuRect(const NovaRuntime &runtime,
                                 std::size_t index) {
  if (index < runtime.main_menu_sprite_definitions.size() &&
      runtime.main_menu_style && runtime.main_menu_sprite_definitions[index]) {
    const auto &origin = runtime.main_menu_style->button_origins[index];
    const auto &sprite = *runtime.main_menu_sprite_definitions[index];
    return SDL_FRect{
        static_cast<float>(origin.x) * kMenuCoordinateScale,
        static_cast<float>(origin.y) * kMenuCoordinateScale,
        static_cast<float>(sprite.tile_width) * kMenuCoordinateScale,
        static_cast<float>(sprite.tile_height) * kMenuCoordinateScale,
    };
  }
  return SDL_FRect{
      kFallbackMenuLeft,
      kFallbackMenuTop +
          static_cast<float>(index) * (kFallbackMenuHeight + kFallbackMenuGap),
      kFallbackMenuWidth,
      kFallbackMenuHeight,
  };
}

[[nodiscard]] bool Contains(const SDL_FRect &rect, SDL_FPoint point) {
  return point.x >= rect.x && point.x <= rect.x + rect.w && point.y >= rect.y &&
         point.y <= rect.y + rect.h;
}

void DrawDebugTextCentered(SDL_Renderer *renderer, float center_x, float y,
                           std::string_view text) {
  constexpr float character_width = 8.0F;
  const auto text_width = static_cast<float>(text.length()) * character_width;
  SDL_RenderDebugText(renderer, center_x - text_width / 2.0F, y, text.data());
}

void DrawMenuBackground(SDL_Renderer *renderer, int width, int height) {
  SDL_SetRenderDrawColor(renderer, 1, 4, 12, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(renderer);

  for (int band = 0; band < height; band += 4) {
    const auto blue = static_cast<std::uint8_t>(15 + band * 20 / height);
    SDL_SetRenderDrawColor(renderer, 3, 10, blue, SDL_ALPHA_OPAQUE);
    SDL_FRect strip{0.0F, static_cast<float>(band), static_cast<float>(width),
                    4.0F};
    SDL_RenderFillRect(renderer, &strip);
  }

  SDL_SetRenderDrawColor(renderer, 112, 164, 236, SDL_ALPHA_OPAQUE);
  for (int index = 0; index < 86; ++index) {
    const auto x = static_cast<float>((index * 137) % width);
    const auto y = static_cast<float>((index * 79) % height);
    SDL_RenderPoint(renderer, x, y);
  }
}

void DrawPlanet(SDL_Renderer *renderer) {
  constexpr float center_x = 548.0F;
  constexpr float center_y = 290.0F;
  constexpr float radius = 172.0F;

  for (int y = -172; y <= 172; ++y) {
    const auto half_width =
        std::sqrt(radius * radius - static_cast<float>(y * y));
    const auto shade = static_cast<std::uint8_t>(34 + (y + 172) * 28 / 344);
    SDL_SetRenderDrawColor(renderer, 7, shade,
                           static_cast<std::uint8_t>(shade + 34),
                           SDL_ALPHA_OPAQUE);
    SDL_RenderLine(renderer, center_x - half_width,
                   center_y + static_cast<float>(y), center_x + half_width,
                   center_y + static_cast<float>(y));
  }

  SDL_SetRenderDrawColor(renderer, 86, 171, 215, SDL_ALPHA_OPAQUE);
  SDL_RenderLine(renderer, center_x - 150.0F, center_y - 82.0F,
                 center_x + 20.0F, center_y - 120.0F);
  SDL_RenderLine(renderer, center_x - 170.0F, center_y - 20.0F,
                 center_x + 84.0F, center_y - 56.0F);
  SDL_RenderLine(renderer, center_x - 166.0F, center_y + 66.0F,
                 center_x + 110.0F, center_y + 34.0F);
}

void DrawHudFrame(SDL_Renderer *renderer) {
  SDL_SetRenderDrawColor(renderer, 51, 113, 171, SDL_ALPHA_OPAQUE);
  const SDL_FRect outer{18.0F, 18.0F, 604.0F, 444.0F};
  SDL_RenderRect(renderer, &outer);
  const SDL_FRect inner{24.0F, 24.0F, 592.0F, 432.0F};
  SDL_RenderRect(renderer, &inner);

  SDL_RenderLine(renderer, 32.0F, 206.0F, 282.0F, 206.0F);
  SDL_RenderLine(renderer, 32.0F, 420.0F, 300.0F, 420.0F);
  SDL_RenderLine(renderer, 348.0F, 206.0F, 608.0F, 206.0F);
  SDL_RenderLine(renderer, 348.0F, 420.0F, 608.0F, 420.0F);

  SDL_SetRenderDrawColor(renderer, 89, 166, 223, SDL_ALPHA_OPAQUE);
  SDL_RenderDebugText(renderer, 40.0F, 34.0F, "NOVA NAVIGATION INTERFACE");
  SDL_RenderDebugText(renderer, 426.0F, 34.0F, "SYSTEM: MENU");
  SDL_RenderDebugText(renderer, 40.0F, 438.0F, "SECTOR 000 / LOCAL");
}

void DrawSplashFrame(SDL_Renderer *renderer, std::string_view heading,
                     std::string_view detail, float progress) {
  DrawMenuBackground(renderer, 640, 480);

  SDL_SetRenderDrawColor(renderer, 48, 113, 179, SDL_ALPHA_OPAQUE);
  const SDL_FRect panel{96.0F, 142.0F, 448.0F, 190.0F};
  SDL_RenderFillRect(renderer, &panel);
  SDL_SetRenderDrawColor(renderer, 142, 209, 255, SDL_ALPHA_OPAQUE);
  SDL_RenderRect(renderer, &panel);
  const SDL_FRect inset{104.0F, 150.0F, 432.0F, 174.0F};
  SDL_RenderRect(renderer, &inset);

  DrawDebugTextCentered(renderer, 320.0F, 184.0F, heading);
  SDL_SetRenderDrawColor(renderer, 207, 229, 255, SDL_ALPHA_OPAQUE);
  DrawDebugTextCentered(renderer, 320.0F, 218.0F, detail);

  SDL_SetRenderDrawColor(renderer, 5, 22, 48, SDL_ALPHA_OPAQUE);
  const SDL_FRect trough{158.0F, 268.0F, 324.0F, 12.0F};
  SDL_RenderFillRect(renderer, &trough);
  SDL_SetRenderDrawColor(renderer, 123, 218, 255, SDL_ALPHA_OPAQUE);
  const SDL_FRect fill{160.0F, 270.0F, 320.0F * progress, 8.0F};
  SDL_RenderFillRect(renderer, &fill);
}

void DrawAmbrosiaStartupSplash(SDL_Renderer *renderer) {
  // Retained only as a fallback when PICT 0x83 cannot be decoded. The real
  // asset is a plain 16-bit DirectBitsRect PICT in Nova Titles 1.rez and is
  // preferred (see NovaUi_PresentStartupSplashFrame).
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(renderer);

  SDL_SetRenderDrawColor(renderer, 18, 36, 72, SDL_ALPHA_OPAQUE);
  for (int index = 0; index < 28; ++index) {
    const auto x = static_cast<float>((index * 97 + 41) % 640);
    const auto y = static_cast<float>((index * 149 + 29) % 480);
    SDL_RenderPoint(renderer, x, y);
  }

  SDL_SetRenderDrawColor(renderer, 105, 170, 244, SDL_ALPHA_OPAQUE);
  constexpr SDL_FRect upper_rule{182.0F, 193.0F, 276.0F, 1.0F};
  constexpr SDL_FRect lower_rule{182.0F, 288.0F, 276.0F, 1.0F};
  SDL_RenderFillRect(renderer, &upper_rule);
  SDL_RenderFillRect(renderer, &lower_rule);

  SDL_SetRenderScale(renderer, 2.0F, 2.0F);
  SDL_SetRenderDrawColor(renderer, 231, 241, 255, SDL_ALPHA_OPAQUE);
  DrawDebugTextCentered(renderer, 160.0F, 105.0F, "AMBROSIA");
  SDL_SetRenderScale(renderer, 1.0F, 1.0F);
  SDL_SetRenderDrawColor(renderer, 128, 172, 229, SDL_ALPHA_OPAQUE);
  DrawDebugTextCentered(renderer, 320.0F, 257.0F, "SOFTWARE, INC.");
}

// Presents a splash PICT scaled uniformly to fit the 640x480 viewport
// (letterboxed when the aspect differs). The original centers the image at
// native size and clips overflow; scaling keeps the full artwork visible
// without distortion.
void PresentSplashTexture(SDL_Renderer *renderer, SDL_Texture *texture) {
  float width = 0.0F;
  float height = 0.0F;
  SDL_GetTextureSize(texture, &width, &height);
  const auto scale = std::min(640.0F / width, 480.0F / height);
  const SDL_FRect destination{(640.0F - width * scale) / 2.0F,
                              (480.0F - height * scale) / 2.0F,
                              width * scale, height * scale};
  SDL_RenderTexture(renderer, texture, nullptr, &destination);
}

} // namespace

// Ghidra: 0x00503f30 NovaProgramEntry
int NovaProgramEntry() {
  NovaRuntime runtime;
  return NovaApp_Run(runtime);
}

// Ghidra: 0x004d2a80 NovaApp_Run
int NovaApp_Run(NovaRuntime &runtime) {
  // Platform_RegisterMainWindow / QuickTime_Initialize are replaced by SDL
  // setup.
  if (!runtime.platform.Initialize()) {
    return 1;
  }
  NovaGameSession_Run(runtime);
  return 0;
}

// Ghidra: 0x00416100 NovaGameSession_Run
void NovaGameSession_Run(NovaRuntime &runtime) {
  // Resource and QuickTime startup are not reconstructed yet. Licence checks
  // are deliberately skipped.
  runtime.game_active = false;
  runtime.status_text = "Select an option.";
  runtime.startup_phase = StartupPhase::loading_splash;
  runtime.startup_phase_started_ms = runtime.platform.ticks_ms();
  runtime.next_menu_prompt_toggle_ms = runtime.startup_phase_started_ms + 650;
  // Ghidra: FUN_004ad960, which loads sp\x95n 600-605 into DAT_00596cb8.
  for (std::size_t index = 0;
       index < runtime.main_menu_sprite_definitions.size(); ++index) {
    const auto sprite_id = static_cast<std::uint16_t>(600 + index);
    runtime.main_menu_sprite_definitions[index] =
        NovaResource_LoadMainMenuSpriteDefinition(sprite_id);
    if (const auto &definition = runtime.main_menu_sprite_definitions[index]) {
      NovaLog::Info(
          "loaded sp\\x95n {}: sprite PICT 0x{:04x}, mask PICT 0x{:04x}, "
          "{}x{} tiles ({}x{})",
          sprite_id, definition->sprites_resource_id,
          definition->mask_resource_id, definition->tile_width,
          definition->tile_height, definition->tiles_x, definition->tiles_y);
    } else {
      NovaLog::Todo("main-menu sp\\x95n {} could not be loaded", sprite_id);
    }
  }
  runtime.main_menu_style = NovaResource_LoadMainMenuStyle();
  if (runtime.main_menu_style) {
    NovaLog::Info(
        "loaded c\\x9alr main-menu style: bright #{:02x}{:02x}{:02x}, "
        "dim #{:02x}{:02x}{:02x}, {} px font",
        runtime.main_menu_style->menu_bright.red,
        runtime.main_menu_style->menu_bright.green,
        runtime.main_menu_style->menu_bright.blue,
        runtime.main_menu_style->menu_dim.red,
        runtime.main_menu_style->menu_dim.green,
        runtime.main_menu_style->menu_dim.blue,
        runtime.main_menu_style->menu_font_size);
  } else {
    NovaLog::Todo("main-menu c\\x9alr style could not be loaded; using "
                  "provisional layout");
  }
  // Ghidra: 0x004b9050 Resource_LoadPictAsImage. The loading splash is PICT
  // 0x1fa4 ("Atmos/ambrosia", portrait); the startup splash is PICT 0x83
  // (the Ambrosia logo, 832x624). Both are plain 16-bit DirectBitsRect PICTs.
  const auto load_splash_texture = [&runtime](std::uint16_t resource_id) {
    if (const auto pict_data = NovaResource_LoadPictData(resource_id)) {
      if (const auto pict = Resource_LoadPictAsImage(*pict_data)) {
        auto texture = SdlTexture::Create(runtime.platform.renderer(),
                                          pict->width, pict->height,
                                          pict->rgba_pixels);
        if (!texture) {
          NovaLog::Error("PICT 0x{:04x} decoded but SDL texture upload "
                         "failed",
                         resource_id);
        }
        return texture;
      }
      NovaLog::Todo("PICT 0x{:04x} failed to decode", resource_id);
    }
    return std::unique_ptr<SdlTexture>{};
  };
  runtime.loading_splash_texture = load_splash_texture(0x1fa4);
  runtime.startup_splash_texture = load_splash_texture(0x83);
  NovaMainLoop_Run(runtime);
}

// Ghidra: 0x00486880 NovaMainLoop_Run
void NovaMainLoop_Run(NovaRuntime &runtime) {
  while (!runtime.quit_requested && !runtime.platform.quit_requested()) {
    NovaMainLoop_UpdateFrame(runtime);
    NovaRender_RedrawAndPresentFrame(runtime, 1);
  }
}

// Ghidra: 0x00488080 NovaMainLoop_UpdateFrame
void NovaMainLoop_UpdateFrame(NovaRuntime &runtime) {
  if (const auto command = runtime.platform.PollCommandEvent()) {
    if (*command == 'q') {
      runtime.requested_action = GameModeAction::quit;
    } else if (runtime.startup_phase == StartupPhase::main_menu) {
      if (*command == 'm') {
        runtime.requested_action = NovaHud_TrackFocusHoverIndex(runtime);
      } else {
        runtime.requested_action = NovaCommand_TranslateByInputMap(*command);
      }
    }
  }

  const auto now_ms = runtime.platform.ticks_ms();
  if (runtime.startup_phase == StartupPhase::loading_splash &&
      now_ms - runtime.startup_phase_started_ms >= kLoadingSplashDurationMs) {
    runtime.startup_phase = StartupPhase::startup_splash;
    runtime.startup_phase_started_ms = now_ms;
  } else if (runtime.startup_phase == StartupPhase::startup_splash &&
             now_ms - runtime.startup_phase_started_ms >=
                 kStartupSplashDurationMs) {
    runtime.startup_phase = StartupPhase::main_menu;
    runtime.next_menu_prompt_toggle_ms = now_ms + 650;
  }

  if (runtime.startup_phase == StartupPhase::main_menu) {
    runtime.hovered_action = NovaHud_TrackFocusHoverIndex(runtime);
  } else {
    runtime.hovered_action.reset();
  }

  if (runtime.requested_action) {
    NovaGameMode_DispatchAction(runtime, *runtime.requested_action);
    runtime.requested_action.reset();
  }

  if (runtime.startup_phase == StartupPhase::main_menu &&
      now_ms >= runtime.next_menu_prompt_toggle_ms) {
    runtime.menu_prompt_visible = !runtime.menu_prompt_visible;
    runtime.next_menu_prompt_toggle_ms = now_ms + 650;
  }
}

// Ghidra: 0x004873b0 NovaRender_RedrawAndPresentFrame
void NovaRender_RedrawAndPresentFrame(NovaRuntime &runtime, short mode) {
  SDL_Renderer *const renderer = runtime.platform.renderer();
  if (runtime.startup_phase == StartupPhase::loading_splash) {
    NovaUi_PresentLoadingSplashFrame(runtime);
    SDL_RenderPresent(renderer);
    return;
  }
  if (runtime.startup_phase == StartupPhase::startup_splash) {
    NovaUi_PresentStartupSplashFrame(runtime);
    SDL_RenderPresent(renderer);
    return;
  }

  DrawMenuBackground(renderer, 640, 480);
  DrawPlanet(renderer);
  DrawHudFrame(renderer);

  SDL_SetRenderDrawColor(renderer, 202, 224, 255, SDL_ALPHA_OPAQUE);
  DrawDebugTextCentered(renderer, 320.0F, 78.0F,
                        "E S C A P E   V E L O C I T Y");
  SDL_SetRenderDrawColor(renderer, 114, 204, 255, SDL_ALPHA_OPAQUE);
  DrawDebugTextCentered(renderer, 320.0F, 102.0F, "N O V A");
  SDL_SetRenderDrawColor(renderer, 142, 189, 229, SDL_ALPHA_OPAQUE);
  DrawDebugTextCentered(renderer, 320.0F, 130.0F, "PILOT COMMAND CONSOLE");

  for (std::size_t index = 0; index < kMenuEntries.size(); ++index) {
    const auto rect = MenuRect(runtime, index);
    const bool hovered = runtime.hovered_action == kMenuEntries[index].action;
    if (hovered) {
      SDL_SetRenderDrawColor(renderer, 39, 99, 151, SDL_ALPHA_OPAQUE);
      SDL_RenderFillRect(renderer, &rect);
      SDL_SetRenderDrawColor(renderer, 190, 232, 255, SDL_ALPHA_OPAQUE);
      SDL_RenderRect(renderer, &rect);
      SDL_RenderDebugText(renderer, rect.x + 6.0F, rect.y + 8.0F, ">");
    }
    const auto menu_color =
        runtime.main_menu_style
            ? (hovered ? runtime.main_menu_style->menu_bright
                       : runtime.main_menu_style->menu_dim)
            : NovaRgbColor{
                  .red = static_cast<std::uint8_t>(hovered ? 242 : 154),
                  .green = static_cast<std::uint8_t>(hovered ? 248 : 200),
                  .blue = static_cast<std::uint8_t>(hovered ? 255 : 239),
              };
    SDL_SetRenderDrawColor(renderer, menu_color.red, menu_color.green,
                           menu_color.blue, SDL_ALPHA_OPAQUE);
    SDL_RenderDebugText(renderer, rect.x + 22.0F, rect.y + 8.0F,
                        kMenuEntries[index].label.data());
  }

  SDL_SetRenderDrawColor(renderer, 159, 190, 227, SDL_ALPHA_OPAQUE);
  DrawDebugTextCentered(renderer, 320.0F, 370.0F, runtime.status_text);
  if (runtime.menu_prompt_visible) {
    DrawDebugTextCentered(renderer, 320.0F, 394.0F, "SELECT A COMMAND");
  }

  if (mode == 1) {
    SDL_RenderPresent(renderer);
  }
}

// Ghidra: 0x004ab070 NovaUi_PresentLoadingSplashFrame
void NovaUi_PresentLoadingSplashFrame(NovaRuntime &runtime) {
  if (runtime.loading_splash_texture) {
    SDL_SetRenderDrawColor(runtime.platform.renderer(), 0, 0, 0,
                           SDL_ALPHA_OPAQUE);
    SDL_RenderClear(runtime.platform.renderer());
    PresentSplashTexture(runtime.platform.renderer(),
                         runtime.loading_splash_texture->get());
    return;
  }
  const auto elapsed_ms =
      runtime.platform.ticks_ms() - runtime.startup_phase_started_ms;
  const auto progress = static_cast<float>(elapsed_ms) /
                        static_cast<float>(kLoadingSplashDurationMs);
  DrawSplashFrame(runtime.platform.renderer(), "ESCAPE VELOCITY: NOVA",
                  "INITIALIZING NAVIGATION SYSTEMS", progress);
}

// Ghidra: 0x004aaf60 NovaUi_PresentStartupSplashFrame
void NovaUi_PresentStartupSplashFrame(NovaRuntime &runtime) {
  if (runtime.startup_splash_texture) {
    SDL_SetRenderDrawColor(runtime.platform.renderer(), 0, 0, 0,
                           SDL_ALPHA_OPAQUE);
    SDL_RenderClear(runtime.platform.renderer());
    PresentSplashTexture(runtime.platform.renderer(),
                         runtime.startup_splash_texture->get());
    return;
  }
  DrawAmbrosiaStartupSplash(runtime.platform.renderer());
}

// Ghidra: 0x00486ed0 NovaGameMode_DispatchAction
void NovaGameMode_DispatchAction(NovaRuntime &runtime, GameModeAction action) {
  switch (action) {
  case GameModeAction::new_game:
    runtime.status_text = "New Game flow is not reconstructed yet.";
    break;
  case GameModeAction::open_pilot:
    runtime.status_text = "Open Pilot flow is not reconstructed yet.";
    break;
  case GameModeAction::quit:
    runtime.quit_requested = true;
    break;
  case GameModeAction::enter_spaceflight:
    runtime.status_text = "Spaceflight requires an active pilot.";
    break;
  case GameModeAction::preferences:
    runtime.status_text = "Preferences dialog is not reconstructed yet.";
    break;
  case GameModeAction::starmap:
    runtime.status_text = "Star Map requires an active pilot.";
    break;
  }
}

// Ghidra: 0x004d6260 NovaCommand_TranslateByInputMap
std::optional<GameModeAction> NovaCommand_TranslateByInputMap(char command) {
  switch (command) {
  case 'n':
    return GameModeAction::new_game;
  case 'o':
    return GameModeAction::open_pilot;
  case 'p':
    return GameModeAction::preferences;
  case 'a':
    return GameModeAction::starmap;
  case 'q':
    return GameModeAction::quit;
  default:
    return std::nullopt;
  }
}

// Ghidra: 0x004861b0 NovaHud_TrackFocusHoverIndex
std::optional<GameModeAction>
NovaHud_TrackFocusHoverIndex(const NovaRuntime &runtime) {
  const auto mouse_position = runtime.platform.mouse_position();
  for (std::size_t index = 0; index < kMenuEntries.size(); ++index) {
    if (Contains(MenuRect(runtime, index), mouse_position)) {
      return kMenuEntries[index].action;
    }
  }
  return std::nullopt;
}
