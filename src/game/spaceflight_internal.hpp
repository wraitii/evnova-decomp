#pragma once

#include "game_state.hpp"

namespace game::spaceflight_detail {

// Cross-translation-unit helpers shared by the spaceflight coordinator and
// the focused movement/player-state implementations.
[[nodiscard]] float NovaShip_IonizationIntensity(const Ship &ship,
                                                 const ShipClass &ship_class);

// Shared ionization block of Ship_HandleShip (NPC gate 0x0043373f, decay
// 0x00434394) and PlayerTick_IonizationAndFuelRegeneration (player gate
// 0x0045073f, decay 0x00452304). When the current charge is positive it
// subtracts decay_rate * elapsed_ticks WITHOUT clamping (the original stores
// the possibly-negative result and only re-normalizes it to zero on the next
// frame's gate), then ramps each velocity axis by 0.025*elapsed_ticks toward
// the ionized cap (1 - min(intensity, 0.7)) * effective_max_speed_px_per_tick.
// The ramp is a fixed per-axis step, not a clamp: a step may overshoot the cap.
// A non-positive charge is zeroed and no ramp runs, matching the original
// pre-decay gate. `effective_max_speed_px_per_tick` must be the value of
// Ship_ComputeShipEffectiveMaxSpeed; that helper does not consult ionization
// (verified 0x004642e0), so the pre/post-decay call order is immaterial.
void NovaShip_UpdateIonizationCharge(GameState &state,
                                     Ship &ship,
                                     float effective_max_speed_px_per_tick,
                                     float elapsed_ticks);

} // namespace game::spaceflight_detail
