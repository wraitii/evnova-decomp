#pragma once

// Unified sprite machinery for the flight renderer, modelling Nova's shared
// Sprite / sprite-world concepts. In the original every visible in-flight
// object (ship, glow, shot, stellar, ambient star, beam, particle) is a Sprite
// instance drawn through one viewport pass; `Sprite_Create` /
// `Sprite_AssignSpriteSet` bind a frame set, and `Sprite_SetPositionFrom-
// CurrentFrameAnchor` positions the sprite so its frame's anchor point (centre
// of mass / gun fire point) lands on the world position. A shot/weapon sprite
// set table (`g_weapon_sprite_set_table`, FUN_004ad960) and a stellar sprite
// set table (link_a_id spin range +1000) back those frame sets.
//
// This clean-room module collapses the flight view's several per-entity sprite
// caches (star field, stellar sets, weapon shot sets, ship/glow sheets) into
// one `SpriteAsset` representation and one shared world->screen draw, so the
// less general per-subsystem drawers can all route through it. It does not yet
// replicate the original's dirty-rect compositor / layer animation timers.
//
// Two asset sources mirror the original's two frame-set builders:
//   * spin descriptors (sp\x9an): a code names a resource whose SpritesID names
//     an rl\x91D sheet of `tile_width x tile_height` tiles. Used for stellars
//     (id + 1000), weapon shots (id + 3000) and the amber star field (700).
//   * bare rl\x91D sheets: each frame is one image (e.g. the player ship's
//     heading-rotation sheet and its engine-glow layer, from sh\x8an).

#include <cstdint>
#include <memory>
#include <vector>

struct SDL_Renderer; // defined in SDL3 (included by sdl_platform.hpp)
class SdlTexture;

namespace game {

// A decoded, SDL-uploaded set of square-tile frames (one texture per frame).
// This is the single asset type backing every in-flight sprite draw; `tile`
// is one frame's on-screen size, `frames` holds each tile's texture.
//
// Ghidra: the sprite-set resource block a Sprite_AssignSpriteSet attaches to a
// sprite; the frame reference list / textures mirror Sprite_CreateFromSprite-
// SheetResources (tile grid) and Sprite_CreateFromMultiFrameResource (bare
// sheet).
struct SpriteAsset {
  std::vector<std::unique_ptr<class SdlTexture>> frames;
  int frame_count = 0; // == frames.size() when loaded
  int tile_width = 0;  // one frame's pixel width
  int tile_height = 0; // one frame's pixel height

  // Loads a spin-descriptor sprite set (sp\x9an resource `spin_id`, whose
  // SpritesID names the rl\x91D sheet). Returns null when the descriptor or
  // sheet is missing/malformed. Ghidra: Sprite_CreateFromSpriteSheetResources
  // + Sprite_PrepareFramesAndAttachResourceData on a Spin_ReadDescriptor
  // result.
  [[nodiscard]] static std::unique_ptr<SpriteAsset>
  LoadSpin(SDL_Renderer *renderer, std::uint16_t spin_id);

  // Loads a bare rl\x91D sheet where every frame is a full image (ship
  // rotation / engine-glow frames). Returns null on missing/malformed data.
  [[nodiscard]] static std::unique_ptr<SpriteAsset>
  LoadSheet(SDL_Renderer *renderer, std::uint16_t sheet_id);

  [[nodiscard]] bool empty() const { return frames.empty(); }
};

// Lazily-populated spin-descriptor asset cache. Keys are the sp\x9an resource
// id (e.g. stellar 1000+i, weapon shot 3000+i, star field 700), so the various
// entity loads collapse into a single behind-one-key lookup. A failed load
// returns null and is not cached (a transiently-missing set is re-queried).
// Ghidra: the per-role sprite-set tables (g_weapon_sprite_set_table + the
// stellar/spin table) each cache their sets by id.
class SpriteStore {
public:
  // Returns the cached asset for a spin descriptor id, or null when it cannot
  // be loaded. A failed load is not cached (a transiently-missing set is
  // re-queried).
  [[nodiscard]] const SpriteAsset *Spin(SDL_Renderer *renderer,
                                        std::uint16_t spin_id);

private:
  // spin descriptor resource id -> loaded set (or null on failure).
  std::vector<std::unique_ptr<SpriteAsset>> sets_;
};

// Draw options for one sprite instance, mirroring the original sprite's
// visible/frame/anchor presentation state (Sprite_SetVisible /
// Sprite_SetCurrentFrame / Sprite_SetPositionFromCurrentFrameAnchor).
struct SpriteDrawOptions {
  float scale = 1.0F;     // native-frame scale (1 = 1:1)
  float alpha_mod = 1.0F; // 0..1 multiplicative alpha (glow dimming)
  bool wrap = false;      // one-exit wraparound for the extending viewport
  // Use the smooth (linear) filtering when the frame is drawn scaled, e.g. the
  // tiny 5x5px star tiles upscaled so they read as soft glows rather than
  // chunky squares (matches the original's smooth sprite scaling draw-proc).
  bool linear_scale = false;
};

// Draws one `asset` frame at world position (world_x, world_y) into the world
// viewport, applying the uniform camera transform (camera centred on
// `camera_world_x/y`, normally the player ship) and the requested options.
// `viewport_w/h` bound the wraparound/clip. All in-flight entity drawers
// (stellars, shots, stars, ship) route through this so the world->screen
// projection and centering logic live in exactly one place.
//
// Ghidra: the per-sprite viewport draw that positions a Sprite from its world
// coords via Sprite_SetPositionFromCurrentFrameAnchor then blits its current
// frame; blended alpha corresponds to the original's texture/blend effect
// color.
void DrawSprite(SDL_Renderer *renderer,
                const SpriteAsset &asset,
                int frame,
                float world_x,
                float world_y,
                float camera_world_x,
                float camera_world_y,
                int viewport_w,
                int viewport_h,
                const SpriteDrawOptions &opts = {});

} // namespace game
