#include "ship_spawn.hpp"

#include "../log.hpp"
#include "government.hpp"

#include <cmath>
#include <random>
#include <vector>

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

// Ghidra 0x004259b0 EncounterFleet_SpawnRandomEncounterFleet -- lead-ship
// slice only. See the header for the field rationale and the deferred parts.
//
// Lead shaping decoded from the original: allocates a slot (reserve 8), then
// ship_class = def lead (already zero-based), government = def government_id,
// ai_behavior = requested code or ship-class default when -1, base shield/
// armor from the ship class, mission slots cleared, escort-eligibility and
// mining-scoop derived flags, and the timed-action seed. The original also
// seeds random cargo for carry_cargo_flag & low-default-AI fleets, positions
// the lead (spin-out / jump-in), copies the 8-bank weapon loadout and spawns
// the escorts; those are deferred and left at defaults here (see header TODO).
int NovaEncounter_SpawnFleetLeadShip(GameState &state,
                                     std::int16_t system_id,
                                     std::int16_t fleet_def_index) {
  const FleetDef *def =
      state.scenario.Fleet(static_cast<std::int16_t>(fleet_def_index + 0x80));
  if (def == nullptr) {
    return -1;
  }
  // The original no-ops when the def has no lead ship or is not currently
  // available (is_available_runtime).
  if (def->lead_ship_class_id < 0 || !def->is_available_runtime) {
    return -1;
  }

  const int slot = NovaShip_AllocateShipSlot(state, system_id, 8);
  if (slot == -1) {
    return -1;
  }

  Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
  // ShipState stores the zero-based ShipClassDef index. Resource lookups add
  // 0x80 at their boundary.
  ship.ship_class_id = def->lead_ship_class_id;
  ship.faction_or_government_id = def->government_id;

  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  // The spawned-lead flavor of the original takes an explicit ai_behavior_code
  // argument; this slice models the -1 flavor, whose effective behavior is the
  // lead ship class's default AI (the code-6 escort behavior is the escorts'
  // concern).
  ship.ai_behavior_code = cls != nullptr ? cls->default_ai_behavior : 0;

  if (cls != nullptr) {
    ship.shield_points = static_cast<float>(cls->base_shield);
    ship.armor_points = static_cast<float>(cls->base_armor);
    ship.timed_action_counter = cls->timed_action_counter_init;
  } else {
    ship.shield_points = 0.0F;
    ship.armor_points = 0.0F;
    ship.timed_action_counter = 0;
  }

  // Mission/misc slots cleared and derived flags (the original computes
  // escort-eligibility + mining-scoop here; mining-scoop is deferred, kept
  // false).
  ship.mission_ship_slot = -1;
  ship.mission_fleet_slot = -1;
  ship.mining_scoop_active = false;
  ship.ai_hostility_accumulator = 0;
  // Ship_AllocateShipSlotInSystem initializes credits to zero; the fleet
  // spawner does not override them.
  ship.credits = 0;
  ship.jump_destination_stellar_id = -2;
  ship.mission_owner_slot = -1;

  // Positioning (DEFERRED positioning nuance): the original spins the lead out
  // at a random polar offset (AI state 0x08) or jumps it in at an adjacent
  // stellar (state 0x15). Those AI-state entries are not yet reconstructed, so
  // the lead is left at the system origin with a fixed heading and a static
  // ai_state_code 0 -- visible and positioned for rendering, but not yet given
  // motion or a spawn animation. TODO(decomp).
  ship.pos_x = 0.0F;
  ship.pos_y = 0.0F;
  ship.vel_x = 0.0F;
  ship.vel_y = 0.0F;
  ship.speed = 0.0F;
  ship.heading = 0.0F;
  ship.ai_state_code = 0;

  return slot;
}

// Ghidra 0x0046b6d0 EncounterFleet_SelectRandomEncounterFleetDefWeighted.
// Weighted-random pick among the system's bound encounter-fleet defs (see the
// header). The original reads SystemDef +0x8a (count), +0x6a ids, +0x7a weights
// directly and tests each FleetDef's lead (+0x00) + is_available_runtime
// (+0x122); we use the clean-room System/FleetDef fields. The decompiled
// cumulative bucket is built as a running total over candidate weights; the
// uniform draw in [0, total) with a (draw+1) <= bucket threshold reproduces the
// canonical weighted-selection the original's unrolled stack-split accumulates.
// Note the fleet/index convention: this returns a 0-based def index (the
// original pushes the raw def id, itself 0-based after scenario load), so the
// result feeds NovaEncounter_SpawnFleetLeadShip / Fleet()+0x80 directly.
int NovaEncounter_SelectFleetDefWeighted(const System &system,
                                         const ScenarioData &scenario,
                                         std::mt19937 &rng) {
  if (system.encounter_fleet_count < 1) {
    return -1;
  }

  std::array<std::int16_t, 8> eligible_ids{};
  std::array<std::int32_t, 8> bucket{}; // cumulative weights
  std::int32_t eligible_count = 0;
  std::int32_t total_weight = 0;
  for (int i = 0; i < system.encounter_fleet_count && i < 8; ++i) {
    const std::int16_t def_index = system.encounter_fleet_ids[i];
    if (def_index < 0) {
      continue;
    }
    const FleetDef *def =
        scenario.Fleet(static_cast<std::int16_t>(def_index + 0x80));
    // The original only admits candidates with a valid lead and the runtime
    // availability bit set; a missing/out-of-range def entry is ineligible.
    if (def == nullptr || def->lead_ship_class_id < 0 ||
        !def->is_available_runtime) {
      continue;
    }
    eligible_ids[static_cast<std::size_t>(eligible_count)] = def_index;
    const std::int32_t w = system.encounter_fleet_weights[i];
    const std::int32_t prev =
        eligible_count > 0
            ? bucket[static_cast<std::size_t>(eligible_count - 1)]
            : 0;
    bucket[static_cast<std::size_t>(eligible_count)] = prev + w;
    total_weight += w;
    ++eligible_count;
  }

  if (eligible_count == 0 || total_weight <= 0) {
    return -1;
  }
  std::uniform_int_distribution<std::int32_t> dist{0, total_weight - 1};
  const std::int32_t draw = dist(rng) + 1; // draw+1 in [1, total]
  for (std::int32_t k = 0; k < eligible_count; ++k) {
    if (draw <= bucket[static_cast<std::size_t>(k)]) {
      return eligible_ids[static_cast<std::size_t>(k)];
    }
  }
  return -1;
}

// Ghidra 0x0046b600 Dude_SelectRandomSystemDudeClassIndex. See the header.
// The original unrolls the eight slots (SystemDef +0x4a class ids / +0x5a
// weights), accumulates a cumulative bucket over valid class ids (>= 0 and
// < 0x200), draws a uniform value in [0, total) via NovaRandom_Range, and its
// descending loop (which keeps overwriting the picked slot) terminates on the
// lowest-index valid slot whose bucket reaches (draw+1). We express that with
// an ascending scan over the same cumulative bucket; it selects the identical
// slot. Real weights sum far below 65536, so the original's 16-bit `total`
// accumulation never wraps.
int NovaDude_SelectRandomSystemDudeClassIndex(const System &system,
                                              std::mt19937 &rng) {
  auto is_valid_slot = [&system](std::size_t i) {
    return system.dude_class_ids[i] >= 0 && system.dude_class_ids[i] < 0x200;
  };

  std::array<std::int32_t, 8> bucket{}; // cumulative weights over valid slots
  std::int32_t total = 0;
  for (std::size_t i = 0; i < system.dude_class_ids.size(); ++i) {
    if (i > 0) {
      bucket[i] = bucket[i - 1];
    }
    if (is_valid_slot(i)) {
      bucket[i] += system.dude_class_weights[i];
      total += system.dude_class_weights[i];
    }
  }
  if (total < 1) {
    return -1;
  }

  std::uniform_int_distribution<std::int32_t> dist{0, total - 1};
  const std::int32_t draw = dist(rng) + 1; // (draw+1) in [1, total]
  for (std::size_t i = 0; i < system.dude_class_ids.size(); ++i) {
    if (is_valid_slot(i) && draw <= bucket[i]) {
      return static_cast<int>(i);
    }
  }
  // The cumulative bucket of the highest valid slot equals `total`, so the
  // draw is always covered; this is unreachable for consistent weights.
  return -1;
}

// Ghidra 0x00425280 EncounterFleet_TrySpawnRandomEncounterFleet. See the
// header for the full filter decode and the selection semantics. The original
// scans all 0x100 FleetDef slots (g_random_encounter_fleet_defs); our
// ScenarioData.fleets is likewise sized to 0x100, so the scan range matches.
int NovaEncounter_TrySpawnRandomFleet(GameState &state,
                                      std::int16_t system_id,
                                      bool /*ignore_ship_availability*/) {
  const System *sys =
      state.scenario.System(static_cast<std::int16_t>(system_id + 0x80));
  if (sys == nullptr) {
    return -1;
  }

  std::vector<bool> eligible(state.scenario.fleets.size(), false);
  std::size_t eligible_count = 0;
  for (std::size_t i = 0; i < state.scenario.fleets.size(); ++i) {
    const FleetDef &def = state.scenario.fleets[i];
    // Only defs with a valid lead and the runtime availability bit can spawn.
    if (def.lead_ship_class_id < 0 || !def.is_available_runtime) {
      continue;
    }
    const std::int16_t filter = def.spawn_system_filter;
    bool mark = false;
    if (filter == -1) {
      mark = true; // anywhere
    } else if (system_id == filter) {
      mark = true; // exact 0-based system id
    } else if (filter > 0x7f && filter < 10000 && system_id == filter - 0x80) {
      mark = true; // exact system id (stored as raw resource id)
    } else if (filter > 9999 && filter < 15000 &&
               filter - 10000 == sys->government_id) {
      mark = true; // specific government id
    } else if (filter > 14999 && filter < 20000 && sys->government_id >= 0 &&
               NovaGovernment_AreGovtsAllied(
                   state.scenario,
                   static_cast<std::int16_t>(filter - 15000),
                   sys->government_id)) {
      mark = true; // government allied to the filter's government
    } else if (filter > 19999 && filter < 25000 && sys->government_id >= 0 &&
               filter - 20000 != sys->government_id) {
      mark = true; // a different government
    } else if (filter > 24999 && filter < 30000 && sys->government_id >= 0 &&
               NovaGovernment_AreGovtsHostileOrXenophobic(
                   state.scenario,
                   static_cast<std::int16_t>(filter - 25000),
                   sys->government_id)) {
      mark = true; // hostile / xenophobic government
    }
    if (mark) {
      eligible[i] = true;
      ++eligible_count;
    }
  }

  if (eligible_count == 0) {
    return -1;
  }
  // Selection: draw uniformly over the full 0x100-def space and only spawn when
  // the drawn slot is a marked (eligible) def -- the original's effective
  // per-eligible-def odds.
  const std::int16_t idx = RandomBelow(state, 0x100);
  if (idx >= 0 && static_cast<std::size_t>(idx) < eligible.size() &&
      eligible[static_cast<std::size_t>(idx)]) {
    return NovaEncounter_SpawnFleetLeadShip(state, system_id, idx);
  }
  return -1;
}

} // namespace game
