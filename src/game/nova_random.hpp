#pragma once

// Ghidra 0x004683b0 NovaRandom_Range: pseudo-random integer in [0, bound).
// The original is a high-multiply over a global 31-bit LCG and reseeds when
// bound == 0; the port draws from the GameState mt19937 PRNG so a session is
// reproducible. Callers used to re-declare this helper per translation unit
// under a handful of names (RandomBelow / RandomRange / NovaRandomRange /
// NovaAiRandomRange / RollRandom); all of them fold into these two overloads.

#include "game_state.hpp"

#include <cstdint>
#include <random>

namespace game {

// [0, bound) from the session PRNG. bound <= 1 yields 0 (the original's
// bound == 1 high-multiply is also 0; the port additionally folds the
// reseeding bound == 0 case to a deterministic 0).
[[nodiscard]] inline std::int16_t RandomBelow(GameState &state,
                                              std::int32_t bound) {
  if (bound <= 1) {
    return 0;
  }
  return static_cast<std::int16_t>(
      std::uniform_int_distribution<std::int32_t>{0, bound - 1}(state.rng));
}

// Same roll against an explicit PRNG (dialogs take the generator directly so
// the draw site reads as a pure function of the session PRNG).
[[nodiscard]] inline std::int16_t RandomBelow(std::mt19937 &rng, int bound) {
  if (bound <= 1) {
    return 0;
  }
  return static_cast<std::int16_t>(
      std::uniform_int_distribution<int>{0, bound - 1}(rng));
}

} // namespace game
