#include "sprite_world.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../rle_sprite_sheet.hpp"
#include "../sdl_platform.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace game {
namespace {

// Shared tile-upload: decodes the rl\x91D `sheet` and uploads every frame as a
// texture, filling `out`. All frames of a sheet share its pixel dimensions.
bool UploadSheetTextures(SDL_Renderer *renderer,
                         const RleSpriteSheet &sheet,
                         SpriteAsset &out) {
  out.frame_count = static_cast<int>(sheet.frames.size());
  out.tile_width = sheet.width;
  out.tile_height = sheet.height;
  out.frames.reserve(sheet.frames.size());
  for (const auto &frame : sheet.frames) {
    auto texture = SdlTexture::Create(
        renderer, sheet.width, sheet.height, frame.rgba_pixels);
    if (!texture) {
      out.frames.clear();
      return false;
    }
    out.frames.push_back(std::move(texture));
  }
  return !out.frames.empty();
}

} // namespace

std::unique_ptr<SpriteAsset> SpriteAsset::LoadSpin(SDL_Renderer *renderer,
                                                   std::uint16_t spin_id) {
  // sp\x9an descriptor -> named rl\x91D sheet of square tiles.
  const auto spin_data = NovaResource_Load(kResourceTypeSprites, spin_id);
  if (!spin_data) {
    NovaLog::Warn("spin sprite: no sp.x9an descriptor resource {}", spin_id);
    return nullptr;
  }
  const auto def = NovaSpriteDefinition_Parse(*spin_data);
  if (!def || def->tile_width <= 0 || def->tile_height <= 0) {
    NovaLog::Warn("spin sprite: malformed sp.x9an descriptor {}", spin_id);
    return nullptr;
  }
  const auto sheet_data =
      NovaResource_Load(kResourceTypeRleSheet16, def->sprites_resource_id);
  if (!sheet_data) {
    NovaLog::Warn("spin sprite {}: no rl.x91D sheet {}",
                  spin_id,
                  def->sprites_resource_id);
    return nullptr;
  }
  auto sheet = RleSpriteSheet_Decode16(*sheet_data);
  if (!sheet || sheet->width != def->tile_width ||
      sheet->height != def->tile_height) {
    NovaLog::Warn("spin sprite {}: rl.x91D sheet {} not a valid tile grid",
                  spin_id,
                  def->sprites_resource_id);
    return nullptr;
  }
  auto asset = std::make_unique<SpriteAsset>();
  if (!UploadSheetTextures(renderer, *sheet, *asset)) {
    NovaLog::Warn("spin sprite {}: texture upload failed", spin_id);
    return nullptr;
  }
  return asset;
}

std::unique_ptr<SpriteAsset> SpriteAsset::LoadSheet(SDL_Renderer *renderer,
                                                    std::uint16_t sheet_id) {
  const auto sheet_data = NovaResource_Load(kResourceTypeRleSheet16, sheet_id);
  if (!sheet_data) {
    NovaLog::Warn("sprite sheet: no rl.x91D sheet {}", sheet_id);
    return nullptr;
  }
  auto sheet = RleSpriteSheet_Decode16(*sheet_data);
  if (!sheet) {
    NovaLog::Warn("sprite sheet: malformed rl.x91D sheet {}", sheet_id);
    return nullptr;
  }
  auto asset = std::make_unique<SpriteAsset>();
  if (!UploadSheetTextures(renderer, *sheet, *asset)) {
    NovaLog::Warn("sprite sheet {}: texture upload failed", sheet_id);
    return nullptr;
  }
  return asset;
}

const SpriteAsset *SpriteStore::Spin(SDL_Renderer *renderer,
                                     std::uint16_t spin_id) {
  const auto index = static_cast<std::size_t>(spin_id);
  if (sets_.size() <= index) {
    sets_.resize(index + 1);
  }
  if (sets_[index]) {
    return sets_[index].get(); // cached (possibly a failed load -> null)
  }
  sets_[index] = SpriteAsset::LoadSpin(renderer, spin_id);
  return sets_[index].get();
}

void DrawSprite(SDL_Renderer *renderer,
                const SpriteAsset &asset,
                int frame,
                float world_x,
                float world_y,
                float camera_world_x,
                float camera_world_y,
                int viewport_w,
                int viewport_h,
                const SpriteDrawOptions &opts) {
  if (asset.frames.empty()) {
    return;
  }
  const int clamped = std::clamp(frame, 0, asset.frame_count - 1);
  const SdlTexture &texture = *asset.frames[static_cast<std::size_t>(clamped)];

  // Uniform camera transform: a world point lands at (world - camera) + half
  // the viewport for the ship-centred camera.
  float sx = (world_x - camera_world_x) + static_cast<float>(viewport_w) / 2.0F;
  float sy = (world_y - camera_world_y) + static_cast<float>(viewport_h) / 2.0F;
  if (opts.wrap) {
    // One-exit wraparound for the extending window: an off-edge sprite
    // reappears on the opposite edge (Ghidra Frame_UpdateViewportWrapBackground
    // Sprites relocates off-edge sprites across the viewport).
    sx = std::fmod(sx, static_cast<float>(viewport_w));
    sy = std::fmod(sy, static_cast<float>(viewport_h));
    if (sx < 0.0F) {
      sx += static_cast<float>(viewport_w);
    }
    if (sy < 0.0F) {
      sy += static_cast<float>(viewport_h);
    }
  }

  // Centre the frame (anchor at its middle) at native/requested scale, like
  // Sprite_SetPositionFromCurrentFrameAnchor placing the frame's anchor on the
  // world position.
  const float width = static_cast<float>(asset.tile_width) * opts.scale;
  const float height = static_cast<float>(asset.tile_height) * opts.scale;
  const SDL_FRect dest{sx - width / 2.0F, sy - height / 2.0F, width, height};

  if (opts.linear_scale) {
    // Smooth filtering for scaled tiles (e.g. tiny star sprites upscaled).
    SDL_SetTextureScaleMode(texture.get(), SDL_SCALEMODE_LINEAR);
  }

  if (opts.alpha_mod < 1.0F) {
    // Blend mode gate mirrors the glow layer: dim the texture by the requested
    // intensity, restore opaque after (kept per-draw so shared textures are not
    // left alpha-modded). Ghidra: the sprite effect color / alpha blending the
    // original applies to the glow layer.
    const std::uint8_t alpha = static_cast<std::uint8_t>(
        std::clamp(opts.alpha_mod, 0.0F, 1.0F) * 255.0F);
    SDL_SetTextureAlphaMod(texture.get(), alpha);
    SDL_RenderTexture(renderer, texture.get(), nullptr, &dest);
    SDL_SetTextureAlphaMod(texture.get(), SDL_ALPHA_OPAQUE);
  } else {
    SDL_RenderTexture(renderer, texture.get(), nullptr, &dest);
  }
}

} // namespace game
