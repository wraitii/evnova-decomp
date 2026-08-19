#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace game {

struct GameState;

struct MissionListEvaluation {
  std::vector<std::int16_t> page_zero;
  std::vector<std::int16_t> page_one;
  bool has_return_mission = false;
};

// Locator warm-up and target-cache population corresponding to
// Misn_ResolveMissionStellarLocators (0x0043c3e0) and
// Mission_ResolveMissionStellarTargets (0x0043d240).
void Mission_ResolveMissionStellarLocators(GameState &state);

[[nodiscard]] MissionListEvaluation Mission_EvaluateMissionLists(
    GameState &state);

// Clean-room counterparts of the accepted-mission portions of
// Mission_PopulateMissionSlotFromDef (0x0043f8c0) and
// Mission_ActivateMissionAtSlot (0x0043f100). These functions intentionally
// stop before reaction scripts, UI refreshes, and locator selection.
[[nodiscard]] bool Mission_PopulateActiveSlot(GameState &state,
                                               std::int16_t mission_id,
                                               std::size_t active_slot);

[[nodiscard]] bool Mission_ActivateAtSlot(GameState &state,
                                           std::int16_t mission_id);

} // namespace game
