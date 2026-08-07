#pragma once

// Per-frame flight rendering for the spaceflight loop. Owns the SDL-side caches
// (textures) that back the world draw: the player's rotating ship sprite and
// (future) the spin sprite sets. Kept off GameState so the simulation stays
// independent of SDL (AGENTS.md); the spaceflight loop owns a SpaceflightView
// for the duration of the session.
//
// View model (Ghidra Frame_RenderViewportBackground + the sprite-world draw in
// Frame_SpaceflightLoop scope 2): the camera is centred on the player, so a
// world point (wx, wy) maps to screen (wx - player.x + half_viewport,
// wy - player.y + play_area_top + half_height). The player ship is drawn at
// the centre of the play area with its frame selected by heading.

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

// Forward declarations of the SDL types (defined in sdl_platform.hpp / SDL3,
// global namespace) so this header can declare the view's interface without
// pulling in SDL here.
class SdlPlatform;
class SdlTexture;
struct SDL_Renderer;

#include "scenario_data.hpp"

namespace game {
struct GameState;

class SpaceflightView {
public:
  SpaceflightView() = default;
  SpaceflightView(const SpaceflightView &) = delete;
  SpaceflightView &operator=(const SpaceflightView &) = delete;

  // Ensures the player's ship sprite sheet (from sh\x8an BaseImageID) is
  // decoded and uploaded, and returns false if it could not be loaded.
  [[nodiscard]] bool EnsureShipSprite(SdlPlatform &platform,
                                      const GameState &state);

  // Draws the whole in-flight world (solid per-system space backdrop, ambient
  // starfield, stellar bodies, player ship) into the current renderer.
  void Draw(SdlPlatform &platform, const GameState &state);

  // Ghidra NovaEffects_QueuedAmbientStarParticles (0x0046ebf0): (re)spawns the
  // 20-slot ambient starfield around the player ship. Spawn count is
  // round(viewportHeight / 600.0 * 20.0) fresh stars; each gets a random world
  // offset within the current viewport (centred on the ship), a random star
  // sprite frame in [0, star_field_.frame_count), and a random per-particle
  // parallax speed = NovaRandom_Range(0x23) * 0.01 (0.00..0.34). The remaining
  // (20-count) slots are merely re-activated with their previous position/speed
  // (outer-slot carry-over). When the system's murk (SystemDef.murk) is
  // negative the field is cleared instead (stars hidden). Called at every
  // spaceflight entry / travel boundary (Ghidra travel/landing paths call it
  // on system entry). Mutates the GameState PRNG, so it is non-const.
  void SpawnAmbientStars(SdlPlatform &platform, GameState &state);

  // Ghidra NovaEffects_UpdateAmbientStarParticles (0x0046ee50): advances the
  // ambient starfield each frame by the ship's movement delta (dx, dy). Each
  // active particle whose parallax speed exceeds the (0.0) drift threshold
  // gains (dx, dy) * per-particle speed, so nearer/faster particles stream
  // past while slow (speed 0) distant ones stay fixed (real spatial parallax).
  void UpdateAmbientStars(float dx, float dy);

  // Draws the solid per-system space background tint (SystemDef.bkgnd_color,
  // Ghidra NovaRender_SetSystemSpaceBackgroundColor /
  // Frame_RenderViewportBackground) then the active ambient star particles.
  void DrawBackground(SdlPlatform &platform, const GameState &state);

  // Ghidra Stellar_UpdateStellarSprites (0x0042cd10), ambient-animation part
  // only: advances each of the current system's animated stellars' sprite frame
  // one animation step. frame_time_ms is the real elapsed frame time (the
  // original accumulates _g_avg_frame_time_ms into
  // StellarDef.sprite_frame_accumulator). Hypergate-style stellars
  // (availability_flags & 0x1000) use the non-engaged drift branch (clamped to
  // engage_highlight_frame); because this build has no AI ships /
  // travel-selection state the engage-highlight pulse is documented and left
  // untouched (TODO(decomp)). Mutates the GameState PRNG (random cycling) so it
  // is non-const.
  void AdvanceStellarAnimation(SdlPlatform &platform,
                               GameState &state,
                               float frame_time_ms);

private:
  // One ambient background star particle (Ghidra AmbientStarParticle pool at
  // g_ambient_star_particles, stride 0x14 = 20 bytes; offsets match the way we
  // index the original fields).
  struct AmbientStar {
    int frame = 0;       // +0x06 random star-sprite frame index at spawn
    float speed = 0.0F;  // +0x08 parallax drift speed (random per particle)
    float pos_x = 0.0F;  // +0x0c world-space x
    float pos_y = 0.0F;  // +0x10 world-space y
    bool active = false; // +0x04 active flag
  };

  std::array<AmbientStar, 20> ambient_stars_{};

  // The ambient star-field artwork: sp\x9an spin descriptor resource 700 is a
  // 4x4 grid of 5x5px star tiles (16 distinct star shapes). Each particle
  // renders one of these frames at native 1:1 size (the original's murk-derived
  // +0xa2..\x0aa8 values are blend-mode sentinels, not pixel sizes).
  // Ghidra: DAT_00593efc built by Spin_ReadDescriptor(700,..), frame count at
  // +0x54 = tiles_x * tiles_y.
  struct StarFieldSheet {
    std::vector<std::unique_ptr<class SdlTexture>> frames;
    int frame_count = 0;
    int tile_width = 0;  // 5
    int tile_height = 0; // 5
  };

  StarFieldSheet star_field_;

  // Unrotated per-frame ship textures plus rotation metadata.
  struct ShipSprite {
    std::vector<std::unique_ptr<class SdlTexture>> frames;
    int frame_count = 0; // base_set_count * frames_per_rotation
    int frames_per_rotation = 36;
    int width = 0;
    int height = 0;
  };

  ShipSprite ship_;
  // The ship's engine-glow layer, a second rl\x9144 sheet (sh\x8an
  // GlowImageID, e.g. 'Shuttle Eng Glow' 0x0578) sharing the base sheet's
  // rotation grid (same frame count), drawn over the base with a thrust-driven
  // alpha. Empty when the class has no glow layer.
  ShipSprite glow_;
  // Whether this class's sh\x8an descriptor named a glow layer at all (so
  // EnsureShipSprite does not retry a missing sheet every frame).
  bool has_glow_ = false;
  // Last glow-draw gate result, so transitions (on/off) can be logged once per
  // change rather than per frame (diagnostic for the flight render).
  bool glow_last_drawn_ = false;

  // Decodes one rl\x9144 ship sheet (resource `id`) and uploads its frames as
  // textures. Returns nullopt when the sheet is missing/malformed. Multi-frame
  // rotation sheets (base + glow) share this decoder.
  [[nodiscard]] std::optional<ShipSprite>
  LoadShipSprite(SDL_Renderer *renderer, std::uint16_t resource_id);

  // One decodable spin sprite set (a stellar/planet graphic). Frames are
  // arranged in a grid of tiles_x * tiles_y; only the first frame is drawn
  // for now (TODO(decomp): advance the ambient stellar animation).
  struct SpinSpriteSet {
    std::vector<std::unique_ptr<class SdlTexture>> frames;
    int frame_count = 0;
    int width = 0;  // tiles_x
    int height = 0; // tiles_y
    int tile_width = 0;
    int tile_height = 0;
  };

  // spin sprite-set id -> loaded set (lazily populated; id+1000 is the spin
  // resource id for the set).
  std::vector<std::unique_ptr<SpinSpriteSet>> spin_sets_;

  // Loads (and caches) the spin sprite set for a stellar's graphic
  // (spin_set_id is the stellar's link_a_id; spin set id + 1000 is the spin
  // resource id). Returns null when the set cannot be loaded.
  [[nodiscard]] const SpinSpriteSet *GetSpinSpriteSet(SdlPlatform &platform,
                                                      int spin_set_id);

  // Loads (and caches) the ambient star-field sheet (sp\x9an resource 700).
  // Returns null when it could not be decoded; on failure stars fall back to
  // plain points. Ghidra DAT_00593efc.
  [[nodiscard]] const StarFieldSheet *
  EnsureStarFieldSheet(SdlPlatform &platform);

  // One animated stellar's frame-stepping runtime state (the original keeps
  // these on StellarDef sprite_current_frame / sprite_previous_frame /
  // sprite_frame_accumulator, +0x476/+0x478/+0x490). Initial: current and
  // previous both 0, accumulator 0.
  struct StellarAnimState {
    int current_frame = 0;
    int previous_frame = 0;
    float frame_accumulator = 0.0F;
  };

  // stellar id (resource id) -> runtime animation state for animated stellars.
  std::map<std::int16_t, StellarAnimState> stellar_anims_;

  // Draws the current system's stellar bodies (planets/stations) at their
  // world positions relative to the player camera.
  void DrawStellarBodies(SdlPlatform &platform, const GameState &state);

  // Draws the player's in-flight active shots (GameState.active_shots) at
  // their world positions relative to the ship, using the same camera
  // transform as the stars/stellars. The light blaster's projectile is drawn
  // as a small bright dot (TODO(decomp): mount the shot's spin sprite set once
  // the shot sprite cache is reconstructed).
  void DrawShots(SdlPlatform &platform, const GameState &state);
};

} // namespace game
