#pragma once

#include "game_state.hpp"

namespace game::spaceflight_detail {

// Cross-translation-unit helpers shared by the spaceflight coordinator and
// the focused movement/player-state implementations.
[[nodiscard]] float NovaShip_IonizationIntensity(const Ship &ship,
                                                 const ShipClass &ship_class);
void TickIonizationDecay(GameState &state, Ship &ship, float elapsed_ticks);

} // namespace game::spaceflight_detail
