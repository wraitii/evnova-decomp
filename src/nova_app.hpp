#pragma once

#include "brgr_archive.hpp"
#include "game/game_state.hpp"
#include "game/nova_font.hpp"
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

// The action codes are the original's NovaGameMode_DispatchAction selectors;
// the six menu focus sprites (sp\x95n 600-605) map lane index -> action code
// directly (Ghidra 0x00486880 NovaMainLoop_Run: hovered lane is dispatched
// verbatim). Lane 5 is the ABOUT NOVA button.
enum class GameModeAction : std::uint8_t {
  new_game = 0,
  open_pilot = 1,
  quit = 2,
  enter_spaceflight = 3,
  preferences = 4,
  about_nova = 5,
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
  // The rollover's rest-state blit is BlitPixel_CopyOrSpan (Ghidra 0x00473b60):
  // the frame is OR-ed onto the backdrop per 16-bit pixel, so its near-black
  // "plate" pixels vanish and only the bright glyph bits merge in. The port
  // keeps the backdrop pixels and pre-composites the same OR per frame.
  std::vector<std::uint8_t> main_menu_backdrop_rgba;
  int main_menu_backdrop_width = 0;
  int main_menu_backdrop_height = 0;
  std::unique_ptr<SdlTexture> main_menu_center_preview_composited;
  std::size_t main_menu_center_preview_composited_frame = 0;
  bool main_menu_center_preview_composited_valid = false;
  // Ghidra: sp\x95n 608-610, three PICT animation strips that reveal the
  // paired rows of buttons as the main menu opens.
  std::array<std::vector<std::unique_ptr<SdlTexture>>, 3>
      main_menu_row_reveal_textures;
  std::array<std::optional<NovaSpriteDefinition>, 6>
      main_menu_sprite_definitions;
  std::array<std::optional<NovaMenuSpriteAsset>, 6> main_menu_sprite_assets;
  std::optional<NovaMainMenuStyle> main_menu_style;
  // Geneva.ttf face cache for the menu status text (the original draws menu
  // text with g_main_menu_font_id 3 / size 9, Ghidra 0x004b32aa).
  game::NovaFontCache font_cache;
  // 128x64 clone-source portrait PICT (3000 + class) shown in the pilot status
  // panel; cached per class id, -1 = none loaded yet.
  std::unique_ptr<SdlTexture> menu_status_portrait;
  std::int16_t menu_status_portrait_class = -1;
  StartupPhase startup_phase = StartupPhase::loading_splash;
  bool game_active = false;
  bool quit_requested = false;
  std::optional<GameModeAction> hovered_action;
  // Hovered action as of the previous frame; a change between frames triggers
  // the menu hover blip (matching the game's transition test).
  std::optional<GameModeAction> previous_hovered_action;
  std::optional<GameModeAction> requested_action;
  std::uint64_t startup_phase_started_ms = 0;
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
