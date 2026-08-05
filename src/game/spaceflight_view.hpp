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
  // offset within the current viewport (centred on the ship) and a random
  // per-particle parallax speed = NovaRandom_Range(0x23) * 0.01 (0.00..0.34).
  // The remaining (20-count) slots are merely re-activated with their previous
  // position/speed (outer-slot carry-over). When the system's murk
  // (SystemDef.murk) is negative the field is cleared instead (stars hidden).
  // Called at every spaceflight entry / travel boundary (Ghidra travel/landing
  // paths call it on system entry). Mutates the GameState PRNG, so it is non-const.
  void SpawnAmbientStars(GameState &state);

  // Ghidra NovaEffects_UpdateAmbientStarParticles (0x0046ee50): advances the
  // ambient starfield each frame by the ship's movement delta (dx, dy). Each
  // active particle whose parallax speed exceeds the (0.0) drift threshold
  // gains (dx, dy) * per-particle speed, so nearer/faster particles stream
  // past while slow (speed 0) distant ones stay fixed (real spatial parallax).
  void UpdateAmbientStars(float dx, float dy);

  // Draws the solid per-system space background tint (SystemDef.bkgnd_color,
  // Ghidra NovaRender_SetSystemSpaceBackgroundColor / Frame_RenderViewportBackground)
  // then the active ambient star particles.
  void DrawBackground(SDL_Renderer *renderer, const GameState &state);

 private:
  // One ambient background star particle (Ghidra FadingEffectSpriteState pool
  // at g_ambient_star_particles, stride 0x14 = 20 bytes; offsets noted where
  // they map the original fields). The star's sprite-frame index (+0x06) and
  // draw-proc are not carried here; the artwork renderer is provisional.
  struct AmbientStar {
    float speed = 0.0F; // +0x08 parallax drift speed (random per particle)
    float pos_x = 0.0F; // +0x0c world-space x
    float pos_y = 0.0F; // +0x10 world-space y
    bool active = false; // +0x04 active flag
  };
  std::array<AmbientStar, 20> ambient_stars_{};
  // Provisional star-field visual size in px; the original sizes each star
  // sprite by 0x20 (=32) when the system murk (alert level) is zero, scaled
  // otherwise (Ghidra Frame_UpdateViewportWrapBackgroundSprites).
  int star_size_ = 0;
  // Unrotated per-frame ship textures plus rotation metadata.
  struct ShipSprite {
    std::vector<std::unique_ptr<class SdlTexture>> frames;
    int frame_count = 0;      // base_set_count * frames_per_rotation
    int frames_per_rotation = 36;
    int width = 0;
    int height = 0;
  };
  ShipSprite ship_;

  // One decodable spin sprite set (a stellar/planet graphic). Frames are
  // arranged in a grid of tiles_x * tiles_y; only the first frame is drawn
  // for now (TODO(decomp): advance the ambient stellar animation).
  struct SpinSpriteSet {
    std::vector<std::unique_ptr<class SdlTexture>> frames;
    int frame_count = 0;
    int width = 0;   // tiles_x
    int height = 0;  // tiles_y
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

  // Draws the current system's stellar bodies (planets/stations) at their
  // world positions relative to the player camera.
  void DrawStellarBodies(SdlPlatform &platform, const GameState &state);
};

} // namespace game
