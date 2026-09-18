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
#include <optional>
#include <vector>

struct SDL_Renderer; // defined in SDL3 (included by sdl_platform.hpp)
class SdlTexture;

namespace game {
struct SpriteAsset;
struct SpriteDrawOptions;

// One decoded frame of a sprite set: its SDL texture plus the frame-local
// anchor point (in pixels, measured from the frame's top-left). This mirrors
// the original SpriteFrame's surface handle (+0x00..) and its anchor pair
// (+0x2a anchor_x, +0x2c anchor_y). `Sprite_SetPositionFromCurrentFrameAnchor`
// positions a sprite so the current frame's anchor lands on the world position;
// a shot's gun-fire point, a ship's centre of mass, a stellar's centre. For
// tile-grid (spin) frame sets built by Sprite_CreateFromSpriteSheetResources
// the anchor is each tile's centre (tile_width/2, tile_height/2); for bare
// sheets (ship rotation frames) the anchor is the class-defined position.
struct SpriteFrame {
  std::unique_ptr<class SdlTexture> texture;
  // Alpha silhouette uploaded for bare rl\x91D sheets. Ship emergence uses
  // the source frame's alpha with solid-white RGB, matching the original's
  // pure-white distance tint without requiring a custom SDL shader.
  std::unique_ptr<class SdlTexture> white_silhouette;
  // Frame-local anchor offset (pixels from the frame's top-left). Defaulted to
  // the centred case by the loaders unless set otherwise.
  float anchor_x = 0.0F;
  float anchor_y = 0.0F;
};

// A decoded, SDL-uploaded set of frames (one texture per frame). This is the
// single asset type backing every in-flight sprite draw; `tile` is one
// (square-tile) frame's on-screen size, `frames` holds each frame.
//
// Ghidra: the sprite-set resource block a Sprite_AssignSpriteSet attaches to a
// sprite; the frame reference list / textures mirror Sprite_CreateFromSprite-
// SheetResources (tile grid) and Sprite_CreateFromMultiFrameResource (bare
// sheet).
struct SpriteAsset {
  std::vector<SpriteFrame> frames;
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

  // Loads a run of consecutive `cicn` color-icon resources as one frame set:
  // `first_id`, `first_id+1`, ..., `first_id+count-1` each decode to one frame
  // (Ghidra Sprite_CreateFromSequentialFrameResources,
  // SpriteFrame_CreateFromCicnResource). Used for the 16-frame ship-target
  // reticle (cicn 10008-10023) and the 8-frame travel reticle (cicn
  // 10000-10007). Every frame's anchor is its top-left (0,0) --
  // SpriteFrame_CreateFromRect (0x00476400) zeroes the anchor pair (+0x2a/
  // +0x2c) for a full-icon-rect frame, and the original positions the four
  // corner brackets without the half-span compensation the ship/stellar
  // updaters apply. Returns null (caller falls back to the debug bracket
  // draws) when any required frame is missing.
  [[nodiscard]] static std::unique_ptr<SpriteAsset>
  LoadCicnSet(SDL_Renderer *renderer, std::uint16_t first_id, int count);

  [[nodiscard]] bool empty() const { return frames.empty(); }
};

// Sprite lifecycle + refcounting, mirroring the original Sprite.c ownership
// model (Sprite_Create 0x004744a0 / Sprite_AddFrame 0x00475740 /
// Sprite_Release 0x00476bd0). In the original a Sprite owns a resizeable frame
// pointer array and each SpriteFrame carries its own refcount so frames can be
// shared across sprites (e.g. the player ship's rotation frames shared by the
// base + glow layers). The clean-room model keeps that shape: a Sprite holds a
// resizeable list of reference-counted SpriteFrameImage handles, and the
// lifecycle helpers below guard create / append / release.

// The reference-counted image shared by sprite frames (one per frame). Ref-
// counting is carried by shared_ptr (the natural RAII refcount; the original's
// manual refcount is implicit in shared_ptr). The anchor
// (`anchor_x/anchor_y`) is the frame-local point Sprite_SetPositionFromCurren-
// tFrameAnchor aligns to the world position (the tile centre for spin sets, the
// ship class's gun-fire / centre point for ship frames). A frame image derived
// from a loaded SpriteAsset keeps a shared reference to that set (source_set /
// source_index) so the actual frame pixels stay alive for the sprite's whole
// lifetime without copying the SDL texture; a refcount-only image (no set, no
// texture) carries just paint geometry for lifecycle tests. The original's
// SpriteFrame refcount/held surface is the shared_ptr here.
class SpriteFrameImage : public std::enable_shared_from_this<SpriteFrameImage> {
public:
  float anchor_x = 0.0F; // frame-local anchor (top-left)
  float anchor_y = 0.0F;
  int width = 0;  // frame pixel width
  int height = 0; // frame pixel height
  // When this image derives from an asset, the shared set + frame index keep
  // the underlying texture alive and identify which frame to present.
  std::shared_ptr<const SpriteAsset> source_set;
  int source_index = -1; // frame index into source_set, or -1
  // The uploaded SDL texture directly owned when the image is not asset-derived
  // (null for pure-metadata / asset-backed images).
  std::unique_ptr<class SdlTexture> owned_texture;
  virtual ~SpriteFrameImage() = default;
};

// Sprite_InitFrameImage 0x00476f80 (SpriteFrame_EnsureLoaded analogue): wraps
// an uploaded SDL texture as a standalone (asset-free) reference-counted frame
// image. `owned_texture` may be non-null; anchor defaults to the frame centre
// unless overridden (width/height mirror the texture's pixel size when given).
[[nodiscard]] std::shared_ptr<SpriteFrameImage>
Sprite_InitFrameImage(std::unique_ptr<class SdlTexture> owned_texture,
                      float anchor_x = 0.0F,
                      float anchor_y = 0.0F,
                      int width = 0,
                      int height = 0);

// Lifecycle-only (no SDL texture) image for refcount tests and metadata use:
// carries only paint geometry/anchor, no pixels. Avoiding the incomplete-type
// SdlTexture parameter keeps this usable from headers that don't pull in SDL.
[[nodiscard]] std::shared_ptr<SpriteFrameImage>
Sprite_InitFrameImage(float anchor_x, float anchor_y, int width, int height);

// A runtime sprite instance owning a resizeable frame set (Ghidra Sprite +
// the frame array it allocates in Sprite_InitOrAllocate). Created empty; frames
// are appended via Sprite_AddFrame. `current_frame` tracks the sprite's
// presented frame (Sprite_SetCurrentFrame). This is the instance model
// Sprite_Create / Sprite_AssignSpriteSet build into in the original; the
// clean-room asset store keeps the read-only SpriteAsset cache and builders can
// assemble a live Sprite from one.
class Sprite {
public:
  Sprite() = default;
  Sprite(const Sprite &) = delete;
  Sprite &operator=(const Sprite &) = delete;
  Sprite(Sprite &&) noexcept = default;
  Sprite &operator=(Sprite &&) noexcept = default;

  // Sprite_AddFrame 0x00475740: appends an owned frame, retaining its image
  // refcount, growing the frame array as needed. Returns the new frame index
  // (0-based), or -1 on failure.
  int AddFrame(std::shared_ptr<SpriteFrameImage> frame);
  // Sprite_Release 0x00476bd0: releases this sprite, releasing each frame's
  // image refcount; an image reaching zero frees its SDL handle.
  void Release();
  // Sprite_SetCurrentFrame 0x00475830: makes `index` the presented frame,
  // clamped to the frame range.
  void SetCurrentFrame(int index);
  // The frame image at `index` (clamped), or null when empty.
  [[nodiscard]] const SpriteFrameImage *TheFrame(int index) const;
  // Sprite_SetPositionFromCurrentFrameAnchor (0x00475af0): stores the world
  // position this sprite's current-frame anchor is aligned to (the position
  // Sprite_SetCurrentFrame + the frame anchor place it). Pure bookkeeping that
  // DrawSprite's anchor-aware placement realises; kept so callers can query the
  // sprite's located position without re-deriving the anchor math.
  void SetPositionFromCurrentFrameAnchor(std::int16_t world_x,
                                         std::int16_t world_y);

  [[nodiscard]] float LocatedX() const { return located_x_; }

  [[nodiscard]] float LocatedY() const { return located_y_; }

  // Total appended frames (Ghidra Sprite.num_frames +0x54).
  [[nodiscard]] int FrameCount() const {
    return static_cast<int>(frames_.size());
  }

private:
  std::vector<std::shared_ptr<SpriteFrameImage>> frames_;
  int current_frame_ = 0;
  float located_x_ = 0.0F;
  float located_y_ = 0.0F;
};

// Sprite_AssignSpriteSet (0x00475f70) clean-room analogue: binds `asset`'s
// frames onto a live `Sprite`, replacing its current frame set. Each asset
// frame becomes a reference-counted SpriteFrameImage on the sprite; the sprite
// retains the shared asset so the SDL textures stay alive for the sprite's
// lifetime without copying them. Returns the number of frames bound, or 0 when
// the asset is empty. This is the step that lets a runtime sprite be assembled
// from a loaded SpriteAsset (the pipeline Sprite_CreateFromSpriteSheetResources
// feeds) for later SpriteWorld placement / Draw.
int Sprite_AssignAsset(Sprite &sprite,
                       std::shared_ptr<const SpriteAsset> asset);

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
  // re-queried). Const so read-only consumers (e.g. radar blip sizing) can
  // share the store; the cache itself is lazy.
  [[nodiscard]] const SpriteAsset *Spin(SDL_Renderer *renderer,
                                        std::uint16_t spin_id) const;

private:
  // spin descriptor resource id -> loaded set (or null on failure).
  mutable std::vector<std::unique_ptr<SpriteAsset>> sets_;
};

// Draw options for one sprite instance, mirroring the original sprite's
// visible/frame/anchor presentation state (Sprite_SetVisible /
// Sprite_SetCurrentFrame / Sprite_SetPositionFromCurrentFrameAnchor).
struct SpriteDrawOptions {
  float scale = 1.0F;     // native-frame scale (1 = 1:1)
  float alpha_mod = 1.0F; // 0..1 multiplicative source intensity
  // Additive compositing (dst = src*alpha + dst). The original's ship effect
  // layers (engine glow, running lights, weapon effects) draw through
  // BlitPixel_TintRgb15Span with brightness 0x20: dst + src*intensity/0x20, so
  // alpha_mod carries intensity/0x20 here. Transparent source pixels stay
  // transparent under SDL_BLENDMODE_ADD.
  bool additive = false;
  bool white_silhouette = false;
  bool wrap = false; // one-exit wraparound for the extending viewport
  // Use the smooth (linear) filtering when the frame is drawn scaled, e.g. the
  // tiny 5x5px star tiles upscaled so they read as soft glows rather than
  // chunky squares (matches the original's smooth sprite scaling draw-proc).
  bool linear_scale = false;
  // Effective system murk percent (System_GetEffectiveMurkPercent, 0-100).
  // When > 0 the sprite is distance-fogged toward the system space/background
  // colour, mirroring Frame_UpdateSpriteDistanceIntensity (0x00438db0) and the
  // fog stage of BlitPixel_TintRgb15Span (0x004736c0): the farther the sprite
  // is from the camera (the player), the more it fades to the backdrop. 0
  // disables the fog (a no-op in a clear-cut system).
  //
  // The fog is a mix toward a CONSTANT colour, not toward `dst`: the original
  // computes `src*(1 - d/32) + space_color*(d/32)` and (for an opaque hull)
  // writes it over the framebuffer, so fogged sprites occlude whatever is
  // behind them. The SDL path reproduces that with a two-pass draw: a
  // fog-coloured alpha silhouette (the frame's `white_silhouette`) is drawn
  // first to occlude `dst`, then the sprite is drawn at `src_factor` of its
  // own alpha over it. Where no silhouette is available the sprite falls back
  // to a plain source-alpha fade over `dst` (documented divergence).
  int fog_murk = 0;
  // The constant the fog mixes toward, 0xRRGGBB (the current system's
  // BkgndColor / space colour). Only consulted by non-additive draws with
  // fog_murk > 0 and an available silhouette.
  std::uint32_t fog_color = 0;
  // When provided, this per-frame anchor (in frame-local pixels, measured from
  // the frame's top-left) is used instead of the frame's stored anchor. This is
  // how a shot's gun-fire point or a non-centred ship frame positions itself:
  // the draw aligns `(world_x, world_y)` to this point within the frame, the
  // same math Sprite_SetPositionFromCurrentFrameAnchor performs with the
  // frame's stored +0x2a/+0x2c anchor pair.
  std::optional<float> anchor_x = std::nullopt;
  std::optional<float> anchor_y = std::nullopt;
};

// Positions a world-space point to screen using the same camera transform
// DrawSprite applies, then returns the frame's *top-left* screen origin given
// its anchor. Pure helper (no render side effects) mirroring the placement the
// original Sprite_SetPositionFromCurrentFrameAnchor (0x00475af0) computes: the
// anchor offset is subtracted from the target so the anchor lands on the world
// position. `ax/ay` are frame-local anchor pixels (frame centre for centred
// sets). Used by DrawSprite and exposed for callers that need the destination
// rect before blitting.
struct SpriteAnchorTransform {
  float screen_x = 0.0F; // top-left screen x after aligning the anchor
  float screen_y = 0.0F; // top-left screen y after aligning the anchor
};

// Ghidra 0x00438db0 Frame_UpdateSpriteDistanceIntensity: the per-sprite
// distance-brightness fog amount. distance_brightness = clamp(trunc(
// effective_murk * distSq * 1.2e-05), 0, 0x1f), where distSq is the sum of the
// squared x87-truncated absolute axis deltas between the player (camera) and
// the sprite's integer world position (the original receives shorts and
// truncates each sprite coordinate). effective_murk is the effective system
// murk and the 1.2e-05 constant is g_distance_intensity_scale_const2
// (0x005754d0). The original also clamps to 0x18 at 8-bit colour depth; every
// SDL texture is 32-bit here, so the 0x1f ceiling applies. A murk of 0 (a
// clear-cut system) always yields 0.
[[nodiscard]] int Sprite_DistanceBrightness(int effective_murk,
                                            float camera_x,
                                            float camera_y,
                                            float sprite_x,
                                            float sprite_y);

[[nodiscard]] SpriteAnchorTransform Sprite_AnchorToScreen(float world_x,
                                                          float world_y,
                                                          float camera_world_x,
                                                          float camera_world_y,
                                                          int viewport_w,
                                                          int viewport_h,
                                                          float anchor_x,
                                                          float anchor_y,
                                                          float scale = 1.0F,
                                                          bool wrap = false);

// Draws one `asset` frame at world position (world_x, world_y) into the world
// viewport. The frame is placed so its anchor point (the frame's stored anchor,
// or the caller's anchor via opts) lands on the world position -- the genuine
// behaviour of Ghidra Sprite_SetPositionFromCurrentFrameAnchor (0x00475af0) --
// rather than naively centering the frame. All in-flight entity drawers
// (stellars, shots, stars, ship) route through this so the world->screen
// projection and per-frame anchor alignment live in exactly one place.
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

// Draws a live `Sprite`'s current frame, resolving its pixels through the frame
// image's shared asset (or owned texture). This completes the lifecycle:
// Sprite_AssignAsset builds the sprite, and this is how it reaches the renderer
// with the same camera/anchor math DrawSprite applies to an asset. The world
// position is the sprite's anchor-aligned position (Sprite_SetPositionFrom-
// CurrentFrameAnchor).
void DrawSprite(SDL_Renderer *renderer,
                const Sprite &sprite,
                int frame,
                float world_x,
                float world_y,
                float camera_world_x,
                float camera_world_y,
                int viewport_w,
                int viewport_h,
                const SpriteDrawOptions &opts = {});

} // namespace game
