#include "nova_app.hpp"

#include "brgr_archive.hpp"
#include "game/new_pilot_flow.hpp"
#include "game/spaceflight.hpp"
#include "log.hpp"
#include "pict_image.hpp"
#include "rle_sprite_sheet.hpp"

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
    MenuEntry{GameModeAction::quit, "QUIT        [Q]"},
    MenuEntry{GameModeAction::enter_spaceflight, "ENTER SPACE"},
    MenuEntry{GameModeAction::preferences, "PREFERENCES [P]"},
    MenuEntry{GameModeAction::starmap, "STAR MAP    [A]"},
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
// Keep authored menu animation independent of the host display's vsync. The
// original presentation was effectively paced at roughly 60 Hz.
constexpr std::uint64_t kMenuAnimationFrameDurationMs = 16;

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

void InitializeMenuEntrance(NovaRuntime &runtime, std::uint64_t now_ms) {
  runtime.menu_entrance_initialized = true;
  runtime.menu_top_animation_frame = 0;
  runtime.next_menu_top_animation_ms = now_ms + kMenuAnimationFrameDurationMs;
  runtime.next_menu_reveal_frame_ms = now_ms;
  runtime.menu_center_preview_frame = 6;
  runtime.menu_center_preview_intensity = 0;

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
    const auto choice_count = runtime.main_menu_logo_textures.size() - 1;
    const auto advance = 1 + static_cast<std::size_t>(now_ms % choice_count);
    runtime.menu_top_animation_frame =
        (runtime.menu_top_animation_frame + advance) %
        runtime.main_menu_logo_textures.size();
    runtime.next_menu_top_animation_ms = now_ms + kMenuAnimationFrameDurationMs;
  }

  if (now_ms >= runtime.next_menu_reveal_frame_ms) {
    runtime.next_menu_reveal_frame_ms = now_ms + kMenuAnimationFrameDurationMs;
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

void UpdateMenuCenterPreview(NovaRuntime &runtime) {
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
  if (runtime.menu_center_preview_frame == desired_frame) {
    runtime.menu_center_preview_intensity = static_cast<std::uint8_t>(
        std::min<int>(32, runtime.menu_center_preview_intensity + 4));
  } else if (runtime.menu_center_preview_intensity > 0) {
    runtime.menu_center_preview_intensity = static_cast<std::uint8_t>(
        std::max<int>(0, runtime.menu_center_preview_intensity - 4));
  } else {
    runtime.menu_center_preview_frame = desired_frame;
  }
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
  // Idle main menu shows only the pulsing "SELECT A COMMAND" prompt; there is
  // no placeholder status line in the original (Ghidra 0x004873b0). status_text
  // is left empty until a menu action reports something.
  runtime.status_text.reset();
  runtime.startup_phase = StartupPhase::loading_splash;
  runtime.startup_phase_started_ms = runtime.platform.ticks_ms();
  runtime.next_menu_prompt_toggle_ms = runtime.startup_phase_started_ms + 650;
  // Ghidra: FUN_004ad960, which loads sp\x95n 600-605 into DAT_00596cb8.
  for (std::size_t index = 0;
       index < runtime.main_menu_sprite_definitions.size();
       ++index) {
    const auto sprite_id = static_cast<std::uint16_t>(600 + index);
    runtime.main_menu_sprite_definitions[index] =
        NovaResource_LoadMainMenuSpriteDefinition(sprite_id);
    if (const auto &definition = runtime.main_menu_sprite_definitions[index]) {
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
    } else {
      NovaLog::Todo("main-menu backdrop PICT 0x1f40 failed to decode");
    }
  }
  if (const auto definition = NovaResource_LoadMainMenuSpriteDefinition(606)) {
    runtime.main_menu_logo_textures =
        LoadPictSpriteFrames(runtime.platform.renderer(), *definition);
    if (runtime.main_menu_logo_textures.empty()) {
      NovaLog::Todo("main-menu top animation sp\x95n 606 failed to load");
    }
  }
  if (const auto definition = NovaResource_LoadMainMenuSpriteDefinition(607)) {
    runtime.main_menu_center_preview_asset =
        LoadMenuSpriteAsset(runtime.platform.renderer(), *definition);
    if (!runtime.main_menu_center_preview_asset) {
      NovaLog::Todo("main-menu center preview sp\x95n 607 failed to load");
    }
  }
  for (std::size_t row = 0; row < runtime.main_menu_row_reveal_textures.size();
       ++row) {
    const auto sprite_id = static_cast<std::uint16_t>(608 + row);
    if (const auto definition =
            NovaResource_LoadMainMenuSpriteDefinition(sprite_id)) {
      runtime.main_menu_row_reveal_textures[row] =
          LoadPictSpriteFrames(runtime.platform.renderer(), *definition);
      if (runtime.main_menu_row_reveal_textures[row].empty()) {
        NovaLog::Todo("main-menu row reveal sp\x95n {} failed to load",
                      sprite_id);
      }
    }
  }
  // Audio: open the SDL output device and preload the main-menu feedback
  // effects. 600/601 mark focus entry/exit. Frame_TickTimerDecayAndEffects
  // queues 602 when each row reveal starts and 603 when it completes.
  const auto load_menu_sound = [](std::uint16_t sound_id) {
    if (const auto resource = NovaResource_LoadSndData(sound_id)) {
      if (auto decoded = NovaSound_Decode(*resource)) {
        return decoded;
      }
      NovaLog::Todo("menu snd \x20resource {} could not be decoded", sound_id);
    } else {
      NovaLog::Todo("menu snd \x20resource {} could not be located", sound_id);
    }
    return std::optional<NovaSoundData>{};
  };
  runtime.menu_focus_enter_sound = load_menu_sound(600);
  runtime.menu_focus_exit_sound = load_menu_sound(601);
  runtime.menu_reveal_start_sound = load_menu_sound(602);
  runtime.menu_reveal_finish_sound = load_menu_sound(603);
  if (!runtime.audio.Initialize()) {
    NovaLog::Warn("continuing without audio (menu sounds are silent)");
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
  // which SDL reports relative to the current renderer viewport. When the
  // player returns to the menu the previous (flight) context left the full-
  // window viewport active, so re-assert the centred playfield here, before
  // any hit-testing, to keep mouse coordinates in 640x480 playfield space.
  runtime.platform.SetCenteredPlayfield();

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
    runtime.startup_phase_started_ms = now_ms;
    runtime.next_menu_prompt_toggle_ms = now_ms + 650;
    InitializeMenuEntrance(runtime, now_ms);
  }

  // The menu bass begins with the second (Ambrosia) splash and is deliberately
  // not restarted at main-menu entry, so playback carries across the boundary.
  if (runtime.startup_phase != StartupPhase::loading_splash &&
      !runtime.menu_music_started) {
    runtime.menu_music_started = true;
    runtime.music.Play();
  }

  if (runtime.startup_phase == StartupPhase::main_menu) {
    UpdateMenuEntrance(runtime, now_ms);
    runtime.hovered_action = NovaHud_TrackFocusHoverIndex(runtime);
    UpdateMenuCenterPreview(runtime);
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

  if (runtime.startup_phase == StartupPhase::main_menu &&
      now_ms >= runtime.next_menu_prompt_toggle_ms) {
    runtime.menu_prompt_visible = !runtime.menu_prompt_visible;
    runtime.next_menu_prompt_toggle_ms = now_ms + 650;
  }
}

// Ghidra: 0x004873b0 NovaRender_RedrawAndPresentFrame
void NovaRender_RedrawAndPresentFrame(NovaRuntime &runtime, short mode) {
  SDL_Renderer *const renderer = runtime.platform.renderer();
  // Fixed screens (menu / splash / docked) render a centred 640x480 playfield:
  // clip draw calls to it (SDL_RenderClear still fills the whole window first
  // with the border colour because the clear ignores the clip rect).
  runtime.platform.SetCenteredPlayfield();
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
    SDL_SetTextureAlphaMod(asset.textures[frame]->get(), alpha);
    SDL_RenderTexture(
        renderer, asset.textures[frame]->get(), nullptr, &destination);
    SDL_SetTextureAlphaMod(asset.textures[frame]->get(), SDL_ALPHA_OPAQUE);
  }

  // Only the pulsing "SELECT A COMMAND" prompt is drawn by the original idle
  // main menu (Ghidra 0x004873b0, string key 0x7d2/0x114). status_text is shown
  // underneath only when a menu action has produced feedback.
  if (MenuEntranceComplete(runtime) && runtime.status_text) {
    SDL_SetRenderDrawColor(renderer, 159, 190, 227, SDL_ALPHA_OPAQUE);
    DrawDebugTextCentered(renderer, 320.0F, 370.0F, *runtime.status_text);
  }
  if (MenuEntranceComplete(runtime) && runtime.menu_prompt_visible) {
    DrawDebugTextCentered(renderer, 320.0F, 394.0F, "SELECT A COMMAND");
  }

  if (mode == 1) {
    SDL_RenderPresent(renderer);
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

// Ghidra: 0x00486ed0 NovaGameMode_DispatchAction
void NovaGameMode_DispatchAction(NovaRuntime &runtime, GameModeAction action) {
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
    if (game::NovaNewPilotFlow_Run(runtime.platform, runtime.game)) {
      runtime.status_text = "New pilot created. Choose ENTER SPACE to fly.";
    } else {
      runtime.status_text = "New game cancelled.";
    }
    break;
  }
  case GameModeAction::open_pilot:
    NovaLog::Todo("Open Pilot file dialog is not reconstructed.");
    runtime.status_text = "Open Pilot flow is not reconstructed yet.";
    break;
  case GameModeAction::quit:
    runtime.quit_requested = true;
    break;
  case GameModeAction::enter_spaceflight: {
    // Ghidra: param_1 == 3. Run spaceflight only when there is an active,
    // living pilot; otherwise show the error blip (an effect the placeholder
    // build cannot show, so a status line is used instead).
    const auto player_alive = runtime.game.player.is_active &&
                              runtime.game.player.death_timer_active < 0.0F;
    if (!runtime.game.game_active || !player_alive) {
      runtime.status_text = "Spaceflight requires an active pilot.";
      break;
    }
    // Blocking: plays the intro cinematic on first entry, then the in-game
    // main loop, returning to the menu when the pilot exits. Mirrors
    // Ship_RunSpaceflightMode being called inline from the dispatcher.
    game::NovaSpaceflight_Run(runtime.platform, runtime.audio, runtime.game);
    runtime.status_text = "Returned from spaceflight.";
    break;
  }
  case GameModeAction::preferences:
    NovaLog::Todo("Preferences dialog is not reconstructed.");
    runtime.status_text = "Preferences dialog is not reconstructed yet.";
    break;
  case GameModeAction::starmap:
    NovaLog::Todo("Star Map dialog requires system/route tables.");
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
