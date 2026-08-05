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

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

// Forward declarations of the SDL-wrapper types (defined in sdl_platform.hpp,
// global namespace) so this header can declare the view's interface without
// pulling in SDL here.
class SdlPlatform;
class SdlTexture;

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

  // Draws the whole in-flight world (starfield, stellar bodies, player ship)
  // into the current renderer.
  void Draw(SdlPlatform &platform, const GameState &state);

 private:
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
