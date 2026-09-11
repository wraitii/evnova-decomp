#pragma once

// Clean-room sprite pixel-mask collision machinery.
//
// The original tests weapon/ship/asteroid contact with a prepared-frame RLE
// command stream:
//   * Sprite_TestPixelMaskOverlap (0x00475c80) intersects the two sprites'
//     current-frame screen bounds, converts the overlap rectangle into each
//     sprite's frame-local space, and calls
//     BlitPixie_CopyRectRegionBetweenFrames (0x00471800).
//   * BlitPixie_CopyRectRegionBetweenFrames scales the row offsets by each
//     frame's pixel-stride shift and delegates to
//     SpriteRleCommandStream_SkipToRowCount (0x00472190), which walks the two
//     RLE runs and returns true as soon as both are inside an opaque run
//     (command 2 = opaque/data, command 3 = transparent, command 1 = end of
//     row, command 0 = terminator).
// The result is a plain binary query: "is there any pixel that is opaque in
// both frames?" in the frames' shared screen/world space.
//
// The clean-room keeps the decoded sprite sheets as RGBA8 pixels (see
// RleSpriteSheet_Decode16), so a frame's mask is the alpha>0 bitmap of those
// pixels. Sprite_TestBoundingCircleOverlap (0x00475be0) is kept as the cheap
// fallback the original uses when the shot half-span is <= 0x20 or the average
// frame time is high (Ship_HandleSpritePairCollision 0x004374f0).

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace game {

// One frame's binary opacity mask. `opaque` is width*height bytes (0 or 1),
// row-major from the frame's top-left. This is the clean-room equivalent of the
// prepared-frame RLE command stream that SpriteRleCommandStream_SkipToRowCount
// consumes.
struct SpriteMask {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> opaque;

  [[nodiscard]] bool Empty() const {
    return width <= 0 || height <= 0 || opaque.empty();
  }

  [[nodiscard]] bool OpaqueAt(int x, int y) const {
    if (x < 0 || y < 0 || x >= width || y >= height) {
      return false;
    }
    return opaque[static_cast<std::size_t>(y) *
                      static_cast<std::size_t>(width) +
                  static_cast<std::size_t>(x)] != 0;
  }
};

// A sprite frame mask bound to a live entity. `anchor_x/anchor_y` is the
// frame-local point Sprite_SetPositionFromCurrentFrameAnchor (0x00475af0)
// aligns to the entity's world position (the tile centre for spin/shot/asteroid
// sets, the frame centre for the clean-room ship sheets). `mask` is non-owning:
// it points into the GameState-owned SpriteMaskStore (or a test-owned mask). A
// null `mask` means the entity has no resolved mask, so collision falls back to
// the bounding circle.
struct CollisionMaskBinding {
  const SpriteMask *mask = nullptr;
  int frame = -1;
  float anchor_x = 0.0F;
  float anchor_y = 0.0F;

  [[nodiscard]] bool HasMask() const {
    return mask != nullptr && !mask->Empty();
  }
};

// Builds a binary mask from interleaved RGBA8 pixels (4 bytes/pixel, alpha at
// +3). A pixel is opaque when its alpha is >= `alpha_threshold` (the original
// treats any nonzero entry in the stored 16-bit frame as opaque; alpha 0 is the
// transparent skip).
[[nodiscard]] SpriteMask
SpriteMask_FromRgba(std::span<const std::uint8_t> rgba_pixels,
                    int width,
                    int height,
                    std::uint8_t alpha_threshold = 1);

// Mirrors Sprite_TestPixelMaskOverlap (0x00475c80) with the RLE overlap scanner
// inlined: places each mask so its anchor lands on the entity's world position,
// intersects the two axis-aligned frame bounds, and returns true when any
// pixel is opaque in both. Coordinates are rounded to the original's integer
// screen space.
[[nodiscard]] bool SpriteMask_TestOverlap(const SpriteMask &a,
                                          float a_world_x,
                                          float a_world_y,
                                          float a_anchor_x,
                                          float a_anchor_y,
                                          const SpriteMask &b,
                                          float b_world_x,
                                          float b_world_y,
                                          float b_anchor_x,
                                          float b_anchor_y);

// Mirrors Sprite_TestBoundingCircleOverlap (0x00475be0): centre = frame
// top-left + half height, radius = half height, strict `<` on squared distance.
// Used as the fallback for entities without a resolved mask and for the small
// shot half-span case in Ship_HandleSpritePairCollision.
[[nodiscard]] bool SpriteMask_TestBoundingCircleOverlap(int a_width,
                                                        int a_height,
                                                        float a_world_x,
                                                        float a_world_y,
                                                        int b_width,
                                                        int b_height,
                                                        float b_world_x,
                                                        float b_world_y);

// Convenience wrapper used by the collision pass. When `allow_pixel_mask` is
// true and both bindings carry a resolved mask, the pixel overlap is used;
// otherwise (no mask, or the original's frame-time/short-span rule selected
// the circle) it falls back to the bounding circle using the supplied radii.
[[nodiscard]] bool CollisionMask_TestContact(const CollisionMaskBinding &a,
                                             float a_world_x,
                                             float a_world_y,
                                             int a_circle_radius,
                                             const CollisionMaskBinding &b,
                                             float b_world_x,
                                             float b_world_y,
                                             int b_circle_radius,
                                             bool allow_pixel_mask = true);

// Lazy, non-SDL cache of frame masks decoded from the game's rl\x91D sheets,
// keyed by the same resource ids the renderer uses (spin sets for shots,
// asteroids and stellars; bare sheets for ship hulls). The store never touches
// SDL, so the simulation layer can resolve masks without a renderer. A failed
// load is cached as an empty entry (mask lookups return null).
class SpriteMaskStore {
public:
  SpriteMaskStore();
  ~SpriteMaskStore();
  SpriteMaskStore(SpriteMaskStore &&) noexcept;
  SpriteMaskStore &operator=(SpriteMaskStore &&) noexcept;
  SpriteMaskStore(const SpriteMaskStore &) = delete;
  SpriteMaskStore &operator=(const SpriteMaskStore &) = delete;

  // Masks for a spin descriptor (sp\x9an): loads the named rl\x91D tile grid
  // and returns the mask for `frame` (clamped), or null.
  [[nodiscard]] const SpriteMask *Spin(std::uint16_t spin_id, int frame) const;

  // Masks for a bare rl\x91D sheet (ship hull / engine-glow frames).
  [[nodiscard]] const SpriteMask *Sheet(std::uint16_t sheet_id,
                                        int frame) const;

  // Frame counts of the cached sets (0 when the resource cannot be loaded).
  // The renderer uses the same count for heading-frame selection and asteroid
  // wander wrapping.
  [[nodiscard]] int SpinFrameCount(std::uint16_t spin_id) const;
  [[nodiscard]] int SheetFrameCount(std::uint16_t sheet_id) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace game
