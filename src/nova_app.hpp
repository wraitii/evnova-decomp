#pragma once

#include "brgr_archive.hpp"
#include "game/game_state.hpp"
#include "game/preferences.hpp"
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
  // True once the menu bass has started on entry to the second startup splash;
  // the same stream continues into the main-menu phase.
  bool menu_music_started = false;
  // Ghidra: NovaAudio_PreloadTransitionEffects loads snd 600..603. The first
  // pair marks focus entry/exit; the second pair accompanies each row's
  // pre-rendered entrance reveal.
  std::optional<NovaSoundData> menu_focus_enter_sound;
  std::optional<NovaSoundData> menu_focus_exit_sound;
  std::optional<NovaSoundData> menu_reveal_start_sound;
  std::optional<NovaSoundData> menu_reveal_finish_sound;
  std::unique_ptr<SdlTexture> loading_splash_texture;
  std::unique_ptr<SdlTexture> startup_splash_texture;
  // Ghidra: title-screen backdrop PICT 0x1f40 (1024x768 ship-interior scene)
  // rendered behind the main-menu buttons.
  std::unique_ptr<SdlTexture> main_menu_backdrop_texture;
  // Ghidra: sp\x95n 606 logo sheet PICT 0x1f4a (7 frames of 654x209).
  std::vector<std::unique_ptr<SdlTexture>> main_menu_logo_textures;
  // Ghidra: sp\x95n 607, one center-preview frame per menu action plus idle.
  std::optional<NovaMenuSpriteAsset> main_menu_center_preview_asset;
  // Ghidra: sp\x95n 608-610, three PICT animation strips that reveal the
  // paired rows of buttons as the main menu opens.
  std::array<std::vector<std::unique_ptr<SdlTexture>>, 3>
      main_menu_row_reveal_textures;
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
  std::uint64_t next_menu_top_animation_ms = 0;
  std::uint64_t next_menu_reveal_frame_ms = 0;
  std::uint64_t menu_center_preview_last_update_ms = 0;
  std::size_t menu_top_animation_frame = 0;
  std::size_t menu_center_preview_frame = 6;
  std::uint8_t menu_center_preview_intensity = 0;
  // Global (non-pilot) preferences and the gameplay key-binding table. Holds
  // the values the Settings dialog edits and that the clean-room play paths
  // (sound volume, ship animations, brightness, flight bindings) will read.
  // Kept on the runtime rather than as globals (AGENTS.md).
  game::NovaPreferences prefs;

  // Active in-game state for the running pilot (new-game flow, intro
  // cinematic, and spaceflight mode all read/write it). Lives on the runtime
  // rather than as globals (AGENTS.md: represent game state explicitly).
  game::GameState game;

  // Original counters begin at -1, then stagger each following strip by half
  // the preceding strip's frame count. A strip is replaced by its two focus
  // sprites once its counter reaches the strip's frame count.
  std::array<int, 3> menu_row_reveal_counters{-1, -1, -1};
  bool menu_entrance_initialized = false;
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
