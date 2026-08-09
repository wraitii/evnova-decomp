#include "ship_spawn.hpp"

#include "../log.hpp"

#include <cmath>
#include <random>

namespace game {
namespace {

// Mirrors the original's NovaRandom_Range(n) -> integer in [0, n) for the
// allocator's scatter. The reimplementation draws from GameState.rng so runs
// stay reproducible; the original uses the global NovaRandom LCG.
inline std::int16_t RandomBelow(GameState &state, std::int32_t n) {
  if (n <= 0) {
    return 0;
  }
  std::uniform_int_distribution<std::int32_t> dist{0, n - 1};
  return static_cast<std::int16_t>(dist(state.rng));
}

} // namespace

// Ghidra 0x004254b0 Ship_AllocateShipSlotInSystem.
//
// Slot scan: iterate slots 1..(0x40 - count)-1 and return the first inactive
// one; when `count` is at least 0x3F the scan body is skipped and -1 is
// returned. The `count` argument reserves a tail of the array (used by spawners
// that must keep room for e.g. the player's fleet/escorts). The player's slot
// 0 is never reallocated.
//
// On a successful allocation it marks the slot active in system_id and resets
// the full ShipState to baseline defaults. We reconstruct the load-bearing /
// gameplay-relevant subset (identity, kinematics, targeting/AI slots, mission
// slots, timers, credits and the [-750, 750) position scatter).
//
// TODO(decomp) intentional omissions (these are deep combat/AI residual fields
// that Ship_AllocateShipSlotInSystem zero/-1-resets but which are only consumed
// once the ship AI/combat systems are reconstructed; Ship's defaults already
// match a zero/-1 reset for them):
//   * the per-ship skill_variance_scale (ShipState +0x40), jamming_score_1..4,
//     combat_state_metric_a / waypoint markers / hit_reaction_timer /
//     player_aggro_accumulator / ai_turn_bias_dir and the various untyped
//     field_0x* offsets (0x60/0x64/0xac/0xb0/0xb9/0xbb-0xbd/0xc8cc/0xc8d6/
//     0xc8e4/0xc8e8/0xc8f4/0xc91e/0xc924) are all left at defaults.
//   * the random inits gated on ShipClassDef.combat_state_init_range /
//     skill_variance_percent (0x9FE/0xA00) and the derelict-government
//     engine-glow override are skipped (those ShipClassDef fields are not yet
//     loaded from scenario data).
int NovaShip_AllocateShipSlot(GameState &state,
                              std::int16_t system_id,
                              std::int16_t reserved_tail) {
  const std::int32_t free_limit =
      static_cast<std::int32_t>(GameState::kMaxShips) - reserved_tail;
  int slot = -1;
  if (1 < free_limit) {
    for (std::int32_t i = 1; i < free_limit; ++i) {
      if (!state.ShipAt(static_cast<std::size_t>(i)).is_active) {
        slot = static_cast<int>(i);
        break;
      }
    }
  }
  if (slot == -1) {
    return -1;
  }

  Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
  // Baseline reset (see header for the field subset rationale).
  ship = Ship{}; // fresh defaults: zero/-1 as Ship_AllocateShipSlotInSystem
                 // writes.
  ship.is_active = true;
  ship.current_system_id = system_id;
  ship.dude_class_id = -1;
  ship.ship_class_id = 0;
  ship.ai_behavior_code = 1;
  ship.faction_or_government_id = -1;
  ship.vel_x = 0.0F;
  ship.vel_y = 0.0F;
  ship.speed = 0.0F;
  ship.ai_state_code = 0;
  ship.ai_control_mode = 0;
  ship.mission_fleet_slot = -1;
  ship.ai_hostility_accumulator = 0;
  ship.ai_target_ship_slot = -1;
  ship.primary_target_ship_slot = -1;
  ship.ai_secondary_target_slot = -1;
  ship.death_timer_active = 0.0F;
  if (const auto *ship_class = state.scenario.Ship(
          static_cast<std::int16_t>(ship.ship_class_id + 0x80));
      ship_class != nullptr) {
    ship.timed_action_counter = ship_class->timed_action_counter_init;
  } else {
    ship.timed_action_counter = 0;
  }
  ship.mission_owner_slot = -1;
  ship.credits = 0;
  ship.target_stellar_object_id = -1;

  // Position scatter: NovaRandom_Range(0x5dc) - 0x2ee -> [-750, 750).
  ship.pos_x = static_cast<float>(RandomBelow(state, 0x5dc) - 0x2ee);
  ship.pos_y = static_cast<float>(RandomBelow(state, 0x5dc) - 0x2ee);

  return slot;
}

} // namespace game
