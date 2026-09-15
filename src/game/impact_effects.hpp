#pragma once

#include "game_state.hpp"
#include "scenario_data.hpp"

namespace game {

// Ghidra Weapon_SpawnWeaponImpactParticleBurst (0x004274d0): emit `count`
// single-pixel SWParticles at (x, y). Each particle's speed is `speed` scaled
// by a random factor (100 +/- scatter)/100 when 0 < scatter < 100, its heading
// is uniform in 0..359 game degrees (0 = up, clockwise, via
// Math_AddPolarVelocity), and its lifetime is uniform in [life_base, life_max]
// when life_base < life_max. `position_scatter` (when > 0) additionally offsets
// the spawn position by a random vector of magnitude up to position_scatter.
// The port runs the burst in world pixels; internally the particle pool uses
// the original 8.8 fixed-point representation.
void NovaEffects_SpawnWeaponImpactParticleBurst(GameState &state,
                                                float x,
                                                float y,
                                                float speed,
                                                std::int16_t scatter,
                                                std::int16_t life_base,
                                                std::int16_t life_max,
                                                std::uint32_t color,
                                                std::int16_t blend_mode,
                                                std::int16_t count,
                                                std::int16_t position_scatter);

// The weapon-impact callsite shape used by Shot_ResolveShotCollisionHit
// (0x00437780), NovaUi_ResolveWeaponSplashImpact (0x00436ff0) and
// Shot_ResolveCollisions (0x00437e20) with scatter 0x14, and by the beam-hit
// queue (0x0042f270) with scatter 0x19. Life max is the original
// round(frame_base * 1.25) (double DAT_005753e0 = 1.25). No-op on a zero
// particle count.
void NovaEffects_SpawnWeaponImpactBurstForWeapon(GameState &state,
                                                 float x,
                                                 float y,
                                                 const Weapon &weapon,
                                                 std::int16_t scatter);

// Ghidra Shot_HandleShot (0x00435830), trail arm 0x00436077: emit the
// continuous point-particle trail selected from the weapon's precomputed
// speed/color variant bands. The helper performs the original Random(8)
// color-then-speed selection; the caller supplies the behind-shot anchor.
void NovaEffects_SpawnWeaponTrailParticles(GameState &state,
                                           float x,
                                           float y,
                                           const Weapon &weapon,
                                           float heading_deg,
                                           float anchor_offset_px,
                                           std::int16_t blend_mode = 0x20);

// Ghidra Shot_SpawnImpactEffectSprite (0x00421500): allocate one entry in the
// 32-slot impact-effect pool. `variant` is the delayed-start value used by the
// original child-impact scatter path.
void NovaEffects_SpawnImpactEffect(GameState &state,
                                   float x,
                                   float y,
                                   std::int16_t effect_id,
                                   std::int16_t variant = 0);

// Ghidra Shot_SpawnShipDestructionDebrisPuff (0x00428090), called from
// Ship_HandleShip (0x00433050) at the death-timer halfway point and by the
// timed-action cascade: claim the first free fading-effect slot, seed it at the
// ship position with the ship's velocity plus a randomized scatter vector
// ((10 + rand(10)) * 0.1), orient it by the resulting velocity bearing, give it
// a 150..249-frame lifetime. Ship_HandleShip queues the spatial destruction
// sound separately after this helper returns. This is the directional wreck
// debris fragment, distinct from the Explode1/Explode2 area impacts that
// Ship_UpdateVisualState spawns.
void NovaEffects_SpawnShipDestructionDebrisPuff(GameState &state,
                                                const Ship &ship);

// Advances the 32-slot directional destruction-fragment pool in normalized
// original frame-time units. The SDL view supplies the matching special ship
// sprite when it draws the pool.
void NovaEffects_TickFadingEffects(GameState &state, float elapsed_ticks);

// Ghidra Shot_SpawnAreaImpactEffects (0x004211d0): spawn the main effect and,
// for 1000+ effect ids, the two randomized child-impact bands. `radius` is
// the weapon's splash radius in world pixels.
void NovaEffects_SpawnAreaImpact(GameState &state,
                                 float x,
                                 float y,
                                 std::int16_t effect_id,
                                 std::int16_t radius,
                                 bool play_sound = true);

// Ghidra Shot_UpdateImpactEffectSprites (0x0042e160): advance animation and
// delayed-start timers. Sprite-frame-count expiry is finalized by the SDL
// view once the corresponding sp.n set has been resolved.
void NovaEffects_TickImpactEffects(GameState &state, float elapsed_ticks);

// Ghidra SWParticles_Update (0x0047c800): advance the SWParticle pool. Each
// live particle loses one life tick; while it survives its 8.8 fixed position
// is integrated by its velocity. The pool's gravity accumulator is 0 in the
// shipped game (SWParticles_AllocatePool(100000, 0)), so vel_y is unchanged.
// The original updates once per rendered frame through the present hook, whose
// enclosing flight loop is limited to one iteration per 21 ms. The port banks
// equivalent whole 21 ms logical calls. `elapsed_ticks` is in 30 Hz simulation
// ticks; fractional remainders are banked in GameState.
void NovaEffects_TickSwParticles(GameState &state, float elapsed_ticks);

} // namespace game
