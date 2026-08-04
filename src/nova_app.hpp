#pragma once

#include "brgr_archive.hpp"
#include "rle_sprite_sheet.hpp"
#include "sdl_platform.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

enum class GameModeAction : std::uint8_t {
  new_game = 0,
  open_pilot = 1,
  quit = 2,
  enter_spaceflight = 3,
  preferences = 4,
  starmap = 5,
};

enum class StartupPhase : std::uint8_t {
  loading_splash,
  startup_splash,
  main_menu,
};

struct NovaMenuSpriteAsset {
  RleSpriteSheet sheet;
  std::vector<std::unique_ptr<SdlTexture>> textures;
};

struct NovaRuntime {
  SdlPlatform platform;
  std::unique_ptr<SdlTexture> loading_splash_texture;
  std::unique_ptr<SdlTexture> startup_splash_texture;
  std::array<std::optional<NovaSpriteDefinition>, 6>
      main_menu_sprite_definitions;
  std::array<std::optional<NovaMenuSpriteAsset>, 6> main_menu_sprite_assets;
  std::optional<NovaMainMenuStyle> main_menu_style;
  StartupPhase startup_phase = StartupPhase::loading_splash;
  bool game_active = false;
  bool quit_requested = false;
  bool menu_prompt_visible = true;
  std::optional<GameModeAction> hovered_action;
  std::optional<GameModeAction> requested_action;
  const char *status_text = "Select an option.";
  std::uint64_t startup_phase_started_ms = 0;
  std::uint64_t next_menu_prompt_toggle_ms = 0;
};

[[nodiscard]] int NovaProgramEntry();
[[nodiscard]] int NovaApp_Run(NovaRuntime &runtime);
void NovaGameSession_Run(NovaRuntime &runtime);
void NovaMainLoop_Run(NovaRuntime &runtime);
void NovaMainLoop_UpdateFrame(NovaRuntime &runtime);
void NovaRender_RedrawAndPresentFrame(NovaRuntime &runtime, short mode);
void NovaGameMode_DispatchAction(NovaRuntime &runtime, GameModeAction action);
void NovaUi_PresentLoadingSplashFrame(NovaRuntime &runtime);
void NovaUi_PresentStartupSplashFrame(NovaRuntime &runtime);

[[nodiscard]] std::optional<GameModeAction>
NovaCommand_TranslateByInputMap(char command);
[[nodiscard]] std::optional<GameModeAction>
NovaHud_TrackFocusHoverIndex(const NovaRuntime &runtime);
