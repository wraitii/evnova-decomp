#include "nova_app.hpp"

#include "brgr_archive.hpp"
#include "game/about_dialog.hpp"
#include "game/hud_overlay.hpp"
#include "game/new_pilot_flow.hpp"
#include "game/nova_font.hpp"
#include "game/probe_state.hpp"
#include "game/ship_ai.hpp"
#include "game/spaceflight.hpp"
#include "game/targeting.hpp"
#include "log.hpp"
#include "pict_image.hpp"
#include "rle_sprite_sheet.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace {

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

constexpr float kMenuCoordinateScale = 640.0F / 1024.0F;
constexpr float kFallbackMenuLeft = 52.0F;
constexpr float kFallbackMenuTop = 238.0F;
constexpr float kFallbackMenuWidth = 220.0F;
constexpr float kFallbackMenuHeight = 24.0F;
constexpr float kFallbackMenuGap = 8.0F;
constexpr float kFallbackLogoOriginX = 191.0F;
constexpr float kFallbackLogoOriginY = 162.0F;
constexpr NovaMenuPoint kFallbackCenterPreviewOrigin{.x = 444, .y = 465};
constexpr std::array<NovaMenuPoint, 3> kFallbackRowRevealOrigins{
    NovaMenuPoint{.x = 343, .y = 399},
    NovaMenuPoint{.x = 337, .y = 462},
    NovaMenuPoint{.x = 337, .y = 526},
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
// bar outline comes from c\x9alr in 1024x768 reference coordinates relative to
// the window center; DAT_00575a58 = 198.0 is the fill span in those pixels.
constexpr int kProgressBarCenterX = 1024 / 2;
constexpr int kProgressBarCenterY = 768 / 2;
constexpr double kProgressBarFillSpan = 198.0;
// Number of staged startup asset loads that drive the bar. The original's
// denominator is the 'ship' resource count (NovaData_LoadAllShipClass
// VisualAndLaunchData 0x004aeda0); the port's startup loads a different set, so
// the total here counts the steps below (see NovaGameSession_Run).
constexpr std::uint8_t kStartupLoadStepCount = 6;

// The original draws menu text with g_main_menu_font_id 3 (Geneva) size 9 in
// the 1024x768 backdrop space (Ghidra 0x004b32aa NovaData_LoadScenarioResource
// Tables + 0x004874d5 NovaRender_RedrawAndPresentFrame); the port draws in the
// 640x480 logical playfield, so the size scales with the 0.625 art factor.
constexpr float kMenuFontLogicalSize = 9.0F * kMenuCoordinateScale;
// Menu label/value colours. Ghidra 0x004b3262/0x004b327d seed DAT_0073564c
// (values, RGB555 triplet 0xffff,0,0) and DAT_00735652 (labels, 0x84d0,0,0);
// the text engine scales each 5-bit component by 8 (FUN_004bc760).
constexpr SDL_Color kMenuLabelColor{128, 0, 0, SDL_ALPHA_OPAQUE};
constexpr SDL_Color kMenuValueColor{248, 0, 0, SDL_ALPHA_OPAQUE};

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
// "enter_ship", ...). MenuRect is in the 640x480 logical canvas while
// /probe/click consumes window points, so map through the scaled
// presentation's on-screen rect. Only revealed rows are published, matching
// the hover hit-test's gating. No effect on game behaviour.
void PublishMainMenuProbeUi(NovaRuntime &runtime) {
  static constexpr std::array<std::string_view, kMenuEntries.size()> kNames{
      "new_pilot",
      "open_pilot",
      "quit_nova",
      "enter_ship",
      "set_prefs",
      "about_nova"};
  const SDL_FRect playfield = runtime.platform.playfield_window_rect();
  const float sx = playfield.w / 640.0F;
  const float sy = playfield.h / 480.0F;
  std::vector<std::pair<std::string, SDL_FRect>> named;
  for (std::size_t index = 0; index < kMenuEntries.size(); ++index) {
    if (!MenuRowRevealed(runtime, index % 3)) {
      continue;
    }
    const SDL_FRect rect = MenuRect(runtime, index);
    named.emplace_back(std::string(kNames[index]),
                       SDL_FRect{playfield.x + rect.x * sx,
                                 playfield.y + rect.y * sy,
                                 rect.w * sx,
                                 rect.h * sy});
  }
  runtime.platform.PublishProbeUi("main_menu", std::move(named));
}

[[nodiscard]] bool MenuSpriteContainsOpaquePixel(const NovaRuntime &runtime,
                                                 std::size_t index,
                                                 SDL_FPoint point) {
  const auto rect = MenuRect(runtime, index);
  if (!Contains(rect, point) ||
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

void DrawDebugTextCentered(SDL_Renderer *renderer,
                           float center_x,
                           float y,
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
    SDL_FRect strip{
        0.0F, static_cast<float>(band), static_cast<float>(width), 4.0F};
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
    SDL_SetRenderDrawColor(renderer,
                           7,
                           shade,
                           static_cast<std::uint8_t>(shade + 34),
                           SDL_ALPHA_OPAQUE);
    SDL_RenderLine(renderer,
                   center_x - half_width,
                   center_y + static_cast<float>(y),
                   center_x + half_width,
                   center_y + static_cast<float>(y));
  }

  SDL_SetRenderDrawColor(renderer, 86, 171, 215, SDL_ALPHA_OPAQUE);
  SDL_RenderLine(renderer,
                 center_x - 150.0F,
                 center_y - 82.0F,
                 center_x + 20.0F,
                 center_y - 120.0F);
  SDL_RenderLine(renderer,
                 center_x - 170.0F,
                 center_y - 20.0F,
                 center_x + 84.0F,
                 center_y - 56.0F);
  SDL_RenderLine(renderer,
                 center_x - 166.0F,
                 center_y + 66.0F,
                 center_x + 110.0F,
                 center_y + 34.0F);
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

void DrawSplashFrame(SDL_Renderer *renderer,
                     std::string_view heading,
                     std::string_view detail,
                     float progress) {
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
                              width * scale,
                              height * scale};
  SDL_RenderTexture(renderer, texture, nullptr, &destination);
}

// Reference-space (1024x768) progress-bar rectangle in native QuickDraw field
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
  return ProgressBarReferenceRect{kProgressBarCenterY + top,
                                  kProgressBarCenterX + left,
                                  kProgressBarCenterY + bottom,
                                  kProgressBarCenterX + right};
}

struct ProgressBarPalette {
  SDL_Color fill;
  SDL_Color inner;
  SDL_Color outer;
};

[[nodiscard]] SDL_Color ToSdlColor(const NovaRgbColor &color) {
  return SDL_Color{color.red, color.green, color.blue, SDL_ALPHA_OPAQUE};
}

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
  return SDL_FRect{
      static_cast<float>(rect.left) * kMenuCoordinateScale,
      static_cast<float>(rect.top) * kMenuCoordinateScale,
      static_cast<float>(rect.right - rect.left) * kMenuCoordinateScale,
      static_cast<float>(rect.bottom - rect.top) * kMenuCoordinateScale};
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
    // Ghidra NovaHud_UpdateLayoutState: each subsequent strip starts half the
    // preceding strip's frame count behind the prior counter (0.5 multiplier
    // at DAT_00575938).
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
        runtime.audio.Play(*runtime.menu_reveal_start_sound);
      }
      if (runtime.menu_row_reveal_counters[row] == frame_count &&
          runtime.menu_reveal_finish_sound) {
        runtime.audio.Play(*runtime.menu_reveal_finish_sound);
      }
    }
  }
}

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

// Ghidra 0x00469030 NovaUi_DrawCombatRankLabel. Maps the raw combat-rating
// points to a rank index (thresholds 100/200/400/800/1600/3200/6400/12800/
// 25600) and returns the STR# 0x8a label (1-based entry rank+1).
[[nodiscard]] std::string MenuCombatRankLabel(const game::GameState &state) {
  std::int32_t rank = state.player_combat_rating_points > 0 ? 1 : 0;
  constexpr std::array<std::int32_t, 9> kThresholds{
      99, 199, 399, 799, 1599, 3199, 6399, 12799, 25599};
  for (std::size_t i = 0; i < kThresholds.size(); ++i) {
    if (state.player_combat_rating_points > kThresholds[i]) {
      rank = static_cast<std::int32_t>(i) + 2;
    }
  }
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
  const auto *system = state.scenario.System(system_id);
  if (system == nullptr) {
    return {};
  }
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

  int tolerance = 0;
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
    if (stellar->hazard_marker) {
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

// Ghidra 0x00468450 NovaText_FormatDateString. Formats
// "<Mon.> <day><st/nd/rd/th>, <year>" from STR# 0x89 (abbreviated month names
// at entry month+12; ordinal suffixes at 0x19-0x1c with the 11-13 -> th rule).
[[nodiscard]] std::string FormatGameDateString(int year, int month, int day) {
  std::string text;
  if (auto name = game::NovaHud_LoadStringEntry(
          0x89, static_cast<std::uint16_t>(month + 0xc))) {
    text += *name;
  }
  text += " ";
  text += std::to_string(day);
  const char *suffix = "th";
  switch (day % 10) {
  case 1:
    suffix = "st";
    break;
  case 2:
    suffix = "nd";
    break;
  case 3:
    suffix = "rd";
    break;
  default:
    break;
  }
  if (day > 10 && day < 14) {
    suffix = "th";
  }
  text += suffix;
  text += ", ";
  text += std::to_string(year);
  return text;
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
// columns of Geneva status text below the buttons plus the clone-source ship
// portrait centered between them. All offsets below are 1024x768 backdrop
// space relative to the backdrop-frame centre, scaled by 0.625 like the art.
void DrawMenuStatusPanel(NovaRuntime &runtime) {
  SDL_Renderer *const renderer = runtime.platform.renderer();
  auto &font_cache = runtime.font_cache;
  const auto &game = runtime.game;
  constexpr float kOriginX = 320.0F;
  constexpr float kOriginY = 240.0F;
  constexpr float kLeftLabelX = kOriginX - 190.0F * kMenuCoordinateScale;
  constexpr float kLeftValueX = kOriginX - 185.0F * kMenuCoordinateScale;
  constexpr float kRightLabelX = kOriginX + 120.0F * kMenuCoordinateScale;
  constexpr float kRightValueX = kOriginX + 125.0F * kMenuCoordinateScale;
  const auto baseline = [&](int offset_1024) {
    return kOriginY + static_cast<float>(offset_1024) * kMenuCoordinateScale;
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
                         kOriginX - 150.0F * kMenuCoordinateScale,
                         kOriginX + 150.0F * kMenuCoordinateScale,
                         baseline(0x136),
                         line,
                         kMenuLabelColor);
    return;
  }

  std::string pilot_name = game.pilot.first_name;
  if (!game.pilot.last_name.empty()) {
    if (!pilot_name.empty()) {
      pilot_name += " ";
    }
    pilot_name += game.pilot.last_name;
  }
  const auto *ship_class = game.scenario.Ship(
      static_cast<std::int16_t>(game.player.ship_class_id + 0x80));
  // TODO(decomp) skipped: the live game calendar (g_current_game_year_month/
  // day, Ghidra 0x00468450 callers) is not tracked by GameState yet; draw the
  // original fresh-pilot baseline date.
  const std::string date_text = FormatGameDateString(1999, 1, 1);

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

  // Ship portrait (Ghidra: DAT_00596d44[clone_source] blit, 128x64 class
  // portrait centered on the panel origin, top at origin + 0x118).
  const std::int16_t class_id = game.player.ship_class_id;
  if (runtime.menu_status_portrait_class != class_id) {
    runtime.menu_status_portrait.reset();
    runtime.menu_status_portrait_class = class_id;
    std::int16_t pict_class = class_id;
    if (ship_class != nullptr && ship_class->clone_source_ship_class >= 0) {
      pict_class = ship_class->clone_source_ship_class;
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
    const SDL_FRect destination{kOriginX - width * kMenuCoordinateScale / 2.0F,
                                baseline(0x118),
                                width * kMenuCoordinateScale,
                                height * kMenuCoordinateScale};
    SDL_RenderTexture(
        renderer, runtime.menu_status_portrait->get(), nullptr, &destination);
  }
}

// Rebuilds the OR-composited rollover preview texture when the displayed
// frame changes. Ghidra 0x0048c580 NovaHud_RenderFocusOverlay: at rest the
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

// Ghidra: 0x00503f30 NovaProgramEntry
int NovaProgramEntry() {
  NovaRuntime runtime;
  // Ghidra: NovaPrefs_ResetToDefaults (0x004b4320) seeds the preference globals
  // before any dialog reads them; the key-settings .prf load-overrides them
  // when available (NovaPrefs_LoadOrInit) and is deferred.
  runtime.prefs.ResetToDefaults();
  return NovaApp_Run(runtime);
}

// Ghidra: 0x004d2a80 NovaApp_Run
int NovaApp_Run(NovaRuntime &runtime) {
  // Platform_RegisterMainWindow / QuickTime_Initialize are replaced by SDL
  // setup.
  if (!runtime.platform.Initialize()) {
    return 1;
  }
  if (!game::NovaPrefs_LoadFromSystemStore(runtime.prefs)) {
    NovaLog::Info("preferences: using original defaults");
  }
  // The original normalizes the on-disk block once during startup too.
  (void)game::NovaPrefs_SaveToSystemStore(runtime.prefs);
  // External probe harness (docs/probe_harness.md): the state reader runs on
  // the main thread at the pump, so it can safely walk the live GameState.
  runtime.platform.probe().SetStateProvider(
      [&runtime](const std::string &query) {
        return ProbeState_Snapshot(runtime.game, query);
      });
  NovaGameSession_Run(runtime);
  return 0;
}

// Ghidra: 0x00416100 NovaGameSession_Run
void NovaGameSession_Run(NovaRuntime &runtime) {
  // Resource and QuickTime startup are not reconstructed yet. Licence checks
  // are deliberately skipped.
  runtime.game_active = false;
  // Idle main menu shows only the "No Pilot File Loaded" prompt (or, with a
  // pilot loaded, the status panel); the original draws no other status line
  // (Ghidra 0x004873b0).
  runtime.startup_phase = StartupPhase::loading_splash;
  runtime.startup_phase_started_ms = runtime.platform.ticks_ms();
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
        static_cast<float>(runtime.prefs.sound_volume) / 8.0F);
  }

  // Background music. The shipped bass track is the MP3 in the Nova Files
  // folder (the original streams a :Music:SongNN path through a codec;
  // SDL3_mixer decodes the MP3 for us). The music device/format is set up here,
  // matching NovaAudio_Initialize(8,0) running before the splash frames in the
  // original; Play() is deferred until the second splash becomes active, then
  // the same stream carries through into the main menu.
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
void NovaMainLoop_Run(NovaRuntime &runtime) {
  while (!runtime.quit_requested && !runtime.platform.quit_requested()) {
    NovaMainLoop_UpdateFrame(runtime);
    NovaRender_RedrawAndPresentFrame(runtime, 1);
  }
}

// Ghidra: 0x00488080 NovaMainLoop_UpdateFrame
void NovaMainLoop_UpdateFrame(NovaRuntime &runtime) {
  // The menu/splash hover tracking below reads platform.mouse_position(),
  // which SDL reports in the logical 640x480 content coordinates because the
  // menu uses the upscaled presentation. When the player returns to the menu
  // the previous (flight) context left the full-window viewport active, so
  // re-assert the scaled playfield here, before any hit-testing, so mouse
  // coordinates stay in 640x480 content space.
  runtime.platform.SetScaledPlayfield();

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
      runtime.audio.Play(*runtime.menu_focus_enter_sound);
    } else if (!runtime.hovered_action && runtime.previous_hovered_action &&
               runtime.menu_focus_exit_sound) {
      runtime.audio.Play(*runtime.menu_focus_exit_sound);
    }
  }
  runtime.previous_hovered_action = runtime.hovered_action;

  if (runtime.requested_action) {
    // NovaAudio_PlayTransitionEffectsWait plays focus-in then focus-out around
    // keyboard actions. Dispatch remains asynchronous in this SDL loop.
    if (runtime.menu_focus_enter_sound) {
      runtime.audio.Play(*runtime.menu_focus_enter_sound);
    }
    NovaGameMode_DispatchAction(runtime, *runtime.requested_action);
    runtime.requested_action.reset();
  }
}

// Ghidra: 0x004873b0 NovaRender_RedrawAndPresentFrame
void NovaRender_RedrawAndPresentFrame(NovaRuntime &runtime, short mode) {
  SDL_Renderer *const renderer = runtime.platform.renderer();
  // Fixed pre-render screens (menu / splash / intro) are uniformly upscaled to
  // fill the window via the scaled 640x480 logical presentation, so the 1024-
  // native art reads at ~1:1 at the 1024x768 minimum.
  runtime.platform.SetScaledPlayfield();
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

  if (runtime.main_menu_backdrop_texture) {
    // Real title-screen backdrop: the 1024x768 ship-interior PICT 0x1f40,
    // uniformly scaled to the 640x480 viewport (same 4:3 aspect, fills it).
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);
    PresentSplashTexture(renderer, runtime.main_menu_backdrop_texture->get());
  } else {
    // Fallback when the backdrop PICT cannot be decoded: procedural
    // starfield + planet + HUD chrome stand in for the ship-interior scene.
    DrawMenuBackground(renderer, 640, 480);
    DrawPlanet(renderer);
    DrawHudFrame(renderer);
  }

  SDL_SetRenderDrawColor(renderer, 202, 224, 255, SDL_ALPHA_OPAQUE);

  // Top background animation (sp\x95n 606, PICT 0x1f4a), drawn at c\x9alr
  // +0xe0. Frame_TickTimerDecayAndEffects selects a different random frame on
  // its short animation timer; UpdateMenuEntrance maintains the equivalent
  // no-repeat progression.
  if (!runtime.main_menu_logo_textures.empty()) {
    const float logo_scale = kMenuCoordinateScale;
    const auto frame = std::min(runtime.menu_top_animation_frame,
                                runtime.main_menu_logo_textures.size() - 1);
    const auto &logo_texture = runtime.main_menu_logo_textures[frame];
    float logo_width = 0.0F;
    float logo_height = 0.0F;
    SDL_GetTextureSize(logo_texture->get(), &logo_width, &logo_height);
    const float logo_origin_x = runtime.main_menu_style
                                    ? runtime.main_menu_style->logo_origin.x
                                    : kFallbackLogoOriginX;
    const float logo_origin_y = runtime.main_menu_style
                                    ? runtime.main_menu_style->logo_origin.y
                                    : kFallbackLogoOriginY;
    const SDL_FRect logo_destination{
        logo_origin_x * logo_scale,
        logo_origin_y * logo_scale,
        logo_width * logo_scale,
        logo_height * logo_scale,
    };
    SDL_RenderTexture(
        renderer, logo_texture->get(), nullptr, &logo_destination);
  } else {
    DrawDebugTextCentered(
        renderer, 320.0F, 78.0F, "E S C A P E   V E L O C I T Y");
    DrawDebugTextCentered(renderer, 320.0F, 102.0F, "N O V A");
  }

  // The original entrance does not interpolate the button sprites. It plays
  // authored PICT strips 608-610 behind each pair, then replaces a completed
  // strip with the two normal focus sprites for that row.
  for (std::size_t row = 0; row < runtime.main_menu_row_reveal_textures.size();
       ++row) {
    const auto &textures = runtime.main_menu_row_reveal_textures[row];
    const auto counter = runtime.menu_row_reveal_counters[row];
    if (counter < 0 || counter >= static_cast<int>(textures.size()) ||
        textures.empty()) {
      continue;
    }
    const auto origin = runtime.main_menu_style
                            ? runtime.main_menu_style->row_reveal_origins[row]
                            : kFallbackRowRevealOrigins[row];
    float width = 0.0F;
    float height = 0.0F;
    SDL_GetTextureSize(
        textures[static_cast<std::size_t>(counter)]->get(), &width, &height);
    const SDL_FRect destination{
        static_cast<float>(origin.x) * kMenuCoordinateScale,
        static_cast<float>(origin.y) * kMenuCoordinateScale,
        width * kMenuCoordinateScale,
        height * kMenuCoordinateScale,
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

  for (std::size_t index = 0; index < kMenuEntries.size(); ++index) {
    if (!MenuRowRevealed(runtime, index % 3)) {
      continue;
    }
    const auto rect = MenuRect(runtime, index);
    const bool hovered = runtime.hovered_action == kMenuEntries[index].action;
    if (runtime.main_menu_sprite_assets[index]) {
      continue;
    }
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
    SDL_SetRenderDrawColor(renderer,
                           menu_color.red,
                           menu_color.green,
                           menu_color.blue,
                           SDL_ALPHA_OPAQUE);
    SDL_RenderDebugText(renderer,
                        rect.x + 22.0F,
                        rect.y + 8.0F,
                        kMenuEntries[index].label.data());
  }

  // Center preview (sp\x95n 607): frames 0-5 correspond to the menu actions,
  // frame 6 is idle. The original fades the old frame out before switching.
  if (runtime.main_menu_center_preview_asset &&
      !runtime.main_menu_center_preview_asset->textures.empty() &&
      runtime.menu_center_preview_intensity > 0) {
    const auto &asset = *runtime.main_menu_center_preview_asset;
    const auto frame =
        std::min(runtime.menu_center_preview_frame, asset.textures.size() - 1);
    const auto origin = runtime.main_menu_style
                            ? runtime.main_menu_style->center_preview_origin
                            : kFallbackCenterPreviewOrigin;
    const SDL_FRect destination{
        static_cast<float>(origin.x) * kMenuCoordinateScale,
        static_cast<float>(origin.y) * kMenuCoordinateScale,
        static_cast<float>(asset.sheet.width) * kMenuCoordinateScale,
        static_cast<float>(asset.sheet.height) * kMenuCoordinateScale,
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
                             320.0F - 150.0F * kMenuCoordinateScale,
                             320.0F + 150.0F * kMenuCoordinateScale,
                             240.0F + 310.0F * kMenuCoordinateScale,
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

// Ghidra: 0x004ab070 NovaUi_PresentLoadingSplashFrame
void NovaUi_PresentLoadingSplashFrame(NovaRuntime &runtime) {
  if (runtime.loading_splash_texture) {
    SDL_SetRenderDrawColor(
        runtime.platform.renderer(), 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(runtime.platform.renderer());
    PresentSplashTexture(runtime.platform.renderer(),
                         runtime.loading_splash_texture->get());
    return;
  }
  const auto elapsed_ms =
      runtime.platform.ticks_ms() - runtime.startup_phase_started_ms;
  const auto progress = static_cast<float>(elapsed_ms) /
                        static_cast<float>(kLoadingSplashDurationMs);
  DrawSplashFrame(runtime.platform.renderer(),
                  "ESCAPE VELOCITY: NOVA",
                  "INITIALIZING NAVIGATION SYSTEMS",
                  progress);
}

// Ghidra: 0x004aaf60 NovaUi_PresentStartupSplashFrame
void NovaUi_PresentStartupSplashFrame(NovaRuntime &runtime) {
  if (runtime.startup_splash_texture) {
    SDL_SetRenderDrawColor(
        runtime.platform.renderer(), 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(runtime.platform.renderer());
    PresentSplashTexture(runtime.platform.renderer(),
                         runtime.startup_splash_texture->get());
    return;
  }
  DrawAmbrosiaStartupSplash(runtime.platform.renderer());
}

// Ghidra: 0x004ab3a0 NovaUi_ProgressCallbackNoOp. The startup path passes this
// as a progress sink where no redraw is wanted; kept as the faithful no-op.
void NovaUi_ProgressCallbackNoOp() {}

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

// Ghidra: 0x004ab3b0 NovaUi_AddProgressAndRedraw
void NovaUi_AddProgressAndRedraw(NovaRuntime &runtime, double delta) {
  runtime.loading_progress_value += delta;
  NovaUi_RedrawProgressBar(runtime);
}

// Ghidra: 0x004ab3d0 NovaUi_RedrawProgressBar
void NovaUi_RedrawProgressBar(NovaRuntime &runtime) {
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

// Ghidra: 0x00486ed0 NovaGameMode_DispatchAction
void NovaGameMode_DispatchAction(NovaRuntime &runtime, GameModeAction action) {
  // Probe-harness hygiene (docs/probe_harness.md): every mode change drops the
  // outgoing screen's published rects so /probe/ui never reports stale layout
  // (e.g. main-menu buttons while in flight). Each screen republishes on its
  // own frames; modals clear themselves via ProbeUiAutoClear.
  runtime.platform.ClearProbeUi();
  switch (action) {
  case GameModeAction::new_game: {
    // Ghidra: param_1 == 0. If a game is already active the original asks
    // before discarding it; confirmation dialogs for an active pilot are not
    // reconstructed yet, so the new-game flow below simply restarts.
    if (runtime.game.game_active) {
      NovaLog::Todo("confirm-discard before starting a new pilot with an "
                    "active game is not reconstructed; starting fresh");
    }
    // Runs the modal new-pilot flow (naming, confirm, reset, scenario load,).
    // On success the flow marks the game active and we return to the menu; the
    // player then chooses ENTER SPACE to play (intro cinematic plays then).
    // The dialogs keep re-rendering the menu behind themselves each frame.
    NovaRender_RedrawAndPresentFrame(runtime, 0);
    if (game::NovaNewPilotFlow_Run(runtime.platform, runtime.game, [&runtime] {
          NovaRender_RedrawAndPresentFrame(runtime, 0);
        })) {
      NovaLog::Info("new pilot created; choose ENTER SHIP to fly");
    } else {
      NovaLog::Info("new game cancelled");
    }
    break;
  }
  case GameModeAction::open_pilot:
    NovaLog::Todo("Open Pilot file dialog is not reconstructed.");
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
    const bool saved = game::NovaMenu_RunSettingsDialog(runtime.platform,
                                                        runtime.audio,
                                                        runtime.music,
                                                        font_cache,
                                                        runtime.prefs,
                                                        [&runtime] {
                                                          NovaRender_RedrawAndPresentFrame(
                                                              runtime, 0);
                                                        });
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
    game::NovaMenu_RunAboutDialog(
        runtime.platform, runtime.game, [&runtime] {
          NovaRender_RedrawAndPresentFrame(runtime, 0);
        });
    break;
  }
  }
}

// Ghidra: 0x004d6260 NovaCommand_TranslateByInputMap
std::optional<GameModeAction> NovaCommand_TranslateByInputMap(char command) {
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
    if (!MenuRowRevealed(runtime, index % 3)) {
      continue;
    }
    if (runtime.main_menu_sprite_assets[index]
            ? MenuSpriteContainsOpaquePixel(runtime, index, mouse_position)
            : Contains(MenuRect(runtime, index), mouse_position)) {
      return kMenuEntries[index].action;
    }
  }
  return std::nullopt;
}
