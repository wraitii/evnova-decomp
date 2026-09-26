#include "nova_app.hpp"

#include "brgr_archive.hpp"
#include "game/about_dialog.hpp"
#include "game/command_input.hpp"
#include "game/extended_prefs.hpp"
#include "game/hud_overlay.hpp"
#include "game/locate_data_dialog.hpp"
#include "game/mission.hpp"
#include "game/mission_trace.hpp"
#include "game/new_pilot_flow.hpp"
#include "game/nova_font.hpp"
#include "game/pilot_file.hpp"
#include "game/player_info_window.hpp"
#include "game/probe_state.hpp"
#include "game/ship_ai.hpp"
#include "game/ship_spawn.hpp"
#include "game/spaceflight.hpp"
#include "game/targeting.hpp"
#include "game/travel.hpp"
#include "game/ui_dialog.hpp"
#include "log.hpp"
#include "nova_paths.hpp"
#include "pict_image.hpp"
#include "rle_sprite_sheet.hpp"
#include "util/color.hpp"
#include "util/geometry.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace {

using evnova::util::ContainsInclusive;
using evnova::util::ToSdlColor;

struct MenuEntry {
  GameModeAction action;
  std::string_view label;
};

constexpr std::array kMenuEntries{
    MenuEntry{GameModeAction::new_game, "NEW PILOT"},
    MenuEntry{GameModeAction::open_pilot, "OPEN PILOT"},
    MenuEntry{GameModeAction::quit, "QUIT NOVA"},
    MenuEntry{GameModeAction::enter_spaceflight, "ENTER SHIP"},
    MenuEntry{GameModeAction::preferences, "SET PREFS"},
    MenuEntry{GameModeAction::about_nova, "ABOUT NOVA"},
};

constexpr std::uint64_t kLoadingSplashDurationMs = 850;
constexpr std::uint64_t kStartupSplashDurationMs = 1'850;
// The port paces its staged startup asset loads to match the original's
// frame-yield cadence (NovaMainLoop_PumpAndYieldFrames(10)) so the bar fills
// visibly instead of jumping to full.
constexpr std::uint64_t kStartupLoadStepDwellMs = 160;
// Time for the smoothed display value to sweep the full bar; the original's
// per-ship increments make it fill over the whole visual load.
constexpr std::uint64_t kStartupProgressFillMs = 1'200;
// These are authored animation timers, not render-loop delays. The original
// title fire changes visibly slower than the 60 Hz frame cadence; tying it to
// every host frame made the flame flicker far too quickly in SDL.
constexpr std::uint64_t kMenuTitleFrameDurationMs = 40;
constexpr std::uint64_t kMenuRevealFrameDurationMs = 16;
// Startup loading progress bar (Ghidra 0x004ab1b0/0x004ab3b0/0x004ab3d0). The
// bar outline comes from c\x9alr in native reference coordinates relative to
// the window center; DAT_00575a58 = 198.0 is the fill span in those pixels.
constexpr double kProgressBarFillSpan = 198.0;
// Number of staged startup asset loads that drive the bar. The original's
// denominator is the 'ship' resource count (NovaData_LoadAllShipClass
// VisualAndLaunchData 0x004aeda0); the port's startup loads a different set, so
// the total here counts the steps below (see NovaGameSession_Run).
constexpr std::uint8_t kStartupLoadStepCount = 6;

// The original draws menu text with g_main_menu_font_id 3 (Geneva) size 9 in
// the 1024x768 backdrop space (Ghidra 0x004b32aa NovaData_LoadScenarioResource
// Tables + 0x004874d5 NovaRender_RedrawAndPresentFrame).
constexpr float kMenuFontLogicalSize = 9.0F;
// Menu label/value colours. Ghidra 0x004b3262/0x004b327d seed DAT_0073564c
// (values, RGB555 triplet 0xffff,0,0) and DAT_00735652 (labels, 0x84d0,0,0);
// the text engine scales each 5-bit component by 8 (FUN_004bc760).
constexpr SDL_Color kMenuLabelColor{128, 0, 0, SDL_ALPHA_OPAQUE};
constexpr SDL_Color kMenuValueColor{248, 0, 0, SDL_ALPHA_OPAQUE};

// Menu buttons are placed from the shipped c\x9alr style plus the button's own
// sprite definition; the startup gate guarantees both are present. Returns an
// empty rect when they are not, and the callers skip the entry rather than
// inventing a layout (the old mock fallback was removed with the locate-data
// screen).
[[nodiscard]] SDL_FRect MenuRect(const NovaRuntime &runtime,
                                 std::size_t index) {
  if (!runtime.main_menu_style ||
      index >= runtime.main_menu_sprite_definitions.size() ||
      !runtime.main_menu_sprite_definitions[index]) {
    return SDL_FRect{};
  }
  const auto &origin = runtime.main_menu_style->button_origins[index];
  const auto &sprite = *runtime.main_menu_sprite_definitions[index];
  return SDL_FRect{
      static_cast<float>(origin.x),
      static_cast<float>(origin.y),
      static_cast<float>(sprite.tile_width),
      static_cast<float>(sprite.tile_height),
  };
}

[[nodiscard]] std::optional<NovaMenuSpriteAsset>
LoadMenuSpriteAsset(SDL_Renderer *renderer,
                    const NovaSpriteDefinition &definition) {
  const auto resource_data = NovaResource_Load(kResourceTypeRleSheet16,
                                               definition.sprites_resource_id);
  if (!resource_data) {
    return std::nullopt;
  }
  auto sheet = RleSpriteSheet_Decode16(*resource_data);
  const auto expected_frames = static_cast<std::size_t>(definition.tiles_x) *
                               static_cast<std::size_t>(definition.tiles_y);
  if (!sheet || sheet->width != definition.tile_width ||
      sheet->height != definition.tile_height ||
      sheet->frames.size() != expected_frames) {
    return std::nullopt;
  }

  NovaMenuSpriteAsset asset{.sheet = std::move(*sheet), .textures = {}};
  asset.textures.reserve(asset.sheet.frames.size());
  for (const auto &frame : asset.sheet.frames) {
    auto texture = SdlTexture::Create(
        renderer, asset.sheet.width, asset.sheet.height, frame.rgba_pixels);
    if (!texture) {
      return std::nullopt;
    }
    asset.textures.push_back(std::move(texture));
  }
  return asset;
}

// Slices a vertically stacked PICT sprite resource into SDL textures. The
// title animation (606) and row reveals (608-610) all use this layout.
[[nodiscard]] std::vector<std::unique_ptr<SdlTexture>>
LoadStackedPictFrames(SDL_Renderer *renderer,
                      const PictImage &pict,
                      const NovaSpriteDefinition &definition) {
  if (definition.tiles_x != 1 || definition.tiles_y == 0 ||
      pict.width != definition.tile_width ||
      pict.height < static_cast<int>(definition.tile_height) *
                        static_cast<int>(definition.tiles_y) ||
      pict.rgba_pixels.size() <
          static_cast<std::size_t>(pict.width) *
              static_cast<std::size_t>(definition.tile_height) *
              static_cast<std::size_t>(definition.tiles_y) * 4) {
    return {};
  }

  std::vector<std::unique_ptr<SdlTexture>> textures;
  textures.reserve(definition.tiles_y);
  for (std::size_t frame = 0; frame < definition.tiles_y; ++frame) {
    const auto pixels = std::span<const std::uint8_t>{
        pict.rgba_pixels.data() +
            frame * static_cast<std::size_t>(definition.tile_height) *
                static_cast<std::size_t>(pict.width) * 4,
        static_cast<std::size_t>(pict.width) * definition.tile_height * 4};
    auto texture = SdlTexture::Create(
        renderer, pict.width, definition.tile_height, pixels);
    if (!texture) {
      return {};
    }
    textures.push_back(std::move(texture));
  }
  return textures;
}

[[nodiscard]] std::vector<std::unique_ptr<SdlTexture>>
LoadPictSpriteFrames(SDL_Renderer *renderer,
                     const NovaSpriteDefinition &definition) {
  const auto resource_data =
      NovaResource_Load(kResourceTypePict, definition.sprites_resource_id);
  if (!resource_data) {
    return {};
  }
  const auto pict = Resource_LoadPictAsImage(*resource_data);
  if (!pict) {
    return {};
  }
  return LoadStackedPictFrames(renderer, *pict, definition);
}

[[nodiscard]] bool MenuRowRevealed(const NovaRuntime &runtime,
                                   std::size_t row) {
  return row < runtime.main_menu_row_reveal_textures.size() &&
         (runtime.main_menu_row_reveal_textures[row].empty() ||
          runtime.menu_row_reveal_counters[row] >=
              static_cast<int>(
                  runtime.main_menu_row_reveal_textures[row].size()));
}

[[nodiscard]] bool MenuEntranceComplete(const NovaRuntime &runtime) {
  return MenuRowRevealed(runtime, 0) && MenuRowRevealed(runtime, 1) &&
         MenuRowRevealed(runtime, 2);
}

// Probe-harness support (docs/probe_harness.md): publish the menu's button
// rects so /probe/click can target them by intent ("new_pilot",
// "enter_ship", ...). MenuRect is in the authored menu canvas while
// /probe/click consumes window points, so map through the active placement.
// Only revealed rows are published, matching the hover hit-test's gating.
// No effect on game behaviour.
void PublishMainMenuProbeUi(NovaRuntime &runtime) {
  static constexpr std::array<std::string_view, kMenuEntries.size()> kNames{
      "new_pilot",
      "open_pilot",
      "quit_nova",
      "enter_ship",
      "set_prefs",
      "about_nova"};
  const auto &placement = runtime.platform.current_placement();
  std::vector<std::pair<std::string, SDL_FRect>> named;
  for (std::size_t index = 0; index < kMenuEntries.size(); ++index) {
    if (!MenuRowRevealed(runtime, index % 3)) {
      continue;
    }
    const SDL_FRect rect = MenuRect(runtime, index);
    named.emplace_back(std::string(kNames[index]),
                       placement.ToWindowRect(rect));
  }
  runtime.platform.PublishProbeUi("main_menu", std::move(named));
}

// @port 0x00475e20 80% ui
// The menu hover follows Sprite_TestOpaquePixelAtPoint (0x00475e20), whose
// bounds check includes the bottom/right edge (unlike the dialogs'
// Rect_ContainsPoint).
[[nodiscard]] bool MenuSpriteContainsOpaquePixel(const NovaRuntime &runtime,
                                                 std::size_t index,
                                                 SDL_FPoint point) {
  const auto rect = MenuRect(runtime, index);
  if (!ContainsInclusive(rect, point) ||
      index >= runtime.main_menu_sprite_assets.size() ||
      !runtime.main_menu_sprite_assets[index]) {
    return false;
  }
  const auto &sheet = runtime.main_menu_sprite_assets[index]->sheet;
  if (sheet.frames.empty() || rect.w <= 0.0F || rect.h <= 0.0F) {
    return false;
  }
  const auto x =
      std::min(sheet.width - 1,
               static_cast<int>((point.x - rect.x) *
                                static_cast<float>(sheet.width) / rect.w));
  const auto y =
      std::min(sheet.height - 1,
               static_cast<int>((point.y - rect.y) *
                                static_cast<float>(sheet.height) / rect.h));
  const auto alpha =
      sheet.frames[0]
          .rgba_pixels[static_cast<std::size_t>(y * sheet.width + x) * 4 + 3];
  return alpha != 0;
}

// Presents fixed artwork at its authored size, centred in a placement whose
// authored dimensions match the resource. PlaceContained composes the UI scale
// with the fit and supplies black margins when the window is larger.
void PresentSplashTexture(NovaRuntime &runtime, SDL_Texture *texture) {
  float width = 0.0F;
  float height = 0.0F;
  SDL_GetTextureSize(texture, &width, &height);
  const auto placement =
      PlaceContained({width, height},
                     runtime.platform.logical_playfield_size(),
                     runtime.platform.ui_scale());
  SdlPlatform::ScopedPlacement scope(runtime.platform, placement);
  const SDL_FRect destination{0.0F, 0.0F, width, height};
  SDL_RenderTexture(
      runtime.platform.renderer(), texture, nullptr, &destination);
}

// Progress-bar rectangle in native QuickDraw field
// order. Ghidra copies c\x9alr +0x5e..+0x64 straight into DAT_0085d099 and
// centers it on the render owner (NovaUi_RunProgressBarReveal 0x004ab1b0).
struct ProgressBarReferenceRect {
  int top = 0;
  int left = 0;
  int bottom = 0;
  int right = 0;

  void inscribe(int dh, int dv) {
    top += dv;
    left += dh;
    bottom -= dv;
    right -= dh;
  }
};

[[nodiscard]] SDL_FPoint StartupAuthoredSize(const NovaRuntime &runtime) {
  SDL_FPoint size{832.0F, 624.0F};
  if (runtime.startup_splash_texture) {
    SDL_GetTextureSize(runtime.startup_splash_texture->get(), &size.x, &size.y);
  }
  return size;
}

[[nodiscard]] ProgressBarReferenceRect
ProgressBarOutline(const NovaRuntime &runtime) {
  // Shipped c\x9alr (Nova Graphics 3, Colors record) fallback: a 200x10 bar.
  std::int16_t top = 280;
  std::int16_t left = -100;
  std::int16_t bottom = 290;
  std::int16_t right = 100;
  if (runtime.main_menu_style) {
    top = runtime.main_menu_style->progress_bar_top;
    left = runtime.main_menu_style->progress_bar_left;
    bottom = runtime.main_menu_style->progress_bar_bottom;
    right = runtime.main_menu_style->progress_bar_right;
  }
  const SDL_FPoint size = StartupAuthoredSize(runtime);
  const int center_x = static_cast<int>(size.x) / 2;
  const int center_y = static_cast<int>(size.y) / 2;
  return ProgressBarReferenceRect{
      center_y + top, center_x + left, center_y + bottom, center_x + right};
}

struct ProgressBarPalette {
  SDL_Color fill;
  SDL_Color inner;
  SDL_Color outer;
};

[[nodiscard]] ProgressBarPalette ProgressBarColors(const NovaRuntime &runtime) {
  if (runtime.main_menu_style) {
    return ProgressBarPalette{
        ToSdlColor(runtime.main_menu_style->progress_fill),
        ToSdlColor(runtime.main_menu_style->progress_inner),
        ToSdlColor(runtime.main_menu_style->progress_outer)};
  }
  return ProgressBarPalette{SDL_Color{255, 0, 0, SDL_ALPHA_OPAQUE},
                            SDL_Color{128, 0, 0, SDL_ALPHA_OPAQUE},
                            SDL_Color{64, 64, 64, SDL_ALPHA_OPAQUE}};
}

[[nodiscard]] SDL_FRect
ProgressBarLogicalRect(const ProgressBarReferenceRect &rect) {
  return SDL_FRect{static_cast<float>(rect.left),
                   static_cast<float>(rect.top),
                   static_cast<float>(rect.right - rect.left),
                   static_cast<float>(rect.bottom - rect.top)};
}

// Staged startup asset loads executed while the progress bar is visible.
// Order follows NovaGameSession_Run (Ghidra 0x00416100).
enum class StartupLoadStep : std::uint8_t {
  menu_sprites,
  backdrop,
  logo,
  center_preview,
  row_reveals,
  menu_sounds,
  count,
};

[[nodiscard]] std::optional<NovaSoundData>
LoadMenuSound(std::uint16_t sound_id) {
  if (const auto resource = NovaResource_LoadSndData(sound_id)) {
    if (auto decoded = NovaSound_Decode(*resource)) {
      return decoded;
    }
    NovaLog::Todo("menu snd \x20resource {} could not be decoded", sound_id);
  } else {
    NovaLog::Todo("menu snd \x20resource {} could not be located", sound_id);
  }
  return std::optional<NovaSoundData>{};
}

// One unit of deferred startup asset loading, run from the main loop while the
// progress bar is visible. The original performs the equivalent work in
// NovaGameSession_Run (Ghidra 0x00416100): FUN_004ad960 loads the menu sprites,
// then the backdrop/rollover art. The clean-room port counts each step as one
// progress unit instead of one 'ship' resource.
void RunStartupLoadStep(NovaRuntime &runtime, std::uint8_t step) {
  switch (static_cast<StartupLoadStep>(step)) {
  case StartupLoadStep::menu_sprites:
    for (std::size_t index = 0;
         index < runtime.main_menu_sprite_definitions.size();
         ++index) {
      const auto sprite_id = static_cast<std::uint16_t>(600 + index);
      runtime.main_menu_sprite_definitions[index] =
          NovaResource_LoadMainMenuSpriteDefinition(sprite_id);
      if (const auto &definition =
              runtime.main_menu_sprite_definitions[index]) {
        NovaLog::Info(
            "loaded sp\\x95n {}: image resource 0x{:04x}, mask resource "
            "0x{:04x}, "
            "{}x{} tiles ({}x{})",
            sprite_id,
            definition->sprites_resource_id,
            definition->mask_resource_id,
            definition->tile_width,
            definition->tile_height,
            definition->tiles_x,
            definition->tiles_y);
        runtime.main_menu_sprite_assets[index] =
            LoadMenuSpriteAsset(runtime.platform.renderer(), *definition);
        if (!runtime.main_menu_sprite_assets[index]) {
          NovaLog::Todo("main-menu rl\\x91D resource 0x{:04x} failed to decode",
                        definition->sprites_resource_id);
        }
      } else {
        NovaLog::Todo("main-menu sp\\x95n {} could not be loaded", sprite_id);
      }
    }
    break;
  case StartupLoadStep::backdrop:
    if (const auto backdrop_data = NovaResource_LoadMainMenuBackdropData()) {
      if (const auto pict = Resource_LoadPictAsImage(*backdrop_data)) {
        runtime.main_menu_backdrop_texture =
            SdlTexture::Create(runtime.platform.renderer(),
                               pict->width,
                               pict->height,
                               pict->rgba_pixels);
        if (!runtime.main_menu_backdrop_texture) {
          NovaLog::Error("main-menu backdrop PICT 0x1f40 decoded but SDL "
                         "texture upload failed");
        }
        // Kept for the OR-composited rollover preview (see NovaRuntime).
        runtime.main_menu_backdrop_rgba = pict->rgba_pixels;
        runtime.main_menu_backdrop_width = pict->width;
        runtime.main_menu_backdrop_height = pict->height;
      } else {
        NovaLog::Todo("main-menu backdrop PICT 0x1f40 failed to decode");
      }
    }
    break;
  case StartupLoadStep::logo:
    if (const auto definition =
            NovaResource_LoadMainMenuSpriteDefinition(606)) {
      runtime.main_menu_logo_textures =
          LoadPictSpriteFrames(runtime.platform.renderer(), *definition);
      if (runtime.main_menu_logo_textures.empty()) {
        NovaLog::Todo("main-menu top animation sp\\x95n 606 failed to load");
      }
    }
    break;
  case StartupLoadStep::center_preview:
    if (const auto definition =
            NovaResource_LoadMainMenuSpriteDefinition(607)) {
      runtime.main_menu_center_preview_asset =
          LoadMenuSpriteAsset(runtime.platform.renderer(), *definition);
      if (!runtime.main_menu_center_preview_asset) {
        NovaLog::Todo("main-menu center preview sp\\x95n 607 failed to load");
      }
    }
    break;
  case StartupLoadStep::row_reveals:
    for (std::size_t row = 0;
         row < runtime.main_menu_row_reveal_textures.size();
         ++row) {
      const auto sprite_id = static_cast<std::uint16_t>(608 + row);
      if (const auto definition =
              NovaResource_LoadMainMenuSpriteDefinition(sprite_id)) {
        runtime.main_menu_row_reveal_textures[row] =
            LoadPictSpriteFrames(runtime.platform.renderer(), *definition);
        if (runtime.main_menu_row_reveal_textures[row].empty()) {
          NovaLog::Todo("main-menu row reveal sp\\x95n {} failed to load",
                        sprite_id);
        }
      }
    }
    break;
  case StartupLoadStep::menu_sounds:
    // Ghidra NovaAudio_PreloadTransitionEffects: 600/601 mark focus
    // entry/exit, 602 starts a row reveal, 603 finishes it.
    runtime.menu_focus_enter_sound = LoadMenuSound(600);
    runtime.menu_focus_exit_sound = LoadMenuSound(601);
    runtime.menu_reveal_start_sound = LoadMenuSound(602);
    runtime.menu_reveal_finish_sound = LoadMenuSound(603);
    break;
  case StartupLoadStep::count:
    break;
  }
}

// @port 0x0048bc90 80% ui
void InitializeMenuEntrance(NovaRuntime &runtime, std::uint64_t now_ms) {
  runtime.menu_entrance_initialized = true;
  runtime.menu_top_animation_frame = 0;
  runtime.next_menu_top_animation_ms = now_ms + kMenuTitleFrameDurationMs;
  runtime.next_menu_reveal_frame_ms = now_ms;
  runtime.menu_center_preview_frame = 6;
  runtime.menu_center_preview_intensity = 0;
  runtime.menu_center_preview_last_update_ms = now_ms;

  int counter = -1;
  for (std::size_t row = 0; row < runtime.menu_row_reveal_counters.size();
       ++row) {
    runtime.menu_row_reveal_counters[row] = counter;
    // Ghidra 0x0048bc90 NovaHud_UpdateLayoutState: each subsequent strip starts
    // half the preceding strip's frame count behind the prior counter (0.5
    // multiplier at DAT_00575938).
    if (row + 1 < runtime.menu_row_reveal_counters.size()) {
      const auto frame_count =
          runtime.main_menu_row_reveal_textures[row].size();
      const auto stagger =
          static_cast<int>(std::lround(static_cast<double>(frame_count) * 0.5));
      counter -= stagger;
    }
  }
}

void UpdateMenuEntrance(NovaRuntime &runtime, std::uint64_t now_ms) {
  if (!runtime.menu_entrance_initialized) {
    InitializeMenuEntrance(runtime, now_ms);
  }

  if (runtime.main_menu_logo_textures.size() > 1 &&
      now_ms >= runtime.next_menu_top_animation_ms) {
    runtime.menu_top_animation_frame = (runtime.menu_top_animation_frame + 1) %
                                       runtime.main_menu_logo_textures.size();
    runtime.next_menu_top_animation_ms = now_ms + kMenuTitleFrameDurationMs;
  }

  if (now_ms >= runtime.next_menu_reveal_frame_ms) {
    runtime.next_menu_reveal_frame_ms = now_ms + kMenuRevealFrameDurationMs;
    for (std::size_t row = 0; row < runtime.menu_row_reveal_counters.size();
         ++row) {
      const auto frame_count =
          static_cast<int>(runtime.main_menu_row_reveal_textures[row].size());
      if (frame_count == 0 ||
          runtime.menu_row_reveal_counters[row] >= frame_count) {
        continue;
      }
      ++runtime.menu_row_reveal_counters[row];
      if (runtime.menu_row_reveal_counters[row] == 0 &&
          runtime.menu_reveal_start_sound) {
        runtime.audio.Play(*runtime.menu_reveal_start_sound,
                           1.0F,
                           1.0F,
                           /*sound_key=*/602,
                           /*priority_width=*/0x32);
      }
      if (runtime.menu_row_reveal_counters[row] == frame_count &&
          runtime.menu_reveal_finish_sound) {
        runtime.audio.Play(*runtime.menu_reveal_finish_sound,
                           1.0F,
                           1.0F,
                           /*sound_key=*/603,
                           /*priority_width=*/0x32);
      }
    }
  }
}

// @port 0x0048c210 70% ui
void UpdateMenuCenterPreview(NovaRuntime &runtime, std::uint64_t now_ms) {
  if (!runtime.main_menu_center_preview_asset ||
      runtime.main_menu_center_preview_asset->textures.empty() ||
      !MenuEntranceComplete(runtime)) {
    runtime.menu_center_preview_intensity = 0;
    return;
  }

  const auto &textures = runtime.main_menu_center_preview_asset->textures;
  const auto idle_frame = textures.size() - 1;
  const auto desired_frame =
      runtime.hovered_action ? static_cast<std::size_t>(*runtime.hovered_action)
                             : idle_frame;
  const auto elapsed_ms = std::min<std::uint64_t>(
      now_ms - runtime.menu_center_preview_last_update_ms, 100);
  runtime.menu_center_preview_last_update_ms = now_ms;
  // Ghidra 0x0048c210 NovaHud_UpdateFocusAnimationState: the fade eases ±4 per
  // 1ms tick toward 0x20 while the hovered lane is stable, and the lane only
  // switches once the fade has decayed to 0. NovaHud_RenderFocusOverlay draws
  // the frame opaque at fade 0x20 and via the tinted rgb15 blit below that
  // (BlitTintedRgb15), which a texture-alpha fade reproduces.
  const auto step = static_cast<int>(elapsed_ms) * 4;
  if (runtime.menu_center_preview_frame == desired_frame) {
    runtime.menu_center_preview_intensity =
        static_cast<std::uint8_t>(std::min<int>(
            32,
            runtime.menu_center_preview_intensity + std::max<int>(1, step)));
  } else if (runtime.menu_center_preview_intensity > 0) {
    runtime.menu_center_preview_intensity =
        static_cast<std::uint8_t>(std::max<int>(
            0, runtime.menu_center_preview_intensity - std::max<int>(1, step)));
  } else {
    runtime.menu_center_preview_frame = desired_frame;
  }
}

// @port 0x00469030 100%
// Ghidra 0x00469030 NovaUi_DrawCombatRankLabel. Maps the raw combat-rating
// points to a rank index (thresholds 100/200/400/800/1600/3200/6400/12800/
// 25600) and returns the STR# 0x8a label (1-based entry rank+1).
[[nodiscard]] std::string MenuCombatRankLabel(const game::GameState &state) {
  const int rank =
      game::NovaPlayerInfo_CombatRankIndex(state.player_combat_rating_points);
  auto label =
      game::NovaHud_LoadStringEntry(0x8a, static_cast<std::uint16_t>(rank + 1));
  return label.value_or(std::string());
}

// Ghidra 0x00468d90 NovaUi_DrawSystemFactionConflictStatus (with the
// System_HasUsableTravelDestination 0x00468af0 gate the caller applies first).
// Maps the system reputation against the owning government's CrimeTol (GovtDef
// 0x46, payload +0x08) onto the STR# 0x86 "Legal Status" ladder;
// hazard-bearing destinations report the Military Dictator/Governor rows and
// xenophobic governments (flags_primary bit 0) report nothing (index 0 ->
// STR# 0x7d2 0x18c "N/A"). The original's signed-overflow ladder reduces to
// plain comparisons for the shipped positive tolerances.
[[nodiscard]] std::string
MenuSystemLegalStatusText(const game::GameState &state,
                          std::int16_t system_id) {
  if (system_id < 0 ||
      static_cast<std::size_t>(system_id) >= state.scenario.systems.size()) {
    return {};
  }
  const auto *system =
      &state.scenario.systems[static_cast<std::size_t>(system_id)];
  constexpr int kNaStringEntry = 0x18c;
  // System_HasUsableTravelDestination over the first four nav defs.
  bool usable_destination = false;
  for (std::size_t i = 0; i < 4; ++i) {
    const std::int16_t nav = system->nav_defs[i];
    const auto *stellar = state.scenario.Stellar(nav);
    if (stellar == nullptr || (stellar->flags & 0x20U) != 0U ||
        (stellar->availability_flags & 0x3000U) != 0U) {
      continue;
    }
    usable_destination = true;
    break;
  }
  if (!usable_destination) {
    return game::NovaHud_LoadStringEntry(0x7d2, kNaStringEntry)
        .value_or(std::string());
  }

  int tolerance = !state.scenario.governments.empty()
                      ? state.scenario.governments.front().crime_tol
                      : 0;
  if (system->government_id >= 0) {
    if (const auto *gov =
            state.scenario.GovernmentByIndex(system->government_id)) {
      tolerance = gov->crime_tol;
    }
  }
  const int reputation =
      system_id >= 0 && static_cast<std::size_t>(system_id) <
                            state.system_reputation.size()
          ? state.system_reputation[static_cast<std::size_t>(system_id)]
          : 0;

  int status = 0;
  if (reputation < 0) {
    status = 2;
  }
  if (reputation < -tolerance) {
    status = 3;
  }
  if (reputation < tolerance * -4) {
    status = 4;
  }
  if (reputation < tolerance * -0x10) {
    status = 5;
  }
  if (reputation < tolerance * -0x40) {
    status = 6;
  }
  if (reputation < tolerance * -0x100) {
    status = 7;
  }
  if (reputation < tolerance * -0x400) {
    status = 8;
  }
  if (reputation < tolerance * -0x1000) {
    status = 9;
  }
  if (reputation == 0) {
    status = 1;
  }
  if (reputation > 0) {
    status = 10;
  }
  if (reputation >= tolerance * 4) {
    status = 11;
  }
  if (reputation > tolerance * 0x10) {
    status = 12;
  }
  if (reputation > tolerance * 0x40) {
    status = 13;
  }
  if (reputation > tolerance * 0x100) {
    status = 14;
  }
  if (reputation > tolerance * 0x400) {
    status = 15;
  }
  // Hazard-bearing nav destinations report the military rows (the original
  // scans nav slots 0..2 only).
  int normal_destinations = 0;
  int hazard_destinations = 0;
  for (std::size_t i = 0; i < 3; ++i) {
    const std::int16_t nav = system->nav_defs[i];
    const auto *stellar = state.scenario.Stellar(nav);
    if (stellar == nullptr || !stellar->is_available ||
        (stellar->flags & 0x20U) != 0U ||
        !game::NovaTargeting_IsStellarUsableForTravel(*stellar)) {
      continue;
    }
    if (stellar->dominated) {
      ++hazard_destinations;
    } else {
      ++normal_destinations;
    }
  }
  if (hazard_destinations > 0) {
    status = normal_destinations < 1 ? 17 : 16;
  }
  if (system->government_id >= 0) {
    if (const auto *gov =
            state.scenario.GovernmentByIndex(system->government_id);
        gov != nullptr && (gov->flags_primary & 1) != 0) {
      status = 0;
    }
  }
  if (status == 0) {
    return game::NovaHud_LoadStringEntry(0x7d2, kNaStringEntry)
        .value_or(std::string());
  }
  return game::NovaHud_LoadStringEntry(0x86,
                                       static_cast<std::uint16_t>(status + 1))
      .value_or(std::string());
}

void DrawMenuText(SdlPlatform &platform,
                  game::NovaFontCache &font_cache,
                  float x,
                  float baseline_y,
                  const std::string &text,
                  const SDL_Color &color) {
  game::NovaText_Draw(platform,
                      font_cache,
                      game::NovaFontFamily::kGeneva,
                      kMenuFontLogicalSize,
                      game::kNovaFontStyleRegular,
                      color,
                      x,
                      baseline_y,
                      text);
}

void DrawMenuTextCentered(SdlPlatform &platform,
                          game::NovaFontCache &font_cache,
                          float x_left,
                          float x_right,
                          float baseline_y,
                          const std::string &text,
                          const SDL_Color &color) {
  game::NovaText_DrawCentered(platform,
                              font_cache,
                              game::NovaFontFamily::kGeneva,
                              kMenuFontLogicalSize,
                              game::kNovaFontStyleRegular,
                              color,
                              x_left,
                              x_right,
                              baseline_y,
                              text);
}

// The pilot status panel (Ghidra 0x004873b0, DAT_00596d28 != 0 branch): two
// columns of Geneva status text below the buttons plus the target-pict ship
// portrait centered between them. All offsets below are 1024x768 backdrop
// space relative to the backdrop-frame centre, scaled by 0.625 like the art.
void DrawMenuStatusPanel(NovaRuntime &runtime) {
  SDL_Renderer *const renderer = runtime.platform.renderer();
  auto &font_cache = runtime.font_cache;
  const auto &game = runtime.game;
  constexpr float kOriginX = 512.0F;
  constexpr float kOriginY = 384.0F;
  constexpr float kLeftLabelX = kOriginX - 190.0F;
  constexpr float kLeftValueX = kOriginX - 185.0F;
  constexpr float kRightLabelX = kOriginX + 120.0F;
  constexpr float kRightValueX = kOriginX + 125.0F;
  const auto baseline = [&](int offset_1024) {
    return kOriginY + static_cast<float>(offset_1024);
  };

  // Destroyed-pilot branch: "<name> has been killed" (STR# 0x7d2 0x115); a
  // pilot named exactly Kenny gets the DAT_0056ce68 easter egg instead.
  if (game::NovaAiShip_IsDestroyed(game.player)) {
    std::string name = game.pilot.first_name;
    if (!game.pilot.last_name.empty()) {
      if (!name.empty()) {
        name += " ";
      }
      name += game.pilot.last_name;
    }
    std::string line;
    if (name == "Kenny") {
      line = "Oh my God! They killed Kenny!";
    } else {
      line = name + " " +
             game::NovaHud_LoadStringEntry(0x7d2, 0x115)
                 .value_or("has been killed");
    }
    DrawMenuTextCentered(runtime.platform,
                         font_cache,
                         kOriginX - 150.0F,
                         kOriginX + 150.0F,
                         baseline(0x136),
                         line,
                         kMenuLabelColor);
    return;
  }

  // Ghidra 0x004873b0 draws g_player_name here, not g_player_nickname. The
  // nickname is persisted separately for gameplay/dialog use but has no row in
  // the original main-menu status panel.
  const std::string &pilot_name = game.pilot.first_name;
  const auto *ship_class = game.scenario.Ship(
      static_cast<std::int16_t>(game.player.ship_class_id + 0x80));
  // Ghidra 0x004873b0 / 0x00468450: the live calendar with the pilot's
  // DatePrefix/DateSuffix (char template +0x13a/+0x14a), abbreviated months.
  const std::string date_text = game::NovaText_FormatDateString(
      game.date, true, game.date_prefix, game.date_suffix);

  struct StatusRow {
    float label_x;
    float value_x;
    int label_baseline;
    int value_baseline;
    std::string label;
    std::string value;
  };

  const std::array rows{
      StatusRow{
          kLeftLabelX,
          kLeftValueX,
          0xfa,
          0x106,
          game::NovaHud_LoadStringEntry(0x7d2, 0xfb).value_or("Pilot Name:"),
          pilot_name},
      StatusRow{
          kLeftLabelX,
          kLeftValueX,
          0x11e,
          0x12a,
          game::NovaHud_LoadStringEntry(0x7d2, 0xff).value_or("Ship Name:"),
          game.player.ship_name},
      StatusRow{
          kLeftLabelX,
          kLeftValueX,
          0x142,
          0x14e,
          game::NovaHud_LoadStringEntry(0x7d2, 0x100).value_or("Ship Class:"),
          ship_class != nullptr ? ship_class->display_name : ""},
      StatusRow{kLeftValueX,
                kLeftValueX,
                0x15a,
                0x15a,
                "",
                ship_class != nullptr ? ship_class->subtitle : ""},
      StatusRow{kRightLabelX,
                kRightLabelX,
                0xfa,
                0x106,
                game::NovaHud_LoadStringEntry(0x7d2, 0x116)
                    .value_or("Legal status in"),
                ""},
      StatusRow{kRightValueX,
                kRightValueX,
                0x106,
                0x112,
                game::NovaHud_LoadStringEntry(0x7d2, 0x117)
                    .value_or("current system:"),
                MenuSystemLegalStatusText(game, game.player.current_system_id)},
      StatusRow{
          kRightLabelX,
          kRightValueX,
          0x12a,
          0x136,
          game::NovaHud_LoadStringEntry(0x7d2, 0xfe).value_or("Combat Rating:"),
          MenuCombatRankLabel(game)},
      StatusRow{
          kRightLabelX,
          kRightValueX,
          0x14e,
          0x15a,
          game::NovaHud_LoadStringEntry(0x7d2, 0xfc).value_or("Current Date:"),
          date_text},
  };
  for (const auto &row : rows) {
    if (!row.label.empty()) {
      DrawMenuText(runtime.platform,
                   font_cache,
                   row.label_x,
                   baseline(row.label_baseline),
                   row.label,
                   kMenuLabelColor);
    }
    if (!row.value.empty()) {
      DrawMenuText(runtime.platform,
                   font_cache,
                   row.value_x,
                   baseline(row.value_baseline),
                   row.value,
                   kMenuValueColor);
    }
  }

  // Ship portrait (Ghidra:
  // g_ship_class_target_pict_images[target_pict_ship_class] blit, 128x64 class
  // portrait centered on the panel origin, top at origin + 0x118).
  const std::int16_t class_id = game.player.ship_class_id;
  if (runtime.menu_status_portrait_class != class_id) {
    runtime.menu_status_portrait.reset();
    runtime.menu_status_portrait_class = class_id;
    std::int16_t pict_class = class_id;
    if (ship_class != nullptr && ship_class->target_pict_ship_class >= 0) {
      pict_class = ship_class->target_pict_ship_class;
    }
    if (const auto pict_data = NovaResource_LoadPictData(
            static_cast<std::uint16_t>(3000 + pict_class))) {
      if (const auto pict = Resource_LoadPictAsImage(*pict_data)) {
        runtime.menu_status_portrait = SdlTexture::Create(
            renderer, pict->width, pict->height, pict->rgba_pixels);
      }
    }
  }
  if (runtime.menu_status_portrait) {
    float width = 0.0F;
    float height = 0.0F;
    SDL_GetTextureSize(runtime.menu_status_portrait->get(), &width, &height);
    const SDL_FRect destination{
        kOriginX - width / 2.0F, baseline(0x118), width, height};
    SDL_RenderTexture(
        renderer, runtime.menu_status_portrait->get(), nullptr, &destination);
  }
}

// Rebuilds the OR-composited rollover preview texture when the displayed
// frame changes.
// @port 0x0048c580 80% rendering
// Ghidra 0x0048c580 NovaHud_RenderFocusOverlay: at rest the
// frame is drawn through BlitPixie_BlitRectRawCopy -> BlitRaw, whose span
// primitive BlitPixel_CopyOrSpan (0x00473b60) ORs each 16-bit source pixel
// into the destination. The frames are authored for that: their "plate"
// pixels are near-black RGB555 and disappear under OR, while the bright
// glyph bits merge into the backdrop (skipped pixels keep the destination).
void UpdateCenterPreviewCompositedTexture(NovaRuntime &runtime) {
  const auto &asset = runtime.main_menu_center_preview_asset;
  if (!asset || asset->textures.empty() || !runtime.main_menu_style ||
      runtime.main_menu_backdrop_rgba.empty()) {
    return;
  }
  const auto frame =
      std::min(runtime.menu_center_preview_frame, asset->textures.size() - 1);
  if (runtime.main_menu_center_preview_composited_valid &&
      runtime.main_menu_center_preview_composited_frame == frame &&
      runtime.main_menu_center_preview_composited) {
    return;
  }

  const auto origin = runtime.main_menu_style->center_preview_origin;
  const int width = asset->sheet.width;
  const int height = asset->sheet.height;
  const auto &frame_pixels = asset->sheet.frames[frame].rgba_pixels;
  std::vector<std::uint8_t> composited(
      static_cast<std::size_t>(width) * height * 4, 0);
  for (int y = 0; y < height; ++y) {
    const int backdrop_y = origin.y + y;
    if (backdrop_y < 0 || backdrop_y >= runtime.main_menu_backdrop_height) {
      continue;
    }
    for (int x = 0; x < width; ++x) {
      const int backdrop_x = origin.x + x;
      if (backdrop_x < 0 || backdrop_x >= runtime.main_menu_backdrop_width) {
        continue;
      }
      const auto destination = (static_cast<std::size_t>(y) * width + x) * 4;
      const auto source = (static_cast<std::size_t>(backdrop_y) *
                               runtime.main_menu_backdrop_width +
                           backdrop_x) *
                          4;
      if (frame_pixels[destination + 3] == 0) {
        // RLE skip opcode: the destination pixel is kept.
        for (int c = 0; c < 3; ++c) {
          composited[destination + c] =
              runtime.main_menu_backdrop_rgba[source + c];
        }
      } else {
        // BlitPixel_CopyOrSpan: OR in 5-bit components, as the 16-bit
        // surface stores them.
        for (int c = 0; c < 3; ++c) {
          const auto merged = static_cast<std::uint8_t>(
              (runtime.main_menu_backdrop_rgba[source + c] >> 3) |
              (frame_pixels[destination + c] >> 3));
          composited[destination + c] =
              static_cast<std::uint8_t>((merged << 3) | (merged >> 2));
        }
      }
      composited[destination + 3] = 255;
    }
  }

  runtime.main_menu_center_preview_composited = SdlTexture::Create(
      runtime.platform.renderer(), width, height, composited);
  runtime.main_menu_center_preview_composited_frame = frame;
  runtime.main_menu_center_preview_composited_valid = true;
}

} // namespace

// @port 0x00503F30 100%
// Ghidra: 0x00503f30 NovaProgramEntry: main wrapper; the port-only top-level
// try/catch logs escaped exceptions (the original always had a console).
int NovaProgramEntry() {
  // Diagnostic-only mission-script/control-bit trace; inert unless
  // EVN_MISSION_TRACE is set (the probe can toggle it at runtime).
  game::MissionTrace::ConfigureFromEnvironment();
  NovaRuntime runtime;
  // Ghidra: NovaPrefs_ResetToDefaults (0x004b4320) seeds the preference globals
  // before any dialog reads them; the key-settings .prf load-overrides them
  // when available (NovaPrefs_LoadOrInit) and is deferred.
  runtime.prefs.ResetToDefaults();
  // Port-only top-level guard: the original always ran with a console and
  // standard handles present, but a windowed launcher can start this
  // console-subsystem binary detached. Log any escaped exception through the
  // non-throwing logger and exit with a failure code instead of aborting with
  // no diagnostic.
  try {
    return NovaApp_Run(runtime);
  } catch (const std::exception &e) {
    NovaLog::Error("fatal: unhandled exception: {}", e.what());
    return 1;
  } catch (...) {
    NovaLog::Error("fatal: unhandled non-standard exception");
    return 1;
  }
}

// @port 0x004D2A80 100%
// Ghidra: 0x004d2a80 NovaApp_Run: app entry / main menu loop.
int NovaApp_Run(NovaRuntime &runtime) {
  // Platform_RegisterMainWindow / QuickTime_Initialize are replaced by SDL
  // setup.
  if (!runtime.platform.Initialize()) {
    return 1;
  }
  if (!game::NovaPrefs_LoadFromSystemStore(runtime.prefs)) {
    NovaLog::Info("preferences: using original defaults");
  }
  // Apply the stored window mode before the first frame. The port defaults to
  // a window; an uncheck of "Run in a Window" switches the OS window to
  // fullscreen (the original's DDIsWindowed toggle, DAT_00bec178).
  runtime.platform.ApplyWindowMode(runtime.prefs.run_in_window);
  // Hand the command-query service the now-loaded binding table. Key Settings
  // later edits runtime.prefs.bindings in place, so the install stays current.
  game::NovaInput_InstallCommandBindings(&runtime.prefs.bindings);
  // The original normalizes the on-disk block once during startup too.
  (void)game::NovaPrefs_SaveToSystemStore(runtime.prefs);
  // Seed the runtime starmap-borders copy: the galaxy map edits GameState and
  // the .prf holds the preference, synced back at the save points below.
  runtime.game.starmap_show_borders = runtime.prefs.starmap_show_borders;
  // External probe harness (docs/probe_harness.md): the state reader runs on
  // the main thread at the pump, so it can safely walk the live GameState.
  runtime.platform.probe().SetStateProvider(
      [&runtime](const std::string &query) {
        return ProbeState_Snapshot(runtime.game, query);
      });
  runtime.platform.SetProbeAudioSuppressionHandler([&runtime](bool suppressed) {
    runtime.audio.SetPlaybackSuppressed(suppressed, [&runtime] {
      return runtime.platform.gameplay_ticks_ms();
    });
    runtime.music.SetPlaybackSuppressed(suppressed);
  });
  // Port-only bootstrap: apply a previously selected install root from the
  // extended prefs, then ask the player to locate the data when neither that
  // nor the normal candidate search finds an install. The original always
  // shipped its data beside the executable; the port can be launched from
  // anywhere, so it cannot silently fall back to a mock menu (see
  // game::NovaUi_RunLocateDataDialog).
  game::NovaExtraPrefs &extra_prefs = runtime.extra_prefs;
  if (!game::NovaExtraPrefs_LoadFromSystemStore(extra_prefs)) {
    NovaLog::Info("extra prefs: no 'EV Nova Extra Prefs.ini' yet");
  }
  // Resolve the presentation multipliers once at startup and hand them to the
  // platform, which owns the placement builders. All three are live:
  // `ui_scale` at the authored UI/HUD sites, `flight_scene_scale` on the
  // free-flight world, and `mission_scale` composed on top of `ui_scale` at the
  // two mission dialogs.
  const game::PresentationScale presentation =
      game::NovaExtraPrefs_ResolvePresentationScale(extra_prefs);
  runtime.platform.SetPresentationScale(presentation);
  NovaLog::Info("presentation scale: ui={:.3g} flight={:.3g} mission={:.3g}",
                presentation.ui,
                presentation.flight_scene,
                presentation.mission);
  if (extra_prefs.install_root) {
    NovaPaths::SetInstallRootOverride(extra_prefs.install_root);
  }
  if (!NovaPaths::InstallRoot()) {
    NovaLog::Warn("no EV Nova install root found; asking the player to locate "
                  "the Community Edition data");
    const auto located = game::NovaUi_RunLocateDataDialog(runtime.platform);
    if (!located) {
      NovaLog::Info("player quit from the locate-data screen");
      return 0;
    }
    NovaPaths::SetInstallRootOverride(*located);
    extra_prefs.install_root = *located;
    if (!game::NovaExtraPrefs_SaveToSystemStore(extra_prefs)) {
      NovaLog::Error("could not persist the selected EV Nova install root; "
                     "the current session will continue");
    }
    NovaLog::Info("using player-selected EV Nova install root '{}'",
                  located->string());
  }
  NovaGameSession_Run(runtime);
  // Ghidra NovaGameSession_Run calls NovaPrefs_SaveToDisk on exit; fold in the
  // runtime starmap-borders toggle so it round-trips even if the player never
  // reopened the Settings dialog.
  runtime.prefs.starmap_show_borders = runtime.game.starmap_show_borders;
  (void)game::NovaPrefs_SaveToSystemStore(runtime.prefs);
  return 0;
}

// @port 0x00416100 45% gameplay,divergence
// DIVERGENCE(original): clean-room session wrapper; resource/QuickTime
// startup, original teardown, and ancillary session init are not reproduced.
// Ghidra: 0x00416100 NovaGameSession_Run
void NovaGameSession_Run(NovaRuntime &runtime) {
  // Resource and QuickTime startup are not reconstructed yet. Licence checks
  // are deliberately skipped.
  runtime.game_active = false;
  // Ghidra 0x00416100 loads the scenario tables before
  // PilotData_AutoresumeLastPilot. Without that ordering a restored class id
  // has no definition, so the effective-stat pass gives it zero armor and the
  // main menu reports the otherwise valid pilot as killed.
  if (!runtime.game.scenario.LoadFromArchives(&runtime.game.rng,
                                              runtime.prefs.ship_animations)) {
    NovaLog::Error(
        "game session: scenario resource tables could not be loaded");
  }
  // Ghidra 0x0043bbb0 NovaResources_LoadMisnResourceDefs (the runtime half of
  // the mission-definition load). Runs right after the table load, matching
  // the original's bootstrap order, and clears any mission interaction latches
  // carried over from a previous session.
  game::Mission_ResetRuntimeStateOnMissionDefsLoad(runtime.game);
  // Ghidra 0x004B0C20 Ship_InitGameplayDataTables. The original seeds the
  // per-definition mission offering rolls before loading/resuming a pilot;
  // keep that observable RNG/table side effect in the session bootstrap.
  game::Mission_RerollOfferingRolls(runtime.game);
  // Ghidra 0x00416e9d (NovaGameSession_Run):
  // Game_ResetReputationAndAvailability seeds the session's baseline reputation
  // and availability before any pilot is loaded or resumed. (Replaces a plain
  // all-zero assign so the per-system government InitialRec floor is honoured.)
  game::NovaGame_ResetReputationAndAvailability(runtime.game,
                                                /*reset_combat_rating=*/true);
  // Idle main menu shows only the "No Pilot File Loaded" prompt (or, with a
  // pilot loaded, the status panel); the original draws no other status line
  // (Ghidra 0x004873b0).
  runtime.startup_phase = StartupPhase::loading_splash;
  runtime.startup_phase_started_ms = runtime.platform.wall_ticks_ms();
  // Ghidra: FUN_004ad960 loads sp\x95n 600-605 into DAT_00596cb8; the port
  // defers it (and the other menu assets) into the startup_splash phase so
  // the progress bar reflects real work. See RunStartupLoadStep.
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
  // 0x1fa4 (369x558 Ambrosia Software logo card); the startup splash is PICT
  // 0x83 (832x624 Escape Velocity Nova title art). Both are plain 16-bit
  // DirectBitsRect PICTs.
  const auto load_splash_texture = [&runtime](std::uint16_t resource_id) {
    if (const auto pict_data = NovaResource_LoadPictData(resource_id)) {
      if (const auto pict = Resource_LoadPictAsImage(*pict_data)) {
        auto texture = SdlTexture::Create(runtime.platform.renderer(),
                                          pict->width,
                                          pict->height,
                                          pict->rgba_pixels);
        if (!texture) {
          NovaLog::Error("PICT 0x{:04x} decoded but SDL texture upload "
                         "failed",
                         resource_id);
        }
        return texture;
      }
      NovaLog::Todo("PICT 0x{:04x} failed to decode", resource_id);
    } else {
      NovaLog::Error("PICT 0x{:04x} could not be loaded", resource_id);
    }
    return std::unique_ptr<SdlTexture>{};
  };
  runtime.loading_splash_texture = load_splash_texture(0x1fa4);
  runtime.startup_splash_texture = load_splash_texture(0x83);
  // The staged assets below are loaded while the startup progress bar is
  // visible (Ghidra NovaUi_RunProgressBarReveal -> asset loads). Total is the
  // number of steps; the original's denominator is the 'ship' resource count
  // (see NovaUi_RunProgressBarReveal).
  runtime.loading_progress_total = static_cast<double>(kStartupLoadStepCount);
  runtime.loading_progress_value = 0.0;
  runtime.startup_load_step = 0;
  if (!runtime.audio.Initialize()) {
    NovaLog::Warn("continuing without audio (menu sounds are silent)");
  } else {
    runtime.audio.SetMasterVolume(
        game::NovaAudio_EffectGainFromPreference(runtime.prefs.sound_volume));
  }

  // Background music. The shipped bass track is the MP3 in the Nova Files
  // folder (the original streams a :Music:SongNN path through a codec; the port
  // decodes the MP3 with dr_mp3 and loops the PCM). The music device/format is
  // set up here, matching NovaAudio_Initialize(8,0) running before the splash
  // frames in the original; Play() is deferred until the second splash becomes
  // active, then the same stream carries through into the main menu.
  if (!runtime.music.Initialize()) {
    NovaLog::Warn("continuing without background music (menu bass is silent)");
  } else if (const auto music_path =
                 NovaResource_LocateFile("Nova Music.mp3")) {
    if (runtime.music.Load(music_path->string())) {
      // Keep the menu bass at a comfortable level under the SFX blips.
      runtime.music.SetVolume(0.8F);
    }
  }

  NovaMainLoop_Run(runtime);
}

// Ghidra: 0x00486880 NovaMainLoop_Run
// @port 0x00486880 100%
void NovaMainLoop_Run(NovaRuntime &runtime) {
  while (!runtime.quit_requested && !runtime.platform.quit_requested()) {
    NovaMainLoop_UpdateFrame(runtime);
    NovaRender_RedrawAndPresentFrame(runtime, 1);
  }
}

// Ghidra: 0x00488080 NovaMainLoop_UpdateFrame
// @port 0x00488080 90% ui
void NovaMainLoop_UpdateFrame(NovaRuntime &runtime) {
  runtime.platform.SetPlacement(
      PlaceContained({1024.0F, 768.0F},
                     runtime.platform.logical_playfield_size(),
                     runtime.platform.ui_scale()));

  if (auto selection = runtime.platform.PollOpenFileDialogResult()) {
    if (!selection->error.empty()) {
      NovaLog::Error("Open Pilot file dialog failed: {}", selection->error);
    } else if (selection->path) {
      game::NovaShip_ResetPlayerShipState(runtime.game);
      // Ghidra 0x004c9e90: Game_ResetReputationAndAvailability(1) follows the
      // ship reset and precedes the save load.
      game::NovaGame_ResetReputationAndAvailability(
          runtime.game, /*reset_combat_rating=*/true);
      const game::PilotLoadError result =
          game::PilotFileLoadSave(*selection->path, runtime.game);
      if (result == game::PilotLoadError::kOk ||
          result == game::PilotLoadError::kRepairsApplied) {
        runtime.game.game_active = true;
        runtime.game.player.is_active = true;
        runtime.game_active = true;
        runtime.menu_status_portrait.reset();
        runtime.menu_status_portrait_class = -1;
        // Ghidra 0x004c9e90 / action 1 tail: clear the previous session's
        // vacant ships, restore mission fleets, rebuild the loaded system's
        // ambient NPC population, and fire region triggers for every visible
        // discovered system. Player escorts are restored by PilotFileLoadSave
        // and spared by the keep_player_engaged=false sweep, so they are not
        // rebuilt here. EvaluateAvailability and Asteroid_InitSystem are left
        // to the spaceflight pre-loop; the mission rearm tail is already
        // applied by the loader.
        game::NovaShip_DeactivateVacantShipsAndTally(
            runtime.game, /*keep_player_engaged=*/false);
        game::NovaSystem_RestoreMissionFleets(
            runtime.game,
            runtime.game.player.current_system_id,
            /*copy_player_heading=*/true,
            runtime.platform.gameplay_ticks_ms());
        game::NovaSystem_PopulateInitialNpcShips(
            runtime.game, runtime.game.player.current_system_id);
        // 0x004870c4-0x004870e7: is_visible (+0x1eb) and discovery_state
        // (+0x90) > 0. The decompiler's "personality_slots - 8" is the
        // discovery_state word: personality_slots is a short[8] at +0x98, so
        // array decay minus 8 is +0x90.
        for (std::size_t system_index = 0;
             system_index < runtime.game.scenario.systems.size();
             ++system_index) {
          const auto &system = runtime.game.scenario.systems[system_index];
          if (system.is_visible && system.discovery_state > 0) {
            game::NovaSystem_TriggerNebulaRegionEvents(
                runtime.game, static_cast<std::int16_t>(system_index));
          }
        }
        NovaLog::Info("opened pilot file '{}'{}",
                      selection->path->string(),
                      result == game::PilotLoadError::kRepairsApplied
                          ? " (repairs applied)"
                          : "");
        if (result == game::PilotLoadError::kRepairsApplied) {
          NovaLog::Todo("Open Pilot repair warning dialog (original STR# "
                        "0x8c entry 0x34) is not ported; repair details were "
                        "written to the log");
        }
      } else {
        NovaLog::Error("could not open pilot file '{}': loader error {}",
                       selection->path->string(),
                       static_cast<int>(result));
      }
    }
  }

  if (const auto command = runtime.platform.PollCommandEvent()) {
    if (*command == 'q') {
      runtime.requested_action = GameModeAction::quit;
    } else if (runtime.startup_phase == StartupPhase::main_menu) {
      if (*command == 'm') {
        runtime.requested_action = NovaHud_TrackFocusHoverIndex(runtime);
      } else {
        runtime.requested_action = NovaCommand_DispatchToMode(*command);
      }
    }
  }

  const auto now_ms = runtime.platform.wall_ticks_ms();
  if (runtime.startup_phase == StartupPhase::loading_splash &&
      now_ms - runtime.startup_phase_started_ms >= kLoadingSplashDurationMs) {
    runtime.startup_phase = StartupPhase::startup_splash;
    runtime.startup_phase_started_ms = now_ms;
  } else if (runtime.startup_phase == StartupPhase::startup_splash) {
    const auto phase_elapsed_ms = now_ms - runtime.startup_phase_started_ms;
    // The original shows the startup splash by itself only while it runs the
    // full-catalog resource checksum pass (FUN_004cd7e0,
    // ResourceData_VerifyCatalogChecksum, deliberately skipped here), then
    // reveals the bar over the same splash (NovaUi_RunProgressBarReveal
    // 0x004ab1b0). With no checksum pass there is nothing to hold for, so the
    // bar is revealed on the first startup-splash frame.
    if (!runtime.startup_progress_started) {
      runtime.startup_progress_started = true;
      runtime.startup_load_step_started_ms = now_ms;
      runtime.loading_progress_last_ms = now_ms;
      NovaUi_RunProgressBarReveal(runtime);
    }
    // Advance the expand-in wipe first, then run one deferred asset load per
    // step. The bar's displayed value chases the real completed work at a
    // constant rate so it sweeps instead of jumping between the six loads.
    if (runtime.loading_progress_reveal_inset > 0) {
      --runtime.loading_progress_reveal_inset;
    } else if (runtime.startup_load_step < kStartupLoadStepCount &&
               now_ms - runtime.startup_load_step_started_ms >=
                   kStartupLoadStepDwellMs) {
      RunStartupLoadStep(runtime, runtime.startup_load_step);
      ++runtime.startup_load_step;
      NovaUi_AddProgressAndRedraw(runtime, 1.0);
      runtime.startup_load_step_started_ms = now_ms;
    }
    const auto display_dt_ms = now_ms - runtime.loading_progress_last_ms;
    runtime.loading_progress_last_ms = now_ms;
    const double display_rate = static_cast<double>(kStartupLoadStepCount) /
                                static_cast<double>(kStartupProgressFillMs);
    runtime.loading_progress_displayed =
        std::min(runtime.loading_progress_value,
                 runtime.loading_progress_displayed +
                     display_rate * static_cast<double>(display_dt_ms));
    // Keep the splash (and the completed bar) up until the minimum has
    // elapsed and the smoothed fill has caught up to the real progress.
    if (runtime.startup_load_step >= kStartupLoadStepCount &&
        runtime.loading_progress_displayed >= runtime.loading_progress_value &&
        phase_elapsed_ms >= kStartupSplashDurationMs) {
      const game::PilotLoadError resume =
          game::PilotData_AutoresumeLastPilot(runtime.game);
      if (resume == game::PilotLoadError::kOk) {
        runtime.game.game_active = true;
        runtime.game.player.is_active = true;
        runtime.game_active = true;
        NovaLog::Info("auto-resumed pilot '{}'", runtime.game.pilot.first_name);
      } else if (resume == game::PilotLoadError::kRepairsApplied) {
        NovaLog::Warn("startup pilot required repairs and was not "
                      "auto-resumed; use Open Pilot to load it explicitly");
      }
      runtime.startup_phase = StartupPhase::main_menu;
      runtime.startup_phase_started_ms = now_ms;
      InitializeMenuEntrance(runtime, now_ms);
    }
  }

  // The menu bass begins with the second (Nova title) splash and is
  // deliberately not restarted at main-menu entry, so playback carries across
  // the boundary.
  if (runtime.startup_phase != StartupPhase::loading_splash &&
      !runtime.menu_music_started) {
    runtime.menu_music_started = true;
    runtime.music.Play();
  }

  if (runtime.startup_phase == StartupPhase::main_menu) {
    UpdateMenuEntrance(runtime, now_ms);
    runtime.hovered_action = NovaHud_TrackFocusHoverIndex(runtime);
    UpdateMenuCenterPreview(runtime, now_ms);
    PublishMainMenuProbeUi(runtime);
  } else {
    runtime.hovered_action.reset();
  }

  // Original focus transitions use handle 600 when entering a button and 601
  // when leaving one. Moving directly between entries therefore plays 600.
  if (runtime.hovered_action != runtime.previous_hovered_action) {
    if (runtime.hovered_action && runtime.menu_focus_enter_sound) {
      runtime.audio.Play(*runtime.menu_focus_enter_sound,
                         1.0F,
                         1.0F,
                         /*sound_key=*/600,
                         /*priority_width=*/10);
    } else if (!runtime.hovered_action && runtime.previous_hovered_action &&
               runtime.menu_focus_exit_sound) {
      runtime.audio.Play(*runtime.menu_focus_exit_sound,
                         1.0F,
                         1.0F,
                         /*sound_key=*/601,
                         /*priority_width=*/10);
    }
  }
  runtime.previous_hovered_action = runtime.hovered_action;

  if (runtime.requested_action) {
    // NovaAudio_PlayTransitionEffectsWait plays focus-in then focus-out around
    // keyboard actions. Dispatch remains asynchronous in this SDL loop.
    if (runtime.menu_focus_enter_sound) {
      runtime.audio.Play(*runtime.menu_focus_enter_sound,
                         1.0F,
                         1.0F,
                         /*sound_key=*/600,
                         /*priority_width=*/10);
    }
    NovaGameMode_DispatchAction(runtime, *runtime.requested_action);
    runtime.requested_action.reset();
  }
}

// Ghidra: 0x004873b0 NovaRender_RedrawAndPresentFrame
// @port 0x0048c3c0 60% rendering
// TODO(decomp): the jitter-scroll and decay-counter motion is approximated by
// the reveal counters.
// @port 0x004873b0 100%
void NovaRender_RedrawAndPresentFrame(NovaRuntime &runtime, short mode) {
  SDL_Renderer *const renderer = runtime.platform.renderer();
  // Main-menu art is authored for the 1024x768 canvas and is contained at the
  // active UI scale, with black margins in larger windows.
  runtime.platform.SetPlacement(
      PlaceContained({1024.0F, 768.0F},
                     runtime.platform.logical_playfield_size(),
                     runtime.platform.ui_scale()));
  if (runtime.startup_phase == StartupPhase::loading_splash) {
    NovaUi_PresentLoadingSplashFrame(runtime);
    runtime.platform.Present();
    return;
  }
  if (runtime.startup_phase == StartupPhase::startup_splash) {
    NovaUi_PresentStartupSplashFrame(runtime);
    // Draw the bar over the splash once the reveal has started; the redraw
    // itself applies the expand-in wipe inset (NovaUi_RedrawProgressBar
    // 0x004ab3d0). Before that the splash is shown alone.
    if (runtime.startup_progress_started) {
      NovaUi_RedrawProgressBar(runtime);
    }
    runtime.platform.Present();
    return;
  }

  // Real title-screen backdrop: the native 1024x768 ship-interior PICT
  // 0x1f40, drawn directly in the menu's authored placement. A missing PICT
  // leaves this black clear in place; the loader has already logged the error.
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(renderer);
  if (runtime.main_menu_backdrop_texture) {
    constexpr SDL_FRect menu_backdrop_rect{0.0F, 0.0F, 1024.0F, 768.0F};
    SDL_RenderTexture(renderer,
                      runtime.main_menu_backdrop_texture->get(),
                      nullptr,
                      &menu_backdrop_rect);
  }

  // Top background animation (sp\x95n 606, PICT 0x1f4a), drawn at c\x9alr
  // +0xe0. Frame_TickTimerDecayAndEffects selects a different random frame on
  // its short animation timer; UpdateMenuEntrance maintains the equivalent
  // no-repeat progression.
  // Ghidra 0x0048c3c0 NovaHud_RenderOverlays: the main-menu overlay compositor
  // (top logo strip + three button-slide reveal channels) folded into this
  // frame renderer; the original's offscreen DrawContext push/blit becomes
  // direct SDL draws at the computed origins.
  if (!runtime.main_menu_logo_textures.empty() && runtime.main_menu_style) {
    const auto frame = std::min(runtime.menu_top_animation_frame,
                                runtime.main_menu_logo_textures.size() - 1);
    const auto &logo_texture = runtime.main_menu_logo_textures[frame];
    float logo_width = 0.0F;
    float logo_height = 0.0F;
    SDL_GetTextureSize(logo_texture->get(), &logo_width, &logo_height);
    const SDL_FRect logo_destination{
        static_cast<float>(runtime.main_menu_style->logo_origin.x),
        static_cast<float>(runtime.main_menu_style->logo_origin.y),
        logo_width,
        logo_height,
    };
    SDL_RenderTexture(
        renderer, logo_texture->get(), nullptr, &logo_destination);
  }

  // The original entrance does not interpolate the button sprites. It plays
  // authored PICT strips 608-610 behind each pair, then replaces a completed
  // strip with the two normal focus sprites for that row.
  for (std::size_t row = 0; row < runtime.main_menu_row_reveal_textures.size();
       ++row) {
    const auto &textures = runtime.main_menu_row_reveal_textures[row];
    const auto counter = runtime.menu_row_reveal_counters[row];
    if (!runtime.main_menu_style || counter < 0 ||
        counter >= static_cast<int>(textures.size()) || textures.empty()) {
      continue;
    }
    const auto origin = runtime.main_menu_style->row_reveal_origins[row];
    float width = 0.0F;
    float height = 0.0F;
    SDL_GetTextureSize(
        textures[static_cast<std::size_t>(counter)]->get(), &width, &height);
    const SDL_FRect destination{
        static_cast<float>(origin.x),
        static_cast<float>(origin.y),
        width,
        height,
    };
    SDL_RenderTexture(renderer,
                      textures[static_cast<std::size_t>(counter)]->get(),
                      nullptr,
                      &destination);
  }

  for (std::size_t index = 0; index < runtime.main_menu_sprite_assets.size();
       ++index) {
    if (!runtime.main_menu_sprite_assets[index] ||
        !MenuRowRevealed(runtime, index % 3)) {
      continue;
    }
    const auto &asset = *runtime.main_menu_sprite_assets[index];
    std::size_t frame_index = 0;
    if (index < kMenuEntries.size() &&
        runtime.hovered_action == kMenuEntries[index].action &&
        asset.textures.size() > 1) {
      frame_index = 1;
    }
    const auto destination = MenuRect(runtime, index);
    SDL_RenderTexture(
        renderer, asset.textures[frame_index]->get(), nullptr, &destination);
  }

  // Center preview (sp\x95n 607): frames 0-5 correspond to the menu actions,
  // frame 6 is idle. The original fades the old frame out before switching.
  if (runtime.main_menu_style && runtime.main_menu_center_preview_asset &&
      !runtime.main_menu_center_preview_asset->textures.empty() &&
      runtime.menu_center_preview_intensity > 0) {
    const auto &asset = *runtime.main_menu_center_preview_asset;
    const auto frame =
        std::min(runtime.menu_center_preview_frame, asset.textures.size() - 1);
    const auto origin = runtime.main_menu_style->center_preview_origin;
    const SDL_FRect destination{
        static_cast<float>(origin.x),
        static_cast<float>(origin.y),
        static_cast<float>(asset.sheet.width),
        static_cast<float>(asset.sheet.height),
    };
    const auto alpha = static_cast<std::uint8_t>(
        static_cast<unsigned>(runtime.menu_center_preview_intensity) * 255U /
        32U);
    // Rest-state drawing ORs the frame into the backdrop (BlitRaw /
    // BlitPixel_CopyOrSpan); draw the pre-composited texture when the real
    // backdrop is available, the raw frame over the fallback background
    // otherwise. The alpha fade approximates the original's 8ms tinted
    // cross-fade (BlitTintedRgb15).
    SDL_Texture *preview_texture = asset.textures[frame]->get();
    if (runtime.main_menu_style && !runtime.main_menu_backdrop_rgba.empty()) {
      UpdateCenterPreviewCompositedTexture(runtime);
      if (runtime.main_menu_center_preview_composited) {
        preview_texture = runtime.main_menu_center_preview_composited->get();
      }
    }
    SDL_SetTextureAlphaMod(preview_texture, alpha);
    SDL_RenderTexture(renderer, preview_texture, nullptr, &destination);
    SDL_SetTextureAlphaMod(asset.textures[frame]->get(), SDL_ALPHA_OPAQUE);
  }

  // Bottom text block (Ghidra 0x004873b0). Without a pilot the menu shows
  // STR# 0x7d2 entry 0x114 ("No Pilot File Loaded") centered under the
  // buttons; it appears once the third slide reveal completes and stays
  // steady (the draw gate is reveal-counter >= threshold, not a blink timer).
  // With a pilot loaded it is replaced by the two-column pilot status panel.
  if (MenuEntranceComplete(runtime)) {
    if (!runtime.game.game_active) {
      if (auto prompt = game::NovaHud_LoadStringEntry(0x7d2, 0x114)) {
        DrawMenuTextCentered(runtime.platform,
                             runtime.font_cache,
                             512.0F - 150.0F,
                             512.0F + 150.0F,
                             384.0F + 310.0F,
                             *prompt,
                             kMenuLabelColor);
      }
    } else {
      DrawMenuStatusPanel(runtime);
    }
  }

  if (mode == 1) {
    runtime.platform.Present();
  }
}

// @port 0x004AB070 100%
// Ghidra: 0x004ab070 NovaUi_PresentLoadingSplashFrame
void NovaUi_PresentLoadingSplashFrame(NovaRuntime &runtime) {
  if (runtime.loading_splash_texture) {
    SDL_SetRenderDrawColor(
        runtime.platform.renderer(), 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(runtime.platform.renderer());
    PresentSplashTexture(runtime, runtime.loading_splash_texture->get());
    return;
  }
  SDL_SetRenderDrawColor(
      runtime.platform.renderer(), 0, 0, 0, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(runtime.platform.renderer());
}

// @port 0x004AAF60 100%
// @port 0x004AB180 100%
// Ghidra: 0x004aaf60 NovaUi_PresentStartupSplashFrame. The black pre-clear
// here also subsumes NovaUi_ClearMainWindowAndHoldFrame (0x004ab180): that
// routine's Frame_CommitFrameAndLatchTransitionWait (0x00467de0) plus its
// BOOL_007354a8 latch are replaced by SdlPlatform::Present(), and its
// full-window black fill by SDL_RenderClear below (the loading-splash
// presenter does the same). Same deliberate skip as spaceflight.cpp:4267.
void NovaUi_PresentStartupSplashFrame(NovaRuntime &runtime) {
  if (runtime.startup_splash_texture) {
    SDL_SetRenderDrawColor(
        runtime.platform.renderer(), 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(runtime.platform.renderer());
    PresentSplashTexture(runtime, runtime.startup_splash_texture->get());
    return;
  }
  SDL_SetRenderDrawColor(
      runtime.platform.renderer(), 0, 0, 0, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(runtime.platform.renderer());
}

// @port 0x004AB3A0 100%
// Ghidra: 0x004ab3a0 NovaUi_ProgressCallbackNoOp. The startup path passes this
// as a progress sink where no redraw is wanted; kept as the faithful no-op.
void NovaUi_ProgressCallbackNoOp() {}

// @port 0x004AB1B0 80% cadence
// Ghidra: 0x004ab1b0 NovaUi_RunProgressBarReveal
void NovaUi_RunProgressBarReveal(NovaRuntime &runtime) {
  // The original resets value to 0 and seeds total from the 'ship' resource
  // count (ResourceData_CountEntries 0x73689570). The port instead sets total
  // from its staged startup loads in NovaGameSession_Run (documented
  // divergence: the ship-class visual tables are not preloaded at startup yet).
  runtime.loading_progress_value = 0.0;
  runtime.loading_progress_displayed = 0.0;
  // The original runs the expand-in wipe synchronously with MarkTickAndWait(2),
  // starting from a rect inset by half its height. The SDL main loop advances
  // one reference pixel per frame via loading_progress_reveal_inset.
  const auto outline = ProgressBarOutline(runtime);
  runtime.loading_progress_reveal_inset =
      (outline.bottom - outline.top + 1) / 2;
}

// @port 0x004AB3B0 100%
// Ghidra: 0x004ab3b0 NovaUi_AddProgressAndRedraw
void NovaUi_AddProgressAndRedraw(NovaRuntime &runtime, double delta) {
  runtime.loading_progress_value += delta;
  NovaUi_RedrawProgressBar(runtime);
}

// @port 0x004AB3D0 85% rendering
// Ghidra: 0x004ab3d0 NovaUi_RedrawProgressBar
void NovaUi_RedrawProgressBar(NovaRuntime &runtime) {
  // The bar's c\x9alr offsets are relative to the same centre as the splash.
  // Use its containment factor too, so smaller windows do not shrink the bar
  // through an unrelated menu canvas.
  const SdlPlatform::ScopedPlacement placement(
      runtime.platform,
      PlaceContained(StartupAuthoredSize(runtime),
                     runtime.platform.logical_playfield_size(),
                     runtime.platform.ui_scale()));
  const ProgressBarPalette colors = ProgressBarColors(runtime);
  ProgressBarReferenceRect outer = ProgressBarOutline(runtime);
  outer.top += runtime.loading_progress_reveal_inset;
  outer.bottom -= runtime.loading_progress_reveal_inset;
  if (outer.bottom <= outer.top || outer.right <= outer.left) {
    return;
  }
  SDL_Renderer *const renderer = runtime.platform.renderer();

  // Outer 1px outline (Ghidra: FrameRect in the ProgOutline colour).
  SDL_SetRenderDrawColor(renderer,
                         colors.outer.r,
                         colors.outer.g,
                         colors.outer.b,
                         SDL_ALPHA_OPAQUE);
  const SDL_FRect outline_rect = ProgressBarLogicalRect(outer);
  SDL_RenderRect(renderer, &outline_rect);

  const double ratio = runtime.loading_progress_total > 0.0
                           ? std::clamp(runtime.loading_progress_displayed /
                                            runtime.loading_progress_total,
                                        0.0,
                                        1.0)
                           : 0.0;

  ProgressBarReferenceRect inner = outer;
  inner.inscribe(1, 1);
  int fill_right = static_cast<int>(std::lround(
      static_cast<double>(inner.left) + ratio * kProgressBarFillSpan));
  if (fill_right >= outer.right) {
    fill_right = outer.right - 1;
  }

  // ProgBright fill (Ghidra: FillRect after a second 1px inset).
  ProgressBarReferenceRect fill = outer;
  fill.inscribe(2, 2);
  fill.right = fill_right - 1;
  if (fill.right > fill.left) {
    SDL_SetRenderDrawColor(renderer,
                           colors.fill.r,
                           colors.fill.g,
                           colors.fill.b,
                           SDL_ALPHA_OPAQUE);
    const SDL_FRect fill_rect = ProgressBarLogicalRect(fill);
    SDL_RenderFillRect(renderer, &fill_rect);
  }

  // ProgDim outline around the filled extent (Ghidra: FrameRect restored to the
  // first inset and extended to the fill edge).
  ProgressBarReferenceRect fill_frame = fill;
  fill_frame.inscribe(-1, -1);
  fill_frame.right = fill_right;
  SDL_SetRenderDrawColor(renderer,
                         colors.inner.r,
                         colors.inner.g,
                         colors.inner.b,
                         SDL_ALPHA_OPAQUE);
  const SDL_FRect fill_frame_rect = ProgressBarLogicalRect(fill_frame);
  SDL_RenderRect(renderer, &fill_frame_rect);

  // Remaining trough (Ghidra: FillRect with PTR_DAT_00575acc).
  ProgressBarReferenceRect trough = inner;
  trough.left = fill_right;
  trough.right = outer.right - 1;
  if (trough.right > trough.left) {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    const SDL_FRect trough_rect = ProgressBarLogicalRect(trough);
    SDL_RenderFillRect(renderer, &trough_rect);
  }
}

// @port 0x00486ed0 100%
// @port 0x004C9E90 85% gameplay,ui
// Ghidra: 0x00486ed0 NovaGameMode_DispatchAction. Also carries the inline
// Menu_OpenPilotFileDialog (0x004c9e90) native chooser; the async result is
// applied by NovaMainLoop_UpdateFrame. Remaining: reputation reset, error
// dialogs, and the STR# 0x8c entry 0x34 repair warning.
void NovaGameMode_DispatchAction(NovaRuntime &runtime, GameModeAction action) {
  // Probe-harness hygiene (docs/probe_harness.md): every mode change drops the
  // outgoing screen's published rects so /probe/ui never reports stale layout
  // (e.g. main-menu buttons while in flight). Each screen republishes on its
  // own frames; modals clear themselves via ProbeUiAutoClear.
  runtime.platform.ClearProbeUi();
  switch (action) {
  case GameModeAction::new_game: {
    // Ghidra 0x00486ed0 action 0: when a game is already active the original
    // loads STR# 0x8c row 8 and confirms through Ui_ShowConfirmDialog (DLOG
    // 0xbba) before Menu_RunNewGameFlow discards the running pilot. A declined
    // prompt leaves the active game untouched.
    if (runtime.game.game_active) {
      game::NovaFontCache confirm_fonts;
      const std::string prompt =
          game::NovaHud_LoadStringEntry(0x8c, 8).value_or(
              "A game is already in progress. Start a new one?");
      if (!game::NovaUi_ShowConfirmDialog(
              runtime.platform, confirm_fonts, prompt, [&runtime] {
                NovaRender_RedrawAndPresentFrame(runtime, 0);
              })) {
        NovaLog::Info("new game declined: active game kept");
        break;
      }
    }
    // Runs the modal new-pilot flow (naming, confirm, reset, scenario load,).
    // On success the flow marks the game active and we return to the menu; the
    // player then chooses ENTER SPACE to play (intro cinematic plays then).
    // The dialogs keep re-rendering the menu behind themselves each frame.
    NovaRender_RedrawAndPresentFrame(runtime, 0);
    if (game::NovaNewPilotFlow_Run(
            runtime.platform,
            runtime.game,
            runtime.prefs.ship_animations,
            [&runtime] { NovaRender_RedrawAndPresentFrame(runtime, 0); })) {
      NovaLog::Info("new pilot created; choose ENTER SHIP to fly");
    } else {
      NovaLog::Info("new game cancelled");
    }
    break;
  }
  case GameModeAction::open_pilot:
    if (!runtime.platform.ShowOpenPilotFileDialog()) {
      NovaLog::Info("Open Pilot file dialog is already active");
    }
    break;
  case GameModeAction::quit:
    runtime.quit_requested = true;
    break;
  case GameModeAction::enter_spaceflight: {
    // Ghidra: param_1 == 3. Run spaceflight only when there is an active,
    // living pilot; otherwise play the error blip (NovaEffects_QueueCentered
    // Resource of the transition sound table entry 3, which the placeholder
    // build cannot show, so it is logged instead).
    const auto player_alive = runtime.game.player.is_active &&
                              runtime.game.player.death_timer_active < 0.0F;
    if (!runtime.game.game_active || !player_alive) {
      NovaLog::Info("ENTER SHIP ignored: no active living pilot");
      break;
    }
    // Blocking: plays the intro cinematic on first entry, then the in-game
    // main loop, returning to the menu when the pilot exits. Mirrors
    // Ship_RunSpaceflightMode being called inline from the dispatcher.
    const bool resume_menu_music = runtime.prefs.intro_music;
    runtime.music.Stop();
    game::NovaSpaceflight_Run(
        runtime.platform, runtime.audio, runtime.game, runtime.prefs);
    if (resume_menu_music) {
      runtime.music.Play();
    }
    break;
  }
  case GameModeAction::preferences: {
    // Blocking Settings modal (Ghidra Menu_RunSettingsDialog 0x00488650, DLOG
    // 0xfa3). Edits runtime.prefs in place; OK commits the .prf (deferred) and
    // Esc/Cancel discards. Returns to the main menu either way.
    game::NovaFontCache font_cache;
    // Fold the runtime starmap-borders toggle into the preference before the
    // dialog's OK path persists the .prf.
    runtime.prefs.starmap_show_borders = runtime.game.starmap_show_borders;
    const bool saved = game::NovaMenu_RunSettingsDialog(
        runtime.platform,
        runtime.audio,
        runtime.music,
        font_cache,
        runtime.prefs,
        runtime.extra_prefs,
        [&runtime] { NovaRender_RedrawAndPresentFrame(runtime, 0); });
    NovaLog::Info("preferences {}", saved ? "saved" : "cancelled");
    // Force a redraw so the menu backdrop (and any brightness change) is seen.
    NovaRender_RedrawAndPresentFrame(runtime, 1);
    break;
  }
  case GameModeAction::about_nova: {
    // Ghidra: param_1 == 5 -> 0x00486120 Menu_RunAboutNovaDialog (sp\x95n 605
    // button labelled ABOUT NOVA, shortcut 'a', loads the d\x91sc 0x7fff
    // "About text" resource).
    // The modal keeps re-rendering the menu behind itself each frame.
    NovaRender_RedrawAndPresentFrame(runtime, 0);
    game::NovaMenu_RunAboutDialog(runtime.platform, runtime.game, [&runtime] {
      NovaRender_RedrawAndPresentFrame(runtime, 0);
    });
    break;
  }
  case GameModeAction::acknowledgements: {
    // Not an original action code: the 'x' branch of NovaCommand_DispatchToMode
    // (0x004872a0) runs the shared text reader on d\x91sc 0x7ffe inline. The
    // port routes it through requested_action for symmetry with the menu.
    NovaRender_RedrawAndPresentFrame(runtime, 0);
    game::NovaMenu_RunAcknowledgementsDialog(
        runtime.platform, runtime.game, [&runtime] {
          NovaRender_RedrawAndPresentFrame(runtime, 0);
        });
    break;
  }
  }
}

// Ghidra: 0x004872a0 NovaCommand_DispatchToMode. Maps polled main-menu command
// tokens to model actions. (The old 0x004d6260 NovaCommand_TranslateByInputMap
// label was a misnomer: that function is the MetroWerks C-locale toupper,
// MWRuntime_ToUpper, which the main loop uses only to fold command events.)
// @port 0x004872a0 100%
std::optional<GameModeAction> NovaCommand_DispatchToMode(char command) {
  switch (command) {
  case 'n':
    return GameModeAction::new_game;
  case 'o':
    return GameModeAction::open_pilot;
  case 'e':
    return GameModeAction::enter_spaceflight;
  case 'p':
    return GameModeAction::preferences;
  case 'a':
    return GameModeAction::about_nova;
  // 0x004872a0 branch: d\x91sc 0x7ffe (ACKNOWLEDGEMENTS) through the
  // travel-selection reader.
  case 'x':
    return GameModeAction::acknowledgements;
  case 'q':
    return GameModeAction::quit;
  default:
    return std::nullopt;
  }
}

// Ghidra: 0x004861b0 NovaHud_TrackFocusHoverIndex
// @port 0x004861b0 100%
std::optional<GameModeAction>
NovaHud_TrackFocusHoverIndex(const NovaRuntime &runtime) {
  const auto mouse_position = runtime.platform.mouse_position();
  for (std::size_t index = 0; index < kMenuEntries.size(); ++index) {
    if (!MenuRowRevealed(runtime, index % 3)) {
      continue;
    }
    if (runtime.main_menu_sprite_assets[index]
            ? MenuSpriteContainsOpaquePixel(runtime, index, mouse_position)
            : ContainsInclusive(MenuRect(runtime, index), mouse_position)) {
      return kMenuEntries[index].action;
    }
  }
  return std::nullopt;
}
