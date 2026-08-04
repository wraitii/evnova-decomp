#pragma once

#include "brgr_archive.hpp"
#include "rle_sprite_sheet.hpp"
#include "sdl_audio.hpp"
#include "sdl_music.hpp"
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
  // SDL audio output shared by menu feedback sounds. Kept as a member (not a
  // singleton) so the runtime owns its lifecycle.
  SdlAudio audio;
  // Background/menu bass stream (SDL3_mixer), separate logical device.
  SdlMusic music;
  // True once the main-menu bass has been started (on first reaching the
  // main-menu phase).
  bool menu_music_started = false;
  // Decoded main-menu feedback sounds: hover blip (id 600) and select blip
  // (id 601). Loaded once during session startup.
  std::optional<NovaSoundData> menu_hover_sound;
  std::optional<NovaSoundData> menu_select_sound;
  std::unique_ptr<SdlTexture> loading_splash_texture;
  std::unique_ptr<SdlTexture> startup_splash_texture;
  // Ghidra: title-screen backdrop PICT 0x1f40 (1024x768 ship-interior scene)
  // rendered behind the main-menu buttons.
  std::unique_ptr<SdlTexture> main_menu_backdrop_texture;
  // Ghidra: sp\x95n 606 logo sheet PICT 0x1f4a (7 frames of 654x209).
  std::vector<std::unique_ptr<SdlTexture>> main_menu_logo_textures;
  std::array<std::optional<NovaSpriteDefinition>, 6>
      main_menu_sprite_definitions;
  std::array<std::optional<NovaMenuSpriteAsset>, 6> main_menu_sprite_assets;
  std::optional<NovaMainMenuStyle> main_menu_style;
  StartupPhase startup_phase = StartupPhase::loading_splash;
  bool game_active = false;
  bool quit_requested = false;
  bool menu_prompt_visible = true;
  std::optional<GameModeAction> hovered_action;
  // Hovered action as of the previous frame; a change between frames triggers
  // the menu hover blip (matching the game's transition test).
  std::optional<GameModeAction> previous_hovered_action;
  std::optional<GameModeAction> requested_action;
  // Status line printed under the menu when a menu action has been triggered.
  // When empty (idle), the original draws no status line, only the pulsing
  // "SELECT A COMMAND" prompt (string key 0x7d2/0x114, Ghidra
  // 0x004873b0 NovaRender_RedrawAndPresentFrame).
  std::optional<const char *> status_text;
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
