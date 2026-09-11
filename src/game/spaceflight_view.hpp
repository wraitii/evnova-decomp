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
#include <random>
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
class HudRenderer;

class SpaceflightView {
public:
  SpaceflightView() = default;
  SpaceflightView(const SpaceflightView &) = delete;
  SpaceflightView &operator=(const SpaceflightView &) = delete;

  // Ensures the player's ship sprite sheet (from sh\x8an BaseImageID) is
  // decoded and uploaded, and returns false if it could not be loaded. Also
  // copies the sh\x8an weapon-exit (muzzle) geometry into
  // GameState::player::muzzle_* so the firing path offsets shots to the ship's
  // gun barrels. Non-const because it mutates the player ship state.
  [[nodiscard]] bool EnsureShipSprite(SdlPlatform &platform, GameState &state);

  // Draws the whole in-flight world (solid per-system space backdrop, ambient
  // starfield, stellar bodies, shots, NPC ships in the current system, player
  // ship) into the current renderer.
  void Draw(SdlPlatform &platform, const GameState &state);

  // Keeps GameState.viewport_center_x/y in sync with the live play area (the
  // original's g_viewport_center_x/y globals, set at interface setup by
  // Ship_InitializeMainInterface 0x004ac380). Asteroid_Spawn scatters new
  // records over this half-size, so it must be current before any spawn on
  // entry, arrival or launch.
  void SyncGameplayViewport(SdlPlatform &platform, GameState &state);

  // Draws one full in-game frame: the extending world via Draw, then the HUD
  // (cockpit PICT, bars, readouts, overlay message) over it, then the
  // hyperspace fire flash. The body of spaceflight.cpp's former
  // DrawInGameFrame; modal windows that composite over the live game view
  // (the boarding/plunder window) call this instead of snapshotting pixels.
  void DrawGameFrame(SdlPlatform &platform,
                     const GameState &state,
                     HudRenderer &hud);

  // Ghidra NovaUi_UpdateShipTargetReticle (0x0042ede0): draws the 4-corner
  // bracket reticle around the player's primary target ship (state.player
  // .primary_target_ship_slot). Hidden when no target. The bracket offset is
  // ceil(max(target frame height, frame width)/2) plus the decaying reticle
  // pulse (GameState.ship_reticle_pulse; the loop decays it by frame time
  // * 1.8, the original's avg-frame-time*60 rate -- see spaceflight.cpp), with
  // the four corner-bracket sprites (cicn 10008-10023) placed at
  // the original's asymmetric positions (TL/BL get the extra 16px left margin,
  // the top row an extra 16px above); the frame index encodes target state
  // (disabled 0xc / targeting-the-player 0x8 / distress-eligible 0x0 /
  // other 0x4). Frame anchors are the cicn frames' top-left (0,0), matching
  // SpriteFrame_CreateFromRect. Falls back to the SDL-line diagnostic brackets
  // when the cicn set cannot be loaded.
  void DrawShipTargetReticle(SdlPlatform &platform, const GameState &state);

  // Ghidra NovaUi_UpdateTravelTargetReticle (0x0042eac0): draws the 4-corner
  // bracket reticle around the currently selected travel destination stellar
  // (state.travel.selected_stellar_id, shown only while a travel target is
  // engaged), using the 8-frame cicn set 10000-10007. Frame base is 0 or 4 by
  // the destination's orientation-engaged flag; sized by the stellar's sprite
  // span. Falls back to nothing (no spin art) when the cicn set is unavailable.
  void DrawTravelTargetReticle(SdlPlatform &platform, const GameState &state);

  // Ghidra Frame_UpdateFadingEffectSprites (0x0043b170): directional debris
  // fragments emitted by Shot_SpawnShipDestructionDebrisPuff (0x00428090).
  void DrawFadingEffects(SdlPlatform &platform, const GameState &state);

  // FreeflightObjectState pool (jettisoned cargo/junk pods, launched drones
  // and effect-package sprites). Simulation half is NovaFreeflight_Tick;
  // this draws the live objects' 500+index spin sets.
  void DrawFreeflightObjects(SdlPlatform &platform, const GameState &state);

  // Clean-room click-to-target ship picking: returns the slot of the active
  // NPC ship in the player's system whose sprite bounding span contains the
  // render-coordinate point (rx, ry), nearest first, or -1. Mirrors the
  // manual's "click on a ship to select it with your targeting sensors"; the
  // original performs the pick through the sprite pixel-test pass, which is
  // approximated here with the sprite half-span hit test (TODO(decomp)). No
  // government filter: any ship under the cursor may be selected for hailing.
  std::int16_t
  PickShipAt(SdlPlatform &platform, const GameState &state, float rx, float ry);

  // Clean-room click-to-target stellar picking (the stellar arm of the
  // original's mouse-target pass, PlayerTick_MouseTargetAndControlCommands
  // 0x0044e019): returns the resource id of the current system's available
  // stellar whose ambient sprite (spin set NovaTargeting_StellarSpriteLinkId
  // + 1000) contains the render-coordinate point, or -1. The original
  // hit-tests the ambient sprite's current-frame rect, expanded 16px per side
  // when shorter than 0x30, and only considers sprites whose Strength state is
  // alive; approximated here with the loaded spin set's tile extent
  // (TODO(decomp)).
  std::int16_t PickStellarAt(SdlPlatform &platform,
                             const GameState &state,
                             float rx,
                             float ry);

  // True when the render-coordinate point lands on the player's own sprite
  // (the original's self-click branch clears the primary ship target).
  // Half-extent approximation with the same 0x30/16px inset expansion.
  bool ClickInPlayerSprite(SdlPlatform &platform,
                           const GameState &state,
                           float rx,
                           float ry);

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

  // Ghidra 0x00436910 Asteroid_UpdateSprites: draws the 16-slot asteroid /
  // drift-debris pool. Each active record's wander_type selects spin resource
  // id 800+type (the Metal/Ice/Silicates/Metal-rich x size-tier asteroid sets,
  // all 50x50 36-frame tumble sheets); the displayed frame is the record's
  // wander accumulator rounded into the set's frame count. Sits above the
  // freeflight objects and below the reticles/beams, per the original scope-8
  // order (Frame_UpdateViewportWrapBackgroundSprites -> ... -> asteroids).
  void DrawAsteroids(SdlPlatform &platform, const GameState &state);

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

public:
  // Radar blip sizing reads the stellar spin sets (Sprite_GetFrameFullHeight
  // 0x00462390 on the loaded set); the HUD renderer gets read access here.
  [[nodiscard]] SpriteStore &sprite_store() { return sprite_store_; }

  // Current animation frame index for a stellar's ambient sprite (0 for
  // single-frame bodies / bodies of other systems). Read by the destination-
  // interaction window so its thumbnail shows the same frame the system view
  // does (Ghidra: the g_stellar_ambient_sprites current-frame draw in
  // NovaUi_DrawTravelDestinationInteractionWindow 0x004812c0).
  [[nodiscard]] int StellarCurrentFrame(std::int16_t stellar_id) const {
    const auto it = stellar_anims_.find(stellar_id);
    return it != stellar_anims_.end() ? it->second.current_frame : 0;
  }

private:
  // The ship-target reticle's 16-frame corner-bracket set (cicn 10008-10023)
  // and the travel reticle's 8-frame set (cicn 10000-10007), loaded on first
  // use. A failed load stays empty so a missing asset is not retried every
  // frame (the reticle drawers fall back to the debug bracket art).
  std::unique_ptr<SpriteAsset> ship_reticle_set_;
  std::unique_ptr<SpriteAsset> travel_reticle_set_;
  bool ship_reticle_tried_ = false;
  bool travel_reticle_tried_ = false;

  // Loads (and caches) the ship-target reticle set {cicn 10008..10023}.
  [[nodiscard]] const SpriteAsset *ShipReticleSet(SdlPlatform &platform);
  // Loads (and caches) the travel-target reticle set {cicn 10000..10007}.
  [[nodiscard]] const SpriteAsset *TravelReticleSet(SdlPlatform &platform);

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
  // Total sprite rows (frame_count / frames_per_rotation) after appending the
  // sh\x8an alt sheet: row 0 = straight flight, rows 1/2 = bank left/right
  // selected by ai_turn_bias_dir (Ship_UpdateVisualState 0x00428340).
  int ship_row_count_ = 1;
  std::uint16_t ship_sprite_behavior_flags_ = 0;
  // Whether this class's sh\x8an descriptor named a glow layer at all (so
  // EnsureShipSprite does not retry a missing sheet every frame).
  bool has_glow_ = false;
  // Last glow-draw gate result, so transitions (on/off) can be logged once per
  // change rather than per frame (diagnostic for the flight render).
  bool glow_last_drawn_ = false;

  // One loaded NPC ship's heading-rotation sprite data: the base hull sheet and
  // (when the class's sh\x8an descriptor names one) the engine-glow layer,
  // sharing the same rotation grid. The glow is drawn over the base with a
  // thrust-driven alpha read from the ship's engine_glow_level (driven in
  // NovaShip_IntegrateNpcMovement); a class without a glow sheet just draws
  // base-only. Cached per ship-class resource id so each distinct class in the
  // current system is decoded/uploaded once per session.
  struct NpcShipSprite {
    SpriteAsset base;
    SpriteAsset glow;      // engine-glow layer (empty when the class has none)
    bool has_glow = false; // whether a glow layer is present/loaded
    int frames_per_rotation = 36;
    // Sprite rows after the alt-sheet append + the class's sh\x8an Flags
    // (row 1/2 = bank left/right when Flags & 1).
    int row_count = 1;
    std::uint16_t sprite_behavior_flags = 0;
  };

  std::map<std::int16_t, NpcShipSprite> npc_ship_sprites_;

  // Loads (and caches) the heading-rotation sheet for a ship class resource
  // id (0x80-relative convention already applied by callers). Returns a
  // pointer to the cached entry, or null when the class/sheet cannot be
  // loaded. Non-const: mutates the cache and logically owns the SDL upload.
  const NpcShipSprite *ShipClassSprite(SdlPlatform &platform,
                                       std::int16_t ship_class_id);

  // Draws every active non-player ship in the current system at its world
  // position, frame selected by heading (player's ship is drawn separately at
  // the viewport centre). Ships whose class sprite failed to load are skipped
  // (logged once).
  void DrawNpcShips(SdlPlatform &platform, const GameState &state);

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

  // Ghidra 0x00436910 Asteroid_UpdateSprites (viewport-wrap half): repositions
  // an active asteroid that has drifted past the viewport edge back to the
  // opposite side. The original checks the placed sprite's frame edge against
  // the viewport +32px; the span is the largest loaded asteroid frame (all
  // shipped types are 50x50) because this port does not keep per-record Sprite
  // handles. Called from AdvanceAnimations after the pure drift tick.
  void WrapAsteroids(SdlPlatform &platform, GameState &state);

  // Draws the player's in-flight active shots (GameState.active_shots) at
  // their world positions relative to the ship, using the same camera
  // transform as the stars/stellars. Each shot is drawn with its weapon's
  // shot sprite set (spin resource shot_sprite_set_id + 3000), currently
  // the first frame at native size; falls back to a small bright dot when the
  // sprite set is unavailable.
  void DrawShots(SdlPlatform &platform, const GameState &state);
  // Ghidra Shot_UpdateImpactEffectSprites (0x0042e160): draws the 32-slot
  // impact animation pool above shots and beams but below ship sprites.
  void DrawImpactEffects(SdlPlatform &platform, const GameState &state);
  // Ghidra SWParticles_DrawParticles (0x0047bdd0): draws the single-pixel
  // weapon-impact / asteroid-debris particles over the ships and impact
  // sprites (the original's post-render particle pass).
  void DrawSwParticles(SdlPlatform &platform, const GameState &state);
  // Ghidra 0x00438c40 (unnamed under-ships beam pass, draw proc of the second
  // gameplay sprite-world layer): draws queued beams whose weapon sets
  // flags_secondary 0x2000 (Bible "display the beam underneath ships").
  // Below shots/ships, above the background.
  void DrawBeamsUnderShips(SdlPlatform &platform, const GameState &state);
  // Ghidra Shot_DrawBeamHitQueueForSurface (0x00438810), visible non-0x2000
  // path: the topmost gameplay layer draws every other queued beam above ships
  // and effects. The original's kinked/flare branches (and twin Shot_DrawBeam-
  // Queue 0x004386f0) are sprite-world save/restore erase machinery, replaced
  // wholesale by SDL's back buffer - TODO(decomp(0x004386f0)) skipped:
  // software surface-restore pass has no SDL equivalent.
  void DrawBeamsOverShips(SdlPlatform &platform, const GameState &state);
  // Jitter source for lightning beams (Ghidra SWBeams_DrawThickFadingBeam
  // 0x0047a410 draws rand()% per segment). Draw-phase rand() calls mutate the
  // original's global RNG stream; this port keeps that out of GameState.rng,
  // so jitter is not replay-identical - TODO(decomp) verify against original
  // draw determinism.
  std::mt19937 beam_jitter_rng_{0x424b4541U};
};

} // namespace game
