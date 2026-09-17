#pragma once

#include "game_state.hpp"

namespace game {

namespace mission_detail {

inline constexpr std::int16_t kResourceIdBase = 0x80;

[[nodiscard]] inline std::int16_t
ResolveContainingSystem(const GameState &state, std::int16_t stellar_id) {
  if (stellar_id < 0 ||
      stellar_id >= static_cast<std::int16_t>(state.scenario.stellars.size())) {
    return -1;
  }
  return state.scenario.stellars[static_cast<std::size_t>(stellar_id)]
      .system_id;
}

} // namespace mission_detail

} // namespace game
