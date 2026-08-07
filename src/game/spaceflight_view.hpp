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
#include "sprite_world.hpp"

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
  // sprite frame in [0, starFieldFrameCount), and a random per-particle
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

  // Unified per-frame animation advance. Mirrors SpriteWorld_UpdateAnimated-
  // Sprites (0x004781f0): one world-level pass each frame advances every
  // animated in-flight entity's frame cadence so the timing basis bounds live
  // in a single place rather than being duplicated per subsystem. This is the
  // only per-frame world-animation hook the spaceflight loop calls; it drives:
  //   * animated stellar frame stepping (AdvanceStellarAnimation),
  //   * ambient-star parallax movement (UpdateAmbientStars, via `dx/dy`),
  // while the player's time-animated shot frames step in NovaWeapon_TickShots
  // (simulation side, same dwell model). frame_time_ms / dx / dy are the real
  // elapsed frame time and ship-movement delta (the original's
  // _g_avg_frame_time_ms + the pre-tick position delta). Mutates GameState
  // (PRNG for random cycling), so it is non-const.
  void AdvanceAnimations(SdlPlatform &platform,
                         GameState &state,
                         float frame_time_ms,
                         float dx,
                         float dy);

private:
  // Ghidra Stellar_UpdateStellarSprites (0x0042cd10), ambient-animation part
  // only: advances each of the current system's animated stellars' sprite frame
  // one animation step. frame_time_ms is the real elapsed frame time (the
  // original accumulates _g_avg_frame_time_ms into
  // StellarDef.sprite_frame_accumulator). Hypergate-style stellars
  // (availability_flags & 0x1000) use the non-engaged drift branch (clamped to
  // engage_highlight_frame); because this build has no AI ships /
  // travel-selection state the engage-highlight pulse is documented and left
  // untouched (TODO(decomp)). Mutates the GameState PRNG (random cycling) so it
  // is non-const. Called from AdvanceAnimations.
  void AdvanceStellarAnimation(SdlPlatform &platform,
                               GameState &state,
                               float frame_time_ms);

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

  // Shared spin-sprite asset cache (stellar +1000, weapon shot +3000, star
  // field 700 are all just different spin resource ids into the one store).
  // Ghidra: g_weapon_sprite_set_table + the stellar/spin sprite-set tables.
  SpriteStore sprite_store_;

  // The ambient star-field artwork: sp\x9an spin descriptor resource 700 is a
  // 4x4 grid of 5x5px star tiles (16 distinct star shapes). Kept as an asset in
  // the store (id 700); each particle renders one frame at native 1:1 size.
  // Ghidra: DAT_00593efc built by Spin_ReadDescriptor(700,..), frame count =
  // tiles_x * tiles_y.
  [[nodiscard]] const SpriteAsset *StarFieldSheet(SdlPlatform &platform);

  // The player ship's heading-rotation sheet and its engine-glow layer, both
  // bare rl\x91D sheets (sh\x8an BaseImageID / GlowImageID) sharing the same
  // rotation grid. Empty when the class has none.
  SpriteAsset ship_;
  SpriteAsset glow_;
  // Rotation frames for one full revolution (sh\x8an FramesPer, default 36);
  // frame count = base_set_count * frames_per_rotation.
  int ship_frames_per_rotation_ = 36;
  // Whether this class's sh\x8an descriptor named a glow layer at all (so
  // EnsureShipSprite does not retry a missing sheet every frame).
  bool has_glow_ = false;
  // Last glow-draw gate result, so transitions (on/off) can be logged once per
  // change rather than per frame (diagnostic for the flight render).
  bool glow_last_drawn_ = false;

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
  // transform as the stars/stellars. Each shot is drawn with its weapon's
  // shot sprite set (spin resource shot_sprite_set_id + 3000), currently
  // the first frame at native size; falls back to a small bright dot when the
  // sprite set is unavailable.
  void DrawShots(SdlPlatform &platform, const GameState &state);
};

} // namespace game
