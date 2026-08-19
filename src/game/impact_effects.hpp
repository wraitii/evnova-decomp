#pragma once

#include "game_state.hpp"

namespace game {

// Ghidra Shot_SpawnImpactEffectSprite (0x00421500): allocate one entry in the
// 32-slot impact-effect pool. `variant` is the delayed-start value used by the
// original child-impact scatter path.
void NovaEffects_SpawnImpactEffect(GameState &state,
                                   float x,
                                   float y,
                                   std::int16_t effect_id,
                                   std::int16_t variant = 0);

// Ship_HandleShip / Shot_SpawnShipDestructionDebrisPuff (0x00433050 ->
// 0x00428090): start the stock explosion cadence and emit the first
// directional debris fragment. Later fragments are emitted by the same
// destruction sequence rather than all being collapsed into one burst.
void NovaEffects_SpawnShipDestructionBurst(GameState &state,
                                           const Ship &ship,
                                           std::int16_t breaking_effect_id);

// Bible Explode2: the terminal fireball, with mass-scaled radius for 1000+.
void NovaEffects_SpawnShipDestructionFinale(GameState &state,
                                            const Ship &ship,
                                            std::int16_t final_effect_id);

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

// Ghidra Weapon_SpawnWeaponImpactEffectPackage (0x00462550), area-effect
// branch only. Secondary freeflight objects and SWParticles remain deferred.
void NovaEffects_SpawnImpactEffectPackage(GameState &state,
                                          float x,
                                          float y,
                                          std::int16_t package_id,
                                          bool play_sound = true);

// Ghidra Shot_UpdateImpactEffectSprites (0x0042e160): advance animation and
// delayed-start timers. Sprite-frame-count expiry is finalized by the SDL
// view once the corresponding sp.n set has been resolved.
void NovaEffects_TickImpactEffects(GameState &state, float elapsed_ticks);

} // namespace game
