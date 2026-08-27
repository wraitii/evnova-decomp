#include "sprite_world.hpp"

#include "../brgr_archive.hpp"
#include "../cicn_image.hpp"
#include "../log.hpp"
#include "../rle_sprite_sheet.hpp"
#include "../sdl_platform.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>

namespace game {
namespace {

// Shared tile-upload: decodes the rl\x91D `sheet` and uploads every frame as a
// texture, filling `out`. All frames of a sheet share its pixel dimensions. For
// a sheet frame the anchor is defaulted to the frame's centre
// (SpriteFrame.anchor_x/y below), which is what SpriteFrame_CreateFromRect
// produces for a bare frame: the frame is drawn centred on its world position
// unless a caller overrides the anchor (see Sprite_DrawOptions.anchor_*).
bool UploadSheetTextures(SDL_Renderer *renderer,
                         const RleSpriteSheet &sheet,
                         SpriteAsset &out) {
  out.frame_count = static_cast<int>(sheet.frames.size());
  out.tile_width = sheet.width;
  out.tile_height = sheet.height;
  out.frames.reserve(sheet.frames.size());
  const float anchor_x = static_cast<float>(sheet.width) / 2.0F;
  const float anchor_y = static_cast<float>(sheet.height) / 2.0F;
  for (const auto &frame : sheet.frames) {
    auto texture = SdlTexture::Create(
        renderer, sheet.width, sheet.height, frame.rgba_pixels);
    if (!texture) {
      out.frames.clear();
      return false;
    }
    // Centre anchor (Ghidra SpriteFrame anchor fields +0x2a/+0x2c for a frame
    // built from a full rect: the frame's centre is the natural anchor point).
    out.frames.push_back(SpriteFrame{std::move(texture), anchor_x, anchor_y});
  }
  return !out.frames.empty();
}

} // namespace

// ---------------------------------------------------------------------------
// Reference-counted sprite-frame lifecycle
// ---------------------------------------------------------------------------
// Ghidra Sprite.c: a Sprite owns a resizeable frame pointer array and each
// SpriteFrame carries a refcount so a frame image can be shared across sprites
// (e.g. the player ship's rotation frames shared by the base + glow layers).
// The clean-room mirror keeps the same ownership: SpriteFrameImage is ref-
// counted via shared_ptr, Sprite::AddFrame retains one, Sprite::Release drops
// each (freeing the SDL handle at zero).
std::shared_ptr<SpriteFrameImage>
Sprite_InitFrameImage(std::unique_ptr<class SdlTexture> texture,
                      float anchor_x,
                      float anchor_y,
                      int width,
                      int height) {
  auto image = std::make_shared<SpriteFrameImage>();
  image->owned_texture = std::move(texture);
  // Explicit geometry: the SDL surface dimensions are known to the caller at
  // upload time (a null texture is allowed for lifecycle/refcount-only images
  // in tests).
  image->width = width;
  image->height = height;
  image->anchor_x = anchor_x;
  image->anchor_y = anchor_y;
  return image;
}

std::shared_ptr<SpriteFrameImage>
Sprite_InitFrameImage(float anchor_x, float anchor_y, int width, int height) {
  auto image = std::make_shared<SpriteFrameImage>();
  image->anchor_x = anchor_x;
  image->anchor_y = anchor_y;
  image->width = width;
  image->height = height;
  return image;
}

// Ghidra 0x00475740 Sprite_AddFrame.
int Sprite::AddFrame(std::shared_ptr<SpriteFrameImage> frame) {
  if (!frame) {
    return -1; // Ghidra Sprite_AddFrame's null-append failure path
  }
  frames_.push_back(std::move(frame));
  return static_cast<int>(frames_.size()) - 1;
}

// Ghidra 0x00476bd0 Sprite_Release.
void Sprite::Release() {
  // Releasing the sprite drops each frame's image refcount; an image reaching
  // zero frees its SDL handle (shared_ptr does this automatically).
  frames_.clear();
  current_frame_ = 0;
}

void Sprite::SetCurrentFrame(int index) {
  if (frames_.empty()) {
    current_frame_ = 0;
    return;
  }
  // Ghidra Sprite_SetCurrentFrame clamps to the live frame range.
  current_frame_ = std::clamp(index, 0, FrameCount() - 1);
}

void Sprite::SetPositionFromCurrentFrameAnchor(std::int16_t world_x,
                                               std::int16_t world_y) {
  // Ghidra Sprite_SetPositionFromCurrentFrameAnchor (0x00475af0): aligns the
  // current frame's anchor to the world position. The clean-room view performs
  // the real alignment in DrawSprite (per-frame anchor); here we just record
  // the located position the same way the original stores it on the sprite.
  located_x_ = static_cast<float>(world_x);
  located_y_ = static_cast<float>(world_y);
}

const SpriteFrameImage *Sprite::TheFrame(int index) const {
  if (frames_.empty()) {
    return nullptr;
  }
  const int clamped = std::clamp(index, 0, FrameCount() - 1);
  return frames_[static_cast<std::size_t>(clamped)].get();
}

// Ghidra 0x00475f70 Sprite_AssignSpriteSet.
int Sprite_AssignAsset(Sprite &sprite,
                       std::shared_ptr<const SpriteAsset> asset) {
  if (!asset || asset->frames.empty()) {
    return 0;
  }
  Sprite rebuilt;
  // Build a reference-counted frame image per asset frame; each retains the
  // shared asset so the frame pixels (SDL textures) outlive the sprite, and
  // records which frame of the set it presents.
  for (int i = 0; i < asset->frame_count; ++i) {
    const SpriteFrame &frame = asset->frames[static_cast<std::size_t>(i)];
    auto image = std::make_shared<SpriteFrameImage>();
    image->anchor_x = frame.anchor_x;
    image->anchor_y = frame.anchor_y;
    image->width = asset->tile_width;
    image->height = asset->tile_height;
    image->source_set = asset; // keeps the set (textures) alive
    image->source_index = i;
    rebuilt.AddFrame(std::move(image));
  }
  // Replace the sprite's frame set wholesale (Sprite_AssignSpriteSet rebinds
  // the frame list, releasing the old set).
  sprite.Release();
  sprite = std::move(rebuilt);
  return asset->frame_count;
}

// Ghidra 0x00474ab0 Sprite_CreateFromSpriteSheetResources.
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

std::unique_ptr<SpriteAsset> SpriteAsset::LoadCicnSet(SDL_Renderer *renderer,
                                                      std::uint16_t first_id,
                                                      int count) {
  auto asset = std::make_unique<SpriteAsset>();
  asset->frame_count = count;
  bool have_dimensions = false;
  for (int i = 0; i < count; ++i) {
    const std::uint16_t id = static_cast<std::uint16_t>(first_id + i);
    const auto data = NovaResource_Load(kResourceTypeCicn, id);
    if (!data) {
      NovaLog::Warn("cicn frame: no resource {}", static_cast<unsigned>(id));
      return nullptr;
    }
    const auto image = Resource_LoadCicnAsImage(*data);
    if (!image) {
      NovaLog::Warn("cicn frame {}: could not decode", static_cast<unsigned>(id));
      return nullptr;
    }
    auto texture = SdlTexture::Create(
        renderer, image->width, image->height, image->rgba_pixels);
    if (!texture) {
      NovaLog::Warn("cicn frame {}: texture upload failed",
                    static_cast<unsigned>(id));
      return nullptr;
    }
    if (!have_dimensions) {
      asset->tile_width = image->width;
      asset->tile_height = image->height;
      have_dimensions = true;
    }
    // Top-left anchor (0,0), matching SpriteFrame_CreateFromRect (0x00476400):
    // it writes anchor_x/anchor_y (frame +0x2a/+0x2c) = 0 for a frame built
    // from the icon rect. The original's reticle updaters position the four
    // brackets WITHOUT the half-span compensation the ship/stellar updaters
    // apply, so the frame's top-left lands exactly on the placement point.
    asset->frames.push_back(
        SpriteFrame{std::move(texture), 0.0F, 0.0F});
  }
  if (asset->frames.empty()) {
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

SpriteAnchorTransform Sprite_AnchorToScreen(float world_x,
                                            float world_y,
                                            float camera_world_x,
                                            float camera_world_y,
                                            int viewport_w,
                                            int viewport_h,
                                            float anchor_x,
                                            float anchor_y,
                                            float scale,
                                            bool wrap) {
  SpriteAnchorTransform out;
  // Uniform camera transform: a world point lands at (world - camera) + half
  // the viewport for the ship-centred camera.
  float sx = (world_x - camera_world_x) + static_cast<float>(viewport_w) / 2.0F;
  float sy = (world_y - camera_world_y) + static_cast<float>(viewport_h) / 2.0F;
  if (wrap) {
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
  // Sprite_SetPositionFromCurrentFrameAnchor (0x00475af0): shift the target so
  // the frame's anchor point (anchor_x/anchor_y, scaled) lands on the world
  // position -- i.e. the top-left goes to target - anchor*scale.
  out.screen_x = sx - anchor_x * scale;
  out.screen_y = sy - anchor_y * scale;
  return out;
}

namespace {
// Renders one frame texture at an anchor-aligned position (shared by the asset-
// and sprite-based DrawSprite overloads so the placement/alpha logic lives in
// exactly one place). `texture` is the frame's SDL handle; `width/height` are
// its native pixel size; `anchor_x/anchor_y` the (already resolved) frame-local
// anchor.
void BlitFrame(SDL_Renderer *renderer,
               const SdlTexture &texture,
               float width,
               float height,
               float anchor_x,
               float anchor_y,
               float world_x,
               float world_y,
               float camera_world_x,
               float camera_world_y,
               int viewport_w,
               int viewport_h,
               const SpriteDrawOptions &opts) {
  const SpriteAnchorTransform pos = Sprite_AnchorToScreen(world_x,
                                                          world_y,
                                                          camera_world_x,
                                                          camera_world_y,
                                                          viewport_w,
                                                          viewport_h,
                                                          anchor_x,
                                                          anchor_y,
                                                          opts.scale,
                                                          opts.wrap);
  const SDL_FRect dest{pos.screen_x, pos.screen_y, width, height};

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

// Resolves a frame image's SDL texture + native size: asset-backed images use
// the shared set (kept alive via shared_ptr), standalone images their own
// owned_texture. Returns null when no pixels are available.
const SdlTexture *ResolveFrameTexture(const SpriteFrameImage &image,
                                      int &out_width,
                                      int &out_height) {
  out_width = image.width;
  out_height = image.height;
  if (image.owned_texture) {
    return image.owned_texture.get();
  }
  if (image.source_set && image.source_index >= 0 &&
      image.source_index < image.source_set->frame_count) {
    const SpriteFrame &sf =
        image.source_set->frames[static_cast<std::size_t>(image.source_index)];
    if (sf.texture) {
      out_width = image.source_set->tile_width;
      out_height = image.source_set->tile_height;
      return sf.texture.get();
    }
  }
  return nullptr;
}
} // namespace

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
  const SpriteFrame &sf = asset.frames[static_cast<std::size_t>(clamped)];
  if (!sf.texture) {
    return;
  }
  // Anchor-aware placement: align the frame's anchor (or the opts override) to
  // the world position -- the genuine Sprite_SetPositionFromCurrentFrameAnchor
  // math. For tile/sheet frames the stored anchor is the frame centre, so this
  // draws centred exactly as before; a caller providing opts.anchor_* (e.g. a
  // shot's gun-fire point) gets the real anchor alignment instead.
  const auto [ax, ay] =
      (opts.anchor_x && opts.anchor_y)
          ? std::pair<float, float>{*opts.anchor_x, *opts.anchor_y}
          : std::pair<float, float>{sf.anchor_x, sf.anchor_y};
  BlitFrame(renderer,
            *sf.texture,
            static_cast<float>(asset.tile_width) * opts.scale,
            static_cast<float>(asset.tile_height) * opts.scale,
            ax,
            ay,
            world_x,
            world_y,
            camera_world_x,
            camera_world_y,
            viewport_w,
            viewport_h,
            opts);
}

void DrawSprite(SDL_Renderer *renderer,
                const Sprite &sprite,
                int frame,
                float world_x,
                float world_y,
                float camera_world_x,
                float camera_world_y,
                int viewport_w,
                int viewport_h,
                const SpriteDrawOptions &opts) {
  const SpriteFrameImage *image = sprite.TheFrame(frame);
  if (!image) {
    return;
  }
  int width = 0;
  int height = 0;
  const SdlTexture *texture = ResolveFrameTexture(*image, width, height);
  if (!texture) {
    return;
  }
  const auto [ax, ay] =
      (opts.anchor_x && opts.anchor_y)
          ? std::pair<float, float>{*opts.anchor_x, *opts.anchor_y}
          : std::pair<float, float>{image->anchor_x, image->anchor_y};
  BlitFrame(renderer,
            *texture,
            static_cast<float>(width) * opts.scale,
            static_cast<float>(height) * opts.scale,
            ax,
            ay,
            world_x,
            world_y,
            camera_world_x,
            camera_world_y,
            viewport_w,
            viewport_h,
            opts);
}

} // namespace game
