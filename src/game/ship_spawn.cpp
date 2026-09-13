#include "ship_spawn.hpp"

#include "../log.hpp"
#include "government.hpp"
#include "mission.hpp"
#include "outfit.hpp"
#include "ship_ai.hpp"
#include "spaceflight.hpp"
#include "weapon.hpp"

#include <array>
#include <chrono>
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

// Ghidra 0x0046b870 ShipClass_ComputeShipClassSkillVarianceScale. The class
// percentage p produces one integer in [0, 2p], then maps it to
// (draw + 100 - p) * 0.01, so p=0 remains the neutral 1.0 scale.
[[nodiscard]] float SkillVarianceScale(GameState &state,
                                       const ShipClass *ship_class) {
  if (ship_class == nullptr || ship_class->skill_variance_percent <= 0) {
    return 1.0F;
  }
  const auto percent = ship_class->skill_variance_percent;
  const auto draw =
      RandomBelow(state, static_cast<std::int32_t>(percent) * 2 + 1);
  return static_cast<float>(draw + 100 - percent) * 0.01F;
}

// Ghidra _DAT_00575260: the base-speed literal the original compares against
// for ai_behavior 3 (speed-locked) dude placement in
// EncounterFleet_SpawnRandomSystemDudeShip.
constexpr float kSpeedLockedSpeed = 0.0F;

// Ghidra DAT_0057522c: the initial polar velocity added to an NPC entering
// state 0x08. The original value is a 50.0f literal. Ship.heading is stored in
// radians in the clean-room state, while the original ShipState stores degrees.
constexpr float kSpinInVelocity = 50.0F;

// Ghidra 0x0041c710 / 0x0041ba80. The original builds this radius by adding a
// 50-unit ramp while subtracting 1.16 each iteration, then adds 1000. The
// resulting 2102.64-unit polar offset is intentionally much larger than the
// ordinary [-750, 750) allocation scatter.
[[nodiscard]] float RandomPolarArrivalRadius() {
  float radius = 0.0F;
  for (float ramp = 50.0F; ramp > 0.0F; ramp -= 1.16F) {
    radius += ramp;
  }
  return radius + 1000.0F;
}

void AddArrivalSlowdownVelocity(Ship &ship) {
  ship.vel_x += std::sin(ship.heading) * kSpinInVelocity;
  ship.vel_y -= std::cos(ship.heading) * kSpinInVelocity;
}

// Ghidra Math_BearingFromPointToPoint with the clean-room heading convention
// (radians, 0 = up, +x right, +y down): heading = atan2(dx, -dy).
[[nodiscard]] float
BearingFromPointToPoint(float from_x, float from_y, float to_x, float to_y) {
  return std::atan2(to_x - from_x, -(to_y - from_y));
}

// Shared placement of the mission-fleet respawn arms of
// System_TickNpcSpawnMaintenance (0x0041d6e0): a ±256 scatter around a point
// on the shared arrival bearing at the ~2100-unit polar radius, facing back
// toward the system centre with a 50-unit inward velocity, no AI state entry,
// and the jump-destination sentinels (-2 stellar / -2 system).
void PlaceMissionFleetRespawn(GameState &state,
                              Ship &ship,
                              float bearing,
                              std::uint32_t now_ms) {
  const float radius = RandomPolarArrivalRadius();
  const float point_x = std::sin(bearing) * radius;
  const float point_y = -std::cos(bearing) * radius;
  ship.pos_x = point_x + static_cast<float>(RandomBelow(state, 0x200) - 0x100);
  ship.pos_y = point_y + static_cast<float>(RandomBelow(state, 0x200) - 0x100);
  ship.heading = std::atan2(-ship.pos_x, ship.pos_y);
  ship.jump_destination_stellar_id = -2;
  ship.jump_destination_system_id = -2;
  ship.vel_x = 0.0F;
  ship.vel_y = 0.0F;
  ship.speed = 0.0F;
  ship.ai_station_hold_timer = -999.0F;
  ship.ai_mode_start_time_ms = now_ms;
  AddArrivalSlowdownVelocity(ship);
  ship.arrival_monitor_elapsed_ticks = 0.0F;
  ship.arrival_monitor_active = true;
  ship.arrival_monitor_warning_logged = false;
  NovaLog::Info(
      "NPC mission arrival monitor armed: slot={} class={} behavior={} "
      "state={} control={} speed={:.2f} station_hold={:.2f}",
      ship.ship_instance_id,
      ship.ship_class_id,
      ship.ai_behavior_code,
      ship.ai_state_code,
      ship.ai_control_mode,
      std::hypot(ship.vel_x, ship.vel_y),
      ship.ai_station_hold_timer);
}

void PlaceRandomPolarSlowdown(GameState &state, Ship &ship) {
  const float angle =
      static_cast<float>(RandomBelow(state, 0x168)) * 0.017453292519943295F;
  const float spawn_radius = RandomPolarArrivalRadius();
  ship.pos_x = std::sin(angle) * spawn_radius;
  ship.pos_y = -std::cos(angle) * spawn_radius;
  ship.heading = std::atan2(-ship.pos_x, ship.pos_y);
  ship.vel_x = 0.0F;
  ship.vel_y = 0.0F;
  ship.speed = 0.0F;
  // heading points from the spawn point back to the system centre, so this
  // initial 50-unit velocity is inward, matching Math_AddPolarVelocity in the
  // original spawn path.
  AddArrivalSlowdownVelocity(ship);
  NovaAi_EnterState8Slowdown(state, ship);
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
//   * jamming_score_1..4, sprite_animation_timer / waypoint markers /
//     hit_reaction_timer /
//     player_aggro_accumulator / ai_turn_bias_dir and the various untyped
//     field_0x* offsets (0x60/0x64/0xac/0xb0/0xb9/0xbb-0xbd/0xc8cc) are all
//     left at defaults. The visual fields at +0xc8d6,
//     +0xc8e4/+0xc8e8/+0xc8ec, and +0xc8f4/+0xc8f6; their reset/seed behavior
//     remains deferred with the presentation subsystem.
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
  // ShipState +0x86: NPC ship identity. The original compares instance ids
  // against slot-indexed targeting fields (squad_leader_ship_slot /
  // primary_target_ship_slot) and 0 means "the player", so an NPC's instance
  // id must equal its slot. Ghidra Ship_AllocateShipSlotInSystem (0x004254b0)
  // does not write the field itself -- it relies on the per-slot identity
  // being stable across allocations; seeding it here is the equivalent
  // clean-room behaviour (the reuse of a slot keeps its identity).
  ship.ship_instance_id = static_cast<std::int16_t>(slot);
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
  ship.squad_leader_ship_slot = -1;
  ship.primary_target_ship_slot = -1;
  ship.ai_secondary_target_slot = -1;
  ship.death_timer_active = 0.0F;
  if (const auto *ship_class = state.scenario.Ship(
          static_cast<std::int16_t>(ship.ship_class_id + 0x80));
      ship_class != nullptr) {
    ship.timed_action_counter = ship_class->timed_action_counter_init;
    ship.skill_variance_scale = SkillVarianceScale(state, ship_class);
  } else {
    ship.timed_action_counter = 0;
    ship.skill_variance_scale = 1.0F;
  }
  ship.mission_owner_slot = -1;
  ship.credits = 0;
  ship.defense_fleet_home_stellar_id = -1;

  // The original's allocator draws the sprite-animation cadence seeds from
  // the ship class AS RESET (class 0), before any spawner rewrites the class
  // id and before the position scatter; spawners that care redraw from their
  // own class.
  if (const ShipClass *class0 = state.scenario.Ship(0x80); class0 != nullptr) {
    if (class0->skill_variance_percent > 0) {
      ship.sprite_animation_cycle_index =
          RandomBelow(state, class0->skill_variance_percent);
    }
    if (class0->combat_state_init_range > 0) {
      ship.sprite_animation_timer = static_cast<float>(
          RandomBelow(state, class0->combat_state_init_range));
    }
  }

  // Position scatter: NovaRandom_Range(0x5dc) - 0x2ee -> [-750,750).
  ship.pos_x = static_cast<float>(RandomBelow(state, 0x5dc) - 0x2ee);
  ship.pos_y = static_cast<float>(RandomBelow(state, 0x5dc) - 0x2ee);

  return slot;
}

// Ghidra 0x00422400 ShipClass_SpawnEscortShipFromClass.
int NovaShipClass_SpawnEscortShipFromClass(GameState &state,
                                           std::int16_t ship_class_id,
                                           std::int16_t spawn_stellar_id) {
  const std::int16_t system_id = state.player.current_system_id;
  const int slot = NovaShip_AllocateShipSlot(state, system_id, 8);
  if (slot == -1 || ship_class_id < 0) {
    return -1;
  }

  Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
  ship.ship_class_id = ship_class_id;
  ship.ai_behavior_code = 6;
  ship.squad_leader_ship_slot = 0;
  ship.faction_or_government_id = -1;
  ship.random_ai_render_cadence = 2;
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship_class_id + 0x80));
  ship.shield_points =
      static_cast<float>(cls != nullptr ? cls->base_shield : 0);
  ship.armor_points = static_cast<float>(cls != nullptr ? cls->base_armor : 0);
  ship.dude_class_id = -1;
  ship.afterburner_latch = NovaShip_CanShipUseAfterburner(state, ship) ? 1 : 0;
  ship.mining_scoop_active = NovaOutfit_HasMiningScoopOutfit(state, ship);
  // ShipState +0xBB: the hired-escort origin mark (the comm dialog's
  // "Captured Escort" status gate); the original sets it at every escort
  // spawn, hired or restored.
  ship.escort_origin_mark = 1;
  ship.mission_fleet_slot = -1;
  ship.mission_owner_slot = -1;
  ship.escort_command_code = -1;
  ship.jamming_score = {-1, -1, -1, -1};

  if (spawn_stellar_id == -1) {
    ship.pos_x = state.player.pos_x;
    ship.pos_y = state.player.pos_y;
    ship.heading = state.player.heading;
    // Math_AddPolarVelocity 0x0043b4a0 with a random bearing and a
    // 50 + rand(0x32) speed: the game convention is vel_x += sin, vel_y -= cos.
    const float speed = static_cast<float>(RandomBelow(state, 0x32) + 0x32);
    const float bearing_deg = static_cast<float>(RandomBelow(state, 0x168));
    const float bearing_rad = bearing_deg * (3.14159265358979323846F / 180.0F);
    ship.vel_x += std::sin(bearing_rad) * speed;
    ship.vel_y -= std::cos(bearing_rad) * speed;
  } else if (const Stellar *stellar = state.scenario.Stellar(spawn_stellar_id);
             stellar != nullptr) {
    ship.pos_x = static_cast<float>(stellar->pos_x);
    ship.pos_y = static_cast<float>(stellar->pos_y);
  }

  // Stock loadout: the original copies all eight 100-stride weapon-bank
  // ammo/secondary rows from the class defaults here; the clean-room builds
  // the same loadout eagerly via the shared NPC bank initializer.
  NovaWeapon_EnsureNpcWeaponBanks(state, ship);

  NovaShip_ResetAiBehaviorRuntimeFields(ship);
  NovaShip_EnterSquadReturnState(state, ship);
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
// the lead (slowdown / jump-in), copies the 8-bank weapon loadout and spawns
// the escorts; those are deferred and left at defaults here (see header TODO).
int NovaEncounter_SpawnFleetLeadShip(GameState &state,
                                     std::int16_t system_id,
                                     std::int16_t fleet_def_index,
                                     std::int16_t ai_behavior_code) {
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
  ship.ai_behavior_code = ai_behavior_code >= 0
                              ? ai_behavior_code
                              : (cls != nullptr ? cls->default_ai_behavior : 0);

  if (cls != nullptr) {
    ship.skill_variance_scale = SkillVarianceScale(state, cls);
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
  ship.pers_def_slot = -1;
  ship.mission_fleet_slot = -1;
  ship.mining_scoop_active = false;
  ship.ai_hostility_accumulator = 0;
  // Ship_AllocateShipSlotInSystem initializes credits to zero; the fleet
  // spawner does not override them.
  ship.credits = 0;
  ship.jump_destination_stellar_id = -2;
  ship.mission_owner_slot = -1;

  // Positioning: the original either spins the lead out at a random polar
  // offset (AI state 0x08) or jumps it in at an adjacent stellar (AI state
  // 0x15). The selector below chooses an eligible travel point first, then
  // accepts only a restricted stellar for emergence. The state-0x08 fallback
  // uses the original polar placement and slowdown entry.
  ship.pos_x = 0.0F;
  ship.pos_y = 0.0F;
  ship.vel_x = 0.0F;
  ship.vel_y = 0.0F;
  ship.speed = 0.0F;
  ship.heading = 0.0F;
  ship.ai_state_code = 0;

  const std::int16_t entry_stellar =
      NovaAi_SelectRandomAdjacentDestination(state, ship);
  if (entry_stellar >= 0) {
    const Stellar *stellar = state.scenario.Stellar(entry_stellar);
    ship.pos_x = static_cast<float>(stellar->pos_x);
    ship.pos_y = static_cast<float>(stellar->pos_y);
    NovaAi_EnterState15JumpOutToSystem(state, ship, entry_stellar);
  } else {
    PlaceRandomPolarSlowdown(state, ship);
  }

  return slot;
}

// Ghidra 0x0043A020 System_UpdateRandomEncounterCountdown.
void NovaSystem_UpdateReinforcementCountdown(GameState &state,
                                             float elapsed_ticks) {
  const std::int16_t system_id = state.player.current_system_id;
  if (system_id < 0 ||
      static_cast<std::size_t>(system_id) >= GameState::kMaxSystems) {
    return;
  }
  const System *system =
      state.scenario.System(static_cast<std::int16_t>(system_id + 0x80));
  if (system == nullptr || system->reinf_fleet < 0) {
    return;
  }
  const auto index = static_cast<std::size_t>(system_id);
  float &countdown = state.reinforcement_countdown[index];
  if (countdown <= 0.0F) {
    return;
  }
  countdown -= elapsed_ticks;
  if (countdown > 0.0F) {
    if (countdown < static_cast<float>(system->reinf_time) * 0.25F &&
        state.reinforcement_retrigger_delay[index] < 1) {
      // TODO(decomp(0x0043A020)) skipped: reinforcement warning overlay and
      // centered alert sound are not reproduced yet.
      NovaLog::Todo("reinforcement countdown: warning presentation not ported");
      state.reinforcement_retrigger_delay[index] =
          std::max<std::int16_t>(system->reinf_interval, 1);
    }
    return;
  }

  (void)NovaEncounter_SpawnFleetLeadShip(
      state, system_id, system->reinf_fleet, /*ai_behavior_code=*/4);
  countdown = -1.0F;
  state.reinforcement_retrigger_delay[index] =
      std::max<std::int16_t>(system->reinf_interval, 1);
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

// Ghidra 0x0046b4b0 Dude_SelectShipTypeIndexFromDudeDef. See the header.
// The original unrolls the 16 ship slots, building cumulative buckets over
// the entries whose class is present and (unless `ignore_ship_availability`)
// whose runtime availability result is non-zero, draws a uniform value in
// [0, total), and picks the lowest-index slot whose cumulative bucket first
// reaches (draw+1). Our clean-room does not yet evaluate ship-class
// availability expressions, so every present slot is a candidate; see the
// TODO(decomp) in the header.
int NovaDude_SelectShipTypeIndex(const DudeDef &dude,
                                 bool ignore_ship_availability,
                                 std::mt19937 &rng) {
  auto present = [&dude, ignore_ship_availability](std::size_t i) {
    (void)ignore_ship_availability; // availability eval deferred (see header)
    return dude.ship_types[i] >= 0 && dude.ship_types[i] < 0x200;
  };

  std::array<std::int32_t, 16> bucket{}; // cumulative weights
  std::int32_t total = 0;
  for (std::size_t i = 0; i < dude.ship_types.size(); ++i) {
    if (i > 0) {
      bucket[i] = bucket[i - 1];
    }
    if (present(i)) {
      bucket[i] += dude.ship_probabilities[i];
      total += dude.ship_probabilities[i];
    }
  }
  if (total < 1) {
    return -1;
  }

  std::uniform_int_distribution<std::int32_t> dist{0, total - 1};
  const std::int32_t draw = dist(rng) + 1; // (draw+1) in [1, total]
  for (std::size_t i = 0; i < dude.ship_types.size(); ++i) {
    if (present(i) && draw <= bucket[i]) {
      return static_cast<int>(i);
    }
  }
  return -1; // unreachable for consistent weights
}

// Ghidra 0x0041ba80 EncounterFleet_SpawnRandomSystemDudeShip. See the header.
// The original scans the first inactive ship slot and, within it, picks a
// random system-bound dude class (NovaDude_SelectRandomSystemDudeClassIndex)
// then a weighted ship type from that dude def (NovaDude_SelectShipTypeIndex),
// and lays the dude-def's identity + class stats onto the slot in place (it
// does NOT go through Ship_AllocateShipSlotInSystem; the slot was already
// found inactive). We reconstruct the load-bearing identity/kinematics/vitals
// subset; deep combat/AI residual fields and the 8-bank weapon loadout are
// deferred (see the header).
int NovaEncounter_SpawnRandomSystemDudeShip(GameState &state,
                                            std::int16_t system_id,
                                            std::uint16_t reserved_slots) {
  const std::int32_t free_limit =
      static_cast<std::int32_t>(GameState::kMaxShips) -
      static_cast<std::int32_t>(reserved_slots);
  for (std::int32_t slot = 1; slot < free_limit; ++slot) {
    Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
    if (ship.is_active) {
      continue;
    }

    const System *sys =
        state.scenario.System(static_cast<std::int16_t>(system_id + 0x80));
    if (sys == nullptr) {
      return -1;
    }
    const int dude_slot =
        NovaDude_SelectRandomSystemDudeClassIndex(*sys, state.rng);
    if (dude_slot < 0) {
      continue; // no selectable dude class for this system
    }
    const std::int16_t dude_class_id = sys->dude_class_ids[dude_slot];
    const DudeDef *dude =
        state.scenario.Dude(static_cast<std::int16_t>(dude_class_id + 0x80));
    if (dude == nullptr) {
      continue;
    }
    const int type_slot = NovaDude_SelectShipTypeIndex(
        *dude, /*ignore_ship_availability=*/false, state.rng);
    if (type_slot < 0 || type_slot >= 16) {
      return -1; // ship type selection failed -> slot released
    }

    ship.is_active = true;
    ship.current_system_id = system_id;
    ship.ship_instance_id = static_cast<std::int16_t>(slot); // identity == slot
    ship.dude_class_id = dude_class_id;
    ship.ship_class_id = dude->ship_types[type_slot];
    ship.faction_or_government_id = dude->government_id;

    const ShipClass *cls = state.scenario.Ship(
        static_cast<std::int16_t>(ship.ship_class_id + 0x80));
    if (dude->ai_type < 1) {
      ship.ai_behavior_code = cls != nullptr ? cls->default_ai_behavior : 0;
    } else {
      ship.ai_behavior_code = dude->ai_type;
    }

    // Position: ai_behavior 3 (speed-locked) ships anchor to the system's first
    // stellar when that class's base speed matches the special speed value;
    // otherwise scatter in [-750,750). The clean-room System uses nav_defs[0]
    // as the first stellar the payload's SpaceObj table points at.
    const bool speed_locked = ship.ai_behavior_code == 3 &&
                              !sys->nav_defs.empty() &&
                              sys->nav_defs[0] >= 0x80 && cls != nullptr &&
                              cls->speed == kSpeedLockedSpeed;
    if (speed_locked) {
      if (const auto *st = state.scenario.Stellar(sys->nav_defs[0]); st) {
        ship.pos_x = static_cast<float>(st->pos_x);
        ship.pos_y = static_cast<float>(st->pos_y);
      } else {
        ship.pos_x = ship.pos_y = 0.0F;
      }
    } else {
      ship.pos_x = static_cast<float>(RandomBelow(state, 0x5dc) - 0x2ee);
      ship.pos_y = static_cast<float>(RandomBelow(state, 0x5dc) - 0x2ee);
    }

    ship.vel_x = 0.0F;
    ship.vel_y = 0.0F;
    ship.speed = 0.0F;
    ship.heading =
        static_cast<float>(RandomBelow(state, 0x168)) * 0.017453292519943295F;
    ship.jump_destination_stellar_id = -2;
    ship.ai_state_code = 0;
    ship.ai_control_mode = 0;
    ship.mission_fleet_slot = -1;
    ship.mission_owner_slot = -1;
    ship.pers_def_slot = -1;
    ship.ai_hostility_accumulator = 0;
    ship.squad_leader_ship_slot = -1;
    ship.primary_target_ship_slot = -1;
    ship.ai_secondary_target_slot = -1;
    ship.death_timer_active = 0.0F;
    ship.timed_action_counter =
        cls != nullptr ? cls->timed_action_counter_init : 0;
    ship.credits = 10000;
    // The original derives mining_scoop_active from the spawned class's scoop
    // outfit (Outfit_HasMiningScoopOutfit); the clean-room leaves it false (it
    // only affects mining AI, not yet reconstructed). TODO(decomp).
    ship.mining_scoop_active = false;
    ship.defense_fleet_home_stellar_id = -1;
    if (cls != nullptr) {
      ship.skill_variance_scale = SkillVarianceScale(state, cls);
      ship.shield_points = static_cast<float>(cls->base_shield);
      ship.armor_points = static_cast<float>(cls->base_armor);
      ship.fuel_points = static_cast<float>(cls->base_fuel);
    }
    NovaLog::Debug("spawned random dude ship: slot {} class {} govt {} ai {}",
                   slot,
                   ship.ship_class_id,
                   ship.faction_or_government_id,
                   ship.ai_behavior_code);
    return slot;
  }
  return -1;
}

// Ghidra 0x004235c0 Pers_SpawnShipFromPersDef.
int NovaPers_SpawnShipFromPersDef(GameState &state,
                                  std::int16_t system_id,
                                  bool exclude_derelict_govts,
                                  std::int16_t forced_pers_slot) {
  std::array<std::uint8_t, 0x400> eligible{};
  int eligible_count = 0;

  if (forced_pers_slot < 0 || 0x3fe < forced_pers_slot) {
    // Candidate scan over slots 0..0x3fe: the original loop bound (sVar13 <
    // 0x3ff) excludes the Shareware Enforcer sentinel slot 0x3ff.
    const System *system =
        state.scenario.System(static_cast<std::int16_t>(system_id + 0x80));
    if (system == nullptr) {
      return -1;
    }
    const std::int16_t sys_govt = system->government_id;
    for (std::size_t slot = 0; slot < 0x3ff; ++slot) {
      const PersDef &def = state.scenario.pers_defs[slot];
      if (!(def.present && def.ai_behavior_code > 0 &&
            def.is_available_runtime && def.loaded_latch)) {
        continue;
      }
      bool ok = false;
      const std::int16_t filter = def.spawn_system_filter;
      if (filter == -1 || system_id == filter) {
        ok = true;
      }
      if (0x7f < filter && filter < 10000 && system_id == filter - 0x80) {
        ok = true;
      }
      // 0x270e == 9998: the government-code window excludes 9999, so a raw
      // filter of 9999 matches systems with government -1 (original quirk).
      if (0x270e < filter && filter < 15000 && filter - 10000 == sys_govt) {
        ok = true;
      }
      if (14999 < filter && filter < 20000 && sys_govt > -1 &&
          NovaGovernment_AreGovtsAllied(
              state.scenario,
              static_cast<std::int16_t>(filter - 15000),
              sys_govt)) {
        ok = true;
      }
      if (19999 < filter && filter < 25000 && sys_govt > -1 &&
          filter - 20000 != sys_govt) {
        ok = true;
      }
      if (24999 < filter && filter < 30000 && sys_govt > -1 &&
          NovaGovernment_AreGovtsHostileOrXenophobic(
              state.scenario,
              static_cast<std::int16_t>(filter - 25000),
              sys_govt)) {
        ok = true;
      }
      if (ok && exclude_derelict_govts && def.government_id > -1) {
        const Government *govt =
            state.scenario.GovernmentByIndex(def.government_id);
        if (govt != nullptr && (govt->flags_primary & 0x0800U) != 0U) {
          ok = false;
        }
      }
      if (ok && !Mission_CheckReactionConditionSatisfied(
                    state, def.availability_expression)) {
        ok = false;
      }
      if (ok) {
        eligible[slot] = 1;
        ++eligible_count;
      }
    }
  } else {
    // Forced slot: no scan, and the def is force-marked present so the spawn
    // proceeds even for an absent përs record (original quirk used by the
    // ambush and player-core call sites).
    const auto forced = static_cast<std::size_t>(forced_pers_slot);
    eligible[forced] = 1;
    state.scenario.pers_defs[forced].present = true;
    eligible_count = 1;
  }

  // Drop candidates whose personality is already active in the ship table:
  // every active non-fleet ship with a pers slot knocks out candidates with
  // the same +0x624 first-name-byte and +0x78a display-name id.
  for (std::size_t ship_slot = 1; ship_slot < GameState::kMaxShips;
       ++ship_slot) {
    const Ship &active = state.ShipAt(ship_slot);
    if (!active.is_active || active.mission_fleet_slot != -1) {
      continue;
    }
    if (active.pers_def_slot < 0 || active.pers_def_slot >= 0x400) {
      continue;
    }
    const PersDef &active_def =
        state.scenario
            .pers_defs[static_cast<std::size_t>(active.pers_def_slot)];
    const auto first_byte = [](const PersDef &def) {
      return def.display_name.empty() ? '\0' : def.display_name.front();
    };
    for (std::size_t cand = 0; cand < 0x400; ++cand) {
      if (eligible[cand] == 0) {
        continue;
      }
      const PersDef &cand_def = state.scenario.pers_defs[cand];
      if (first_byte(cand_def) == first_byte(active_def) &&
          cand_def.display_name_string_id ==
              active_def.display_name_string_id) {
        eligible[cand] = 0;
        --eligible_count;
      }
    }
  }

  if (eligible_count <= 0) {
    return -1;
  }

  std::int16_t slot = forced_pers_slot;
  if (slot == -1) {
    // The original random range is 0x3fe, so slot 0x3fe is only reachable by
    // a forced call.
    slot = RandomBelow(state, 0x3fe);
  }
  if (slot < 0 || slot >= 0x400 ||
      eligible[static_cast<std::size_t>(slot)] == 0) {
    return -1;
  }
  const PersDef &def = state.scenario.pers_defs[static_cast<std::size_t>(slot)];

  const int alloc = NovaShip_AllocateShipSlot(state, system_id, 8);
  if (alloc < 0) {
    return -1;
  }
  Ship &ship = state.ShipAt(static_cast<std::size_t>(alloc));
  ship.pers_def_slot = slot;
  ship.ship_class_id = def.ship_class_id;
  ship.dude_class_id = -1;
  ship.faction_or_government_id = def.government_id;
  ship.ai_behavior_code = def.ai_behavior_code;
  // Aggress seeds the per-ship cadence slot (+0xC8CC), clamped 1..2 (larger
  // values collapse to 4).
  ship.random_ai_render_cadence = def.aggression_level;
  ship.comm_interacted_mark = 0; // +0xBC
  ship.afterburner_latch = NovaShip_CanShipUseAfterburner(state, ship) ? 1 : 0;
  ship.mining_scoop_active = NovaOutfit_HasMiningScoopOutfit(state, ship);
  ship.escort_command_code = -1;
  // Bible përs Flags 0x0002: escape pod & afterburner.
  if ((def.flags_primary & 0x0002U) != 0U) {
    ship.afterburner_latch = 1;
  }
  if (ship.random_ai_render_cadence < 1) {
    ship.random_ai_render_cadence = 1;
  }
  if (ship.random_ai_render_cadence > 2) {
    ship.random_ai_render_cadence = 4;
  }

  // Weapon banks: class stock loadout, then the përs per-weapon count/ammo
  // deltas (indexed by weapon id - 0x80).
  NovaWeapon_EnsureNpcWeaponBanks(state, ship);
  for (std::size_t bank = 0; bank < 0x100; ++bank) {
    ship.npc_weapon_bank_ammo[bank] = static_cast<std::int16_t>(
        ship.npc_weapon_bank_ammo[bank] + def.weapon_count_delta[bank]);
    ship.npc_weapon_bank_secondary[bank] =
        static_cast<std::int16_t>(ship.npc_weapon_bank_secondary[bank] +
                                  def.weapon_ammo_load_delta[bank]);
  }

  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  ship.shield_points =
      cls != nullptr ? static_cast<float>(cls->base_shield) : 0.0F;
  ship.armor_points =
      cls != nullptr ? static_cast<float>(cls->base_armor) : 0.0F;
  // Bible përs Flags2 0x0001: starts with zero fuel.
  if ((def.flags_secondary & 0x0001U) == 0U && cls != nullptr) {
    ship.fuel_points = static_cast<float>(cls->base_fuel);
  } else {
    ship.fuel_points = 0.0F;
  }
  // ShieldMod (percent / 100 at decode): positive scales both pools.
  if (def.shield_armor_scale > 0.0F) {
    ship.shield_points *= def.shield_armor_scale;
    ship.armor_points *= def.shield_armor_scale;
  }

  bool is_derelict = false;
  if (ship.faction_or_government_id != -1) {
    const Government *govt =
        state.scenario.GovernmentByIndex(ship.faction_or_government_id);
    is_derelict = govt != nullptr && (govt->flags_primary & 0x0800U) != 0U;
  }
  if (is_derelict && cls != nullptr) {
    // Derelict government: no shields, reduced armor (Bible shïp Flags
    // 0x0010: disabled at 10% armor instead of 33%), dead in space.
    ship.shield_points = 0.0F;
    const float armor_scale =
        (cls->capability_flags & 0x10U) != 0U ? 0.1F : 0.32F;
    ship.armor_points =
        static_cast<float>(cls->base_armor) * armor_scale - 1.0F;
    ship.vel_y = 0.0F;
    ship.vel_x = ship.vel_y;
    ship.speed = 0.0F;
  }

  NovaShip_ResetAiBehaviorRuntimeFields(ship);

  if (def.link_mission_id != -1) {
    // Ghidra calls Mission_ResolveMissionStellarTargets (0x0043d240) for the
    // LinkMission target block; that resolver is not reconstructed yet.
    NovaLog::Debug("pers slot {} LinkMission {}: ResolveMissionStellarTargets "
                   "deferred (TODO(decomp 0x0043d240))",
                   slot,
                   def.link_mission_id);
  }

  if (is_derelict) {
    // Derelict wrecks get a random spin-out heading (original stores raw
    // degrees 0..0x167; the clean-room heading is radians) and no engine glow.
    ship.heading =
        static_cast<float>(RandomBelow(state, 0x168)) * 0.017453292519943295F;
    ship.vel_x = 0.0F;
    ship.speed = 0.0F;
    ship.engine_glow_level = 0;
    if ((cls->sprite_behavior_flags & 2U) != 0U) {
      ship.waypoint_arrival_marker_b = cls->skill_variance_percent - 1;
    }
  }

  return alloc;
}

// Ghidra 0x0041c710 Dude_SpawnRandomDudeShipInSystem. See the header.
// Rolls 1-in-7 for a mission ship (deferred), else 1-in-7 for a random-
// encounter fleet (existing NovaEncounter_TrySpawnRandomFleet), else spawns a
// random system dude ship (discarded when its fuel capacity < 1), then
// positions the spawned ship at a random polar offset from system centre and
// faces it toward the origin. The AI-state entry (slowdown / jump-in) and the
// speed polar integration mirror Math_AddPolarVelocity (heading 0 = up, world
// +y down).
int NovaDude_SpawnRandomDudeShipInSystem(GameState &state,
                                         std::int16_t system_id) {
  int slot = -1;
  // 1-in-7 personality branch: Pers_SpawnShipFromPersDef with the derelict-
  // government exclusion (the ambient roll passes flag=1).
  constexpr std::int32_t kDispatchRoll = 7;
  if (RandomBelow(state, kDispatchRoll) == 0) {
    slot = NovaPers_SpawnShipFromPersDef(
        state, system_id, /*exclude_derelict_govts=*/true, -1);
  } else if (RandomBelow(state, kDispatchRoll) == 0) {
    (void)NovaEncounter_TrySpawnRandomFleet(state,
                                            system_id,
                                            /*ignore_ship_availability=*/true);
  } else {
    slot = NovaEncounter_SpawnRandomSystemDudeShip(state, system_id, 8);
    if (slot >= 0) {
      Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
      // Discard ships whose computed fuel capacity < 1 (the original's
      // Ship_ComputeShipFuelCapacity). The clean-room NPC fuel is the class
      // base_fuel; outfit-derived fuel bonuses (opcode-12 outfits) are not
      // typically present on dude ships, so this is faithful for the stock
      // scenario. TODO(decomp).
      const ShipClass *cls = state.scenario.Ship(
          static_cast<std::int16_t>(ship.ship_class_id + 0x80));
      const std::int16_t fuel = cls != nullptr ? cls->base_fuel : 0;
      if (fuel < 1) {
        ship.is_active = false;
        return -1;
      }
    }
  }
  if (slot < 0) {
    return -1;
  }

  Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
  // Random polar offset from system centre, magnitude ~the spawn scale
  // (_DAT_0057522c + ramp + _DAT_0057526c). The original picks a radius and
  // adds it in the heading direction: pos += (sin(h), -cos(h)) * speed.
  const float heading =
      static_cast<float>(RandomBelow(state, 0x168)) * 0.017453292519943295F;
  const float spawn_radius = RandomPolarArrivalRadius();
  ship.pos_x = std::sin(heading) * spawn_radius;
  ship.pos_y = -std::cos(heading) * spawn_radius;
  // Face the spawned ship toward the origin (bearing from its position back to
  // system centre). The original uses Math_BearingFromPointToPoint; this uses
  // the same heading convention as the rest of the codebase (heading =
  // atan2(vx, -vy), heading 0 = up).
  ship.heading = std::atan2(-ship.pos_x, ship.pos_y);
  ship.vel_x = 0.0F;
  ship.vel_y = 0.0F;
  ship.speed = 0.0F;
  ship.jump_destination_stellar_id = -2;
  ship.ai_state_code = 0;
  const std::int16_t entry_stellar =
      NovaAi_SelectRandomAdjacentDestination(state, ship);
  if (entry_stellar >= 0) {
    const Stellar *stellar = state.scenario.Stellar(entry_stellar);
    ship.pos_x = static_cast<float>(stellar->pos_x);
    ship.pos_y = static_cast<float>(stellar->pos_y);
    NovaAi_EnterState15JumpOutToSystem(state, ship, entry_stellar);
  } else {
    AddArrivalSlowdownVelocity(ship);
    NovaAi_EnterState8Slowdown(state, ship);
  }
  NovaLog::Info("dude ship placed at ({}, {}) heading {:.2f}",
                ship.pos_x,
                ship.pos_y,
                ship.heading);
  return slot;
}

// Ghidra 0x0041c9f0 Dude_SpawnShipFromDudeDefInSystem. See the header. The
// original also copies the class's per-weapon 0x100-entry ammo/secondary
// tables into the ship state here; the clean-room builds them lazily on the
// first AI/fire tick (NovaWeapon_EnsureNpcWeaponBanks, keyed by class id),
// which yields the same loadout.
int NovaDude_SpawnShipFromDudeDefInSystem(GameState &state,
                                          std::int16_t dude_def_index,
                                          std::int16_t system_id,
                                          std::int16_t slot_pool,
                                          bool ignore_ship_availability) {
  const int slot = NovaShip_AllocateShipSlot(state, system_id, slot_pool);
  if (slot < 0) {
    return -1;
  }
  const DudeDef *dude =
      state.scenario.Dude(static_cast<std::int16_t>(dude_def_index + 0x80));
  const int type_slot = dude ? NovaDude_SelectShipTypeIndex(
                                   *dude, ignore_ship_availability, state.rng)
                             : -1;
  if (dude == nullptr || type_slot < 0 || type_slot >= 16) {
    state.ShipAt(static_cast<std::size_t>(slot)).is_active = false;
    return -1;
  }

  Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
  ship.dude_class_id = dude_def_index;
  ship.ship_class_id = dude->ship_types[type_slot];
  ship.faction_or_government_id = dude->government_id;
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  ship.ai_behavior_code = dude->ai_type < 1
                              ? (cls != nullptr ? cls->default_ai_behavior : 0)
                              : dude->ai_type;
  NovaShip_ResetAiBehaviorRuntimeFields(ship);
  if (cls != nullptr) {
    ship.shield_points = static_cast<float>(cls->base_shield);
    ship.armor_points = static_cast<float>(cls->base_armor);
  }
  return slot;
}

// Ghidra 0x00421fd0 Stellar_SpawnDefenseFleetShip. Spawns one ship for a
// stellar's Bible defense fleet (spöb DefenseDude): allocate via
// Dude_SpawnShipFromDudeDefInSystem (slot pool 2, retried with ship
// availability ignored), stamp it as the stellar's defender
// (defense_fleet_home_stellar_id), force behavior-3 warship, seed it at the
// stellar's map position with a random heading and an initial velocity at the
// effective max speed along that heading, and finally make it hostile to the
// player. Sets the stellar's field_0x47 latch so the per-tick
// trickle (NovaSystem_TickNpcSpawnMaintenance) may replace losses. `stellar_id`
// is the stellar resource id; returns the ship slot or -1.
int NovaStellar_SpawnDefenseFleetShip(GameState &state,
                                      std::int16_t stellar_id) {
  if (stellar_id < 0x80 || static_cast<std::size_t>(stellar_id - 0x80) >=
                               state.scenario.stellars.size()) {
    return -1;
  }
  Stellar &stellar =
      state.scenario.stellars[static_cast<std::size_t>(stellar_id - 0x80)];
  if (stellar.defense_dude_id == -1) {
    return -1;
  }

  int slot =
      NovaDude_SpawnShipFromDudeDefInSystem(state,
                                            stellar.defense_dude_id,
                                            stellar.system_id,
                                            /*slot_pool=*/2,
                                            /*ignore_ship_availability=*/false);
  if (slot < 0) {
    slot = NovaDude_SpawnShipFromDudeDefInSystem(
        state,
        stellar.defense_dude_id,
        stellar.system_id,
        /*slot_pool=*/2,
        /*ignore_ship_availability=*/true);
  }
  if (slot < 0) {
    return -1;
  }

  Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
  ship.defense_fleet_home_stellar_id = stellar_id;
  ship.boarded_target_latch = 1;
  ship.cloak_transition_latch = 0;
  ship.cloak_fade_progress = 0.0F;
  ship.ai_maneuver_timer_ms = 0.0F;
  ship.ai_behavior_code = 3;
  ship.mission_fleet_slot = -1;
  ship.squad_leader_ship_slot = -1;
  ship.faction_or_government_id = stellar.government_id;
  ship.pos_x = static_cast<float>(stellar.pos_x);
  ship.pos_y = static_cast<float>(stellar.pos_y);
  // Random integer heading in [0, 360) degrees; the clean-room heading field is
  // stored in radians, so convert once. vel_x/vel_y/speed are then seeded by
  // Math_AddPolarVelocity (0x0043b4a0) at the effective max speed (heading 0 =
  // up: vel_x += sin, vel_y -= cos).
  ship.heading = static_cast<float>(RandomBelow(state, 0x168)) *
                 (3.14159265358979323846F / 180.0F);
  ship.vel_x = 0.0F;
  ship.vel_y = 0.0F;
  ship.speed = 0.0F;
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  if (cls != nullptr) {
    const NpcEffectiveStats stats =
        NovaShip_ComputeEffectiveStats(state, ship, *cls);
    ship.vel_x += std::sin(ship.heading) * stats.max_speed_px_per_tick;
    ship.vel_y -= std::cos(ship.heading) * stats.max_speed_px_per_tick;
  }
  NovaAi_SetShipHostileToPlayer(state, ship);
  stellar.field_0x47 = 1;
  return slot;
}

// Ghidra 0x0041cf40 Mission_SpawnMissionShipFromDudeDef. See the header. The
// original bounds the ship-type index with `< 0x11`, which would read one
// entry past the 16-slot ship_types table; that is unreachable in practice
// (every producer stores 0..15), so the clean-room bounds at 16.
// TODO(decomp) sprite_animation_timer seed from ShipClassDef
// .combat_state_init_range (+0xa02): the clean-room ShipClass does not load
// that field yet.
int NovaMission_SpawnMissionShipFromDudeDef(GameState &state,
                                            std::int16_t dude_class_id,
                                            std::int16_t forced_ship_class_id,
                                            std::int16_t spawn_system_id,
                                            std::int16_t mission_fleet_slot) {
  const int slot = NovaShip_AllocateShipSlot(state, spawn_system_id, 8);
  if (slot < 0) {
    return -1;
  }
  // Cadence draw: the original draws NovaRandom_Range(100) here and ignores
  // the result.
  (void)RandomBelow(state, 100);

  Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
  const auto deactivate = [&ship]() {
    ship.is_active = false;
    return -1;
  };
  if (mission_fleet_slot < 0 ||
      mission_fleet_slot >=
          static_cast<std::int16_t>(state.active_missions.size())) {
    // The original indexes g_active_misn unchecked; the clean-room guards.
    NovaLog::Warn("mission ship spawn with out-of-range fleet slot {}",
                  mission_fleet_slot);
    return deactivate();
  }
  ActiveMission &mission = state.active_missions[mission_fleet_slot];
  const DudeDef *dude =
      state.scenario.Dude(static_cast<std::int16_t>(dude_class_id + 0x80));
  if (dude == nullptr) {
    return deactivate();
  }

  // Forced-class lock: single-ship fleets flagged 0x0800 pin the fleet's
  // special-ship-type index to the forced class (used by the hailed-escort
  // respawn, Ship_HandlePlayerTargetActionCommand 0x00454910).
  if (forced_ship_class_id != -1 && (mission.flags_primary & 0x0800U) != 0U &&
      mission.target_ship_count == 1) {
    for (std::size_t i = 0; i < dude->ship_types.size(); ++i) {
      if (forced_ship_class_id == dude->ship_types[i]) {
        mission.special_ship_type_index = static_cast<std::int16_t>(i);
        break;
      }
    }
  }

  std::int16_t type_index = mission.special_ship_type_index;
  if (type_index == -1) {
    type_index = static_cast<std::int16_t>(NovaDude_SelectShipTypeIndex(
        *dude, /*ignore_ship_availability=*/false, state.rng));
    if (type_index == -1) {
      type_index = static_cast<std::int16_t>(
          NovaDude_SelectShipTypeIndex(*dude,
                                       /*ignore_ship_availability=*/true,
                                       state.rng));
    }
  }
  if (type_index < 0 || type_index >= 16) {
    return deactivate();
  }

  ship.mission_fleet_slot = mission_fleet_slot;
  ship.dude_class_id = dude_class_id;
  ship.ship_class_id = dude->ship_types[type_index];
  ship.faction_or_government_id = dude->government_id;
  ship.boarded_target_latch = 0;
  ship.post_hit_mode_hint = -1;
  ship.cloak_transition_latch = 0;
  ship.cloak_fade_progress = 0.0F;
  ship.ai_maneuver_timer_ms = 0.0F;
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  ship.ai_behavior_code = dude->ai_type < 1
                              ? (cls != nullptr ? cls->default_ai_behavior : 0)
                              : dude->ai_type;
  NovaShip_ResetAiBehaviorRuntimeFields(ship);
  // Weapon loadout: see NovaDude_SpawnShipFromDudeDefInSystem — the
  // clean-room copies the class stock loadout lazily.
  if (cls != nullptr) {
    ship.shield_points = static_cast<float>(cls->base_shield);
    ship.armor_points = static_cast<float>(cls->base_armor);
  }
  ship.timed_action_counter =
      cls != nullptr ? cls->timed_action_counter_init : -1;
  ship.waypoint_arrival_marker_b = static_cast<std::int16_t>(
      (cls != nullptr ? cls->skill_variance_percent : 0) - 1);
  ship.skill_variance_scale = SkillVarianceScale(state, cls);
  if (cls != nullptr && cls->skill_variance_percent > 0) {
    ship.sprite_animation_cycle_index =
        RandomBelow(state, cls->skill_variance_percent);
  }
  if (mission.fleet_spawn_goal == 0) {
    // ShipBehav 0: the fleet spawns hostile to the player.
    NovaAi_SetShipHostileToPlayer(state, ship);
  }
  return slot;
}

// Ghidra mission-fleet restore slice of 0x0041af90
// System_RebuildInitialNpcAndMissionPopulation. See the header.
void NovaSystem_RestoreMissionFleets(GameState &state,
                                     std::int16_t system_id,
                                     bool copy_player_heading,
                                     std::uint32_t now_ms) {
  const System *sys =
      state.scenario.System(static_cast<std::int16_t>(system_id + 0x80));
  for (std::int16_t slot = 0;
       slot < static_cast<std::int16_t>(state.active_missions.size());
       ++slot) {
    if (!state.active_mission_runtime_flags[slot].is_active) {
      continue;
    }
    ActiveMission &mission = state.active_missions[slot];
    const std::int16_t raw_system = mission.current_system_id;
    const std::int16_t resolved =
        (raw_system < 0 || raw_system >= 0x800)
            ? static_cast<std::int16_t>(-1)
            : Misn_ResolveVisibleSystemForTravel(state, raw_system);
    if (resolved != system_id && raw_system != -6) {
      continue;
    }
    std::int16_t count = mission.target_ship_count;
    if (count <= 0 || mission.spawn_rearm_timer >= 0) {
      continue;
    }
    // Follow-player escort fleets keep the ships already escorting across
    // system re-entry and only spawn the difference.
    if (raw_system == -6 && mission.fleet_spawn_goal == 1) {
      for (std::size_t i = 1; i < GameState::kMaxShips; ++i) {
        const Ship &ship = state.ShipAt(i);
        if (ship.is_active && ship.mission_fleet_slot == slot) {
          --count;
        }
      }
    }
    if (count <= 0) {
      continue;
    }

    for (std::int16_t i = 0; i < count; ++i) {
      const int spawned =
          NovaMission_SpawnMissionShipFromDudeDef(state,
                                                  mission.dude_def_index,
                                                  /*forced_ship_class_id=*/-1,
                                                  system_id,
                                                  slot);
      if (spawned < 0) {
        // The original prints a three-part debug string here.
        NovaLog::Debug(
            "mission fleet {} spawn failed in system {}", slot, system_id);
        continue;
      }
      Ship &ship = state.ShipAt(static_cast<std::size_t>(spawned));
      if (mission.spawn_behavior == 3) {
        ship.pos_x = static_cast<float>(RandomBelow(state, 0x200) - 0x100);
        ship.pos_y = static_cast<float>(RandomBelow(state, 0x200) - 0x100);
      }
      if (mission.spawn_behavior == 5) {
        // Derelict wreck: dead in space, shields gone, armor cut to 33%
        // (10% for capability-flags 0x10 classes). Rescue missions with a
        // live (non -32000) deadline latch +0xB9 on the wreck.
        ship.heading = static_cast<float>(RandomBelow(state, 0x168)) *
                       0.017453292519943295F;
        ship.vel_y = 0.0F;
        ship.vel_x = 0.0F;
        ship.speed = 0.0F;
        ship.shield_points = 0.0F;
        const ShipClass *cls = state.scenario.Ship(
            static_cast<std::int16_t>(ship.ship_class_id + 0x80));
        if (cls != nullptr) {
          // The original's _DAT_00575230 subtrahend is 0.0.
          const float armor_factor =
              (cls->capability_flags & 0x10U) != 0U ? 0.1F : 0.33F;
          ship.armor_points =
              static_cast<float>(cls->base_armor) * armor_factor;
        }
        if (mission.time_limit_days_remaining < 1 &&
            mission.time_limit_days_remaining > -32000) {
          ship.boarded_target_latch = 1;
        }
      }
      const std::int16_t mode = mission.special_ship_spawn_mode;
      if (mode <= -1 && mode >= -16 && sys != nullptr) {
        // Negative ShipStart: the fleet arrives at the Nth linked system's
        // nav point (jump-in from a neighbour).
        const std::int16_t nav = sys->nav_defs[-1 - mode];
        if (nav >= 0x80) {
          if (const Stellar *stellar =
                  state.scenario.Stellar(static_cast<std::int16_t>(nav - 0x80));
              stellar != nullptr) {
            ship.pos_x = static_cast<float>(stellar->pos_x);
            ship.pos_y = static_cast<float>(stellar->pos_y);
          }
        }
      }
      if (mission.fleet_spawn_goal == 1) {
        // ShipBehav 1: escort-formation link on the player.
        ship.ai_behavior_code = 6;
        ship.squad_leader_ship_slot = 0;
        ship.resolved_squad_leader_ship_slot = 0;
        ship.formation_leader_ship_slot = 0;
        if (copy_player_heading) {
          ship.heading = state.player.heading;
        }
      }
      ship.jump_destination_stellar_id = -2;
      if (mission.special_ship_spawn_mode == 2) {
        NovaAi_OnShipCloakStateEntered(state, ship);
      }
    }
    if ((mission.flags_primary & 0x0001U) != 0U) {
      Mission_ResolveMisnSlot(state, slot, now_ms);
    }
  }
}

// Ghidra 0x0041af90 System_RebuildInitialNpcAndMissionPopulation, initial
// ambient-population slice. The larger function first restores mission fleets
// and player escorts,
// then performs exactly avg_ships attempts here. Unlike per-tick maintenance,
// its ordinary-dude branch deliberately calls the low-level spawner directly,
// so those ships keep their inner-system [-750,750) scatter instead of being
// moved to the polar state-8 / restricted-stellar state-15 arrival paths.
void NovaSystem_PopulateInitialNpcShips(GameState &state,
                                        std::int16_t system_id) {
  const System *sys =
      state.scenario.System(static_cast<std::int16_t>(system_id + 0x80));
  if (sys == nullptr || sys->avg_ships <= 0) {
    return;
  }

  constexpr std::int32_t kDispatchRoll = 7;
  for (std::int16_t attempt = 0; attempt < sys->avg_ships; ++attempt) {
    int slot = -1;
    if (RandomBelow(state, kDispatchRoll) == 0) {
      // 1-in-7 personality branch (random arm): Pers_SpawnShipFromPersDef
      // without the derelict exclusion. The original's forced arm (the per-
      // system special-personality table at SystemDef +0x98) remains deferred
      // (TODO(decomp): System pers-slot decode). The spawned personality is
      // left at the allocator scatter; only the ordinary dude branch adds the
      // class base-velocity step. Derelict personalities are dead in space.
      const int pers_slot = NovaPers_SpawnShipFromPersDef(
          state, system_id, /*exclude_derelict_govts=*/false, -1);
      if (pers_slot >= 0) {
        Ship &pers = state.ShipAt(static_cast<std::size_t>(pers_slot));
        const Government *govt =
            state.scenario.GovernmentByIndex(pers.faction_or_government_id);
        if (govt != nullptr && (govt->flags_primary & 0x0800U) != 0U) {
          pers.vel_x = 0.0F;
          pers.vel_y = 0.0F;
          pers.speed = 0.0F;
        }
      }
      continue;
    }
    if (RandomBelow(state, kDispatchRoll) == 0) {
      // EncounterFleet_TrySpawnRandomEncounterFleet owns its lead/escort
      // placement; the original does not apply the base-velocity step below to
      // this branch because it does not return the spawned slot to this caller.
      (void)NovaEncounter_TrySpawnRandomFleet(
          state, system_id, /*ignore_ship_availability=*/false);
      continue;
    }

    slot = NovaEncounter_SpawnRandomSystemDudeShip(state, system_id, 8);
    if (slot < 0) {
      continue;
    }

    Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
    const ShipClass *cls = state.scenario.Ship(
        static_cast<std::int16_t>(ship.ship_class_id + 0x80));
    if (cls == nullptr) {
      continue;
    }
    // The clean-room keeps the Bible's raw speed scale (100 units per
    // px/reference-tick), whereas g_ship_class_defs contains the runtime float
    // used by Math_AddPolarVelocityWithClamp in the original.
    const float base_speed = static_cast<float>(cls->speed) / 100.0F;
    ship.vel_x = std::sin(ship.heading) * base_speed;
    ship.vel_y = -std::cos(ship.heading) * base_speed;
  }
}

// Ghidra 0x0041d6e0 System_TickNpcSpawnMaintenance (ambience slice). See the
// header. The original counts the ambient active ships in the system whose
// squad_leader_ship_slot != 0 (ships not actively engaged on the player), then
// below the AvgShips cap rolls a 1-in-500 encounter pick (gated by
// encounter_chance_percent) and otherwise spawns a random dude ship. Tichel and
// most ordinary systems bind no encounter fleets, so the dude spawn is the
// dominant population path.
void NovaSystem_TickNpcSpawnMaintenance(GameState &state,
                                        std::int16_t system_id,
                                        std::uint32_t now_ms) {
  // Mission-fleet respawn stepper (0x0041d6e0's 16-slot mission slice).
  for (std::int16_t slot = 0;
       slot < static_cast<std::int16_t>(state.active_missions.size());
       ++slot) {
    if (!state.active_mission_runtime_flags[slot].is_active) {
      continue;
    }
    ActiveMission &mission = state.active_missions[slot];

    // Aux-fleet arm: once the acceptance roll clock has expired, top the aux
    // dude fleet up to its remaining spawn budget when the mission's spawn
    // locator matches this system. Flags 0x0010 fleets never drain their
    // budget, so dead aux ships keep getting replaced.
    if (mission.rearm_roll_clock < 1 &&
        mission.aux_ships_dude_def_index != -1 &&
        mission.mission_fleet_metric_c < mission.mission_ship_count_active) {
      const std::int16_t deficit = static_cast<std::int16_t>(
          mission.mission_ship_count_active - mission.mission_fleet_metric_c);
      if (deficit > 0 &&
          Mission_DoesSystemMatchMissionLocator(state, system_id, slot)) {
        const float bearing = static_cast<float>(RandomBelow(state, 0x168)) *
                              0.017453292519943295F;
        for (std::int16_t i = 0; i < deficit; ++i) {
          const int spawned = NovaDude_SpawnShipFromDudeDefInSystem(
              state,
              mission.aux_ships_dude_def_index,
              system_id,
              /*slot_pool=*/8,
              /*ignore_ship_availability=*/false);
          if (spawned < 0) {
            continue;
          }
          Ship &ship = state.ShipAt(static_cast<std::size_t>(spawned));
          ship.mission_owner_slot = slot;
          ++mission.mission_fleet_metric_c;
          if ((mission.flags_primary & 0x0010U) == 0U) {
            --mission.mission_ship_count_active;
          }
          PlaceMissionFleetRespawn(state, ship, bearing, now_ms);
        }
        if ((mission.flags_primary & 0x0001U) != 0U) {
          Mission_ResolveMisnSlot(state, slot, now_ms);
        }
      }
    }

    // Main-fleet arm: missions whose spawn system matches (or -6 follow)
    // count down their spawn/rearm timer; when it expires, respawn the fleet
    // toward its target count. special_ship_spawn_mode 1 keeps the alive
    // count at target via goal_count_remaining; other modes fire once (the
    // timer is only re-armed by Misn_TickActiveMissionTimers).
    const std::int16_t raw_system = mission.current_system_id;
    const std::int16_t resolved =
        (raw_system < 0 || raw_system >= 0x800)
            ? static_cast<std::int16_t>(-1)
            : Misn_ResolveVisibleSystemForTravel(state, raw_system);
    const bool in_mission_system = raw_system == -6 || resolved == system_id;
    std::int16_t needed = in_mission_system ? mission.target_ship_count : 0;
    if (mission.special_ship_spawn_mode == 1) {
      needed = static_cast<std::int16_t>(mission.target_ship_count -
                                         mission.goal_count_remaining);
      if (!in_mission_system && mission.goal_count_remaining < 1) {
        needed = 0;
      }
    }
    if (needed > 0) {
      if (mission.spawn_rearm_timer > 0 && mission.spawn_rearm_timer < 0x7d01) {
        --mission.spawn_rearm_timer;
      }
      if (mission.target_ship_count > 0 && mission.spawn_rearm_timer == 0) {
        mission.spawn_rearm_timer = -1;
        // The fleet arrives from the direction of the player's previous
        // system (ShipState +0x94); a random bearing when the player has
        // none.
        float bearing = 0.0F;
        if (state.player.jump_destination_system_id == -1) {
          bearing = static_cast<float>(RandomBelow(state, 0x168)) *
                    0.017453292519943295F;
        } else {
          const System *from = state.scenario.System(
              static_cast<std::int16_t>(system_id + 0x80));
          const System *to = state.scenario.System(static_cast<std::int16_t>(
              state.player.jump_destination_system_id + 0x80));
          if (from != nullptr && to != nullptr) {
            bearing = BearingFromPointToPoint(static_cast<float>(from->pos_x),
                                              static_cast<float>(from->pos_y),
                                              static_cast<float>(to->pos_x),
                                              static_cast<float>(to->pos_y));
          }
        }
        for (std::int16_t i = 0; i < needed; ++i) {
          const int spawned = NovaMission_SpawnMissionShipFromDudeDef(
              state,
              mission.dude_def_index,
              /*forced_ship_class_id=*/-1,
              system_id,
              slot);
          if (spawned < 0) {
            continue;
          }
          Ship &ship = state.ShipAt(static_cast<std::size_t>(spawned));
          ++mission.goal_count_remaining;
          PlaceMissionFleetRespawn(state, ship, bearing, now_ms);
        }
        if ((mission.flags_primary & 0x0001U) != 0U) {
          Mission_ResolveMisnSlot(state, slot, now_ms);
        }
      }
    }

    // Acceptance roll clock stepper.
    if (mission.rearm_roll_clock > 0 && mission.rearm_roll_clock < 0x7d01) {
      --mission.rearm_roll_clock;
    }
  }

  const System *sys =
      state.scenario.System(static_cast<std::int16_t>(system_id + 0x80));
  if (sys == nullptr) {
    return;
  }

  // Count ambient ships: active, in this system, and not targeting the player
  // (squad_leader_ship_slot != 0). Slot 0 (the player) is not counted.
  int ambient = 0;
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    const Ship &ship = state.ShipAt(slot);
    if (ship.is_active && ship.current_system_id == system_id &&
        ship.squad_leader_ship_slot != 0) {
      ++ambient;
    }
  }
  if (ambient < sys->avg_ships) {
    // 1-in-500 encounter roll. The original draws NovaRandom_Range(500) and
    // only proceeds when the draw is exactly 1.
    bool ambient_spawned = false;
    if (RandomBelow(state, 500) == 1) {
      const bool has_encounters =
          sys->encounter_fleet_count >= 1 && sys->encounter_chance_percent > 0;
      // Second gate: a uniform draw in [0, encounter_chance_percent).
      if (has_encounters &&
          RandomBelow(state, 100) < sys->encounter_chance_percent) {
        const int fleet = NovaEncounter_SelectFleetDefWeighted(
            *sys, state.scenario, state.rng);
        if (fleet >= 0) {
          // Original intercept: EncounterFleet_SpawnRandomEncounterFleet.
          (void)NovaEncounter_SpawnFleetLeadShip(
              state, system_id, static_cast<std::int16_t>(fleet));
          ambient_spawned = true;
        }
      }
    }

    // Fall back to a random system dude ship when the encounter did not fire.
    if (!ambient_spawned &&
        NovaDude_SelectRandomSystemDudeClassIndex(*sys, state.rng) != -1) {
      (void)NovaDude_SpawnRandomDudeShipInSystem(state, system_id);
    }
  }

  // Stellar defense-fleet trickle (Ghidra 0x0041d6e0 tail): scan the system's
  // 16 nav stellars; for the first one whose field_0x47 latch is set and whose
  // present_ship_count budget is positive but whose live defenders number below
  // one wave (max_ship_count % 10), spawn one defender and decrement the
  // budget. The original stops at the first satisfying stellar (at most one
  // defense spawn per tick), then runs the ambient-mission tail.
  for (const auto nav : sys->nav_defs) {
    if (nav < 0x80 || static_cast<std::size_t>(nav - 0x80) >=
                          state.scenario.stellars.size()) {
      continue;
    }
    Stellar &stellar =
        state.scenario.stellars[static_cast<std::size_t>(nav - 0x80)];
    if (stellar.field_0x47 == 0 || stellar.present_ship_count <= 0) {
      continue;
    }
    int present = 0;
    for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
      const Ship &ship = state.ShipAt(slot);
      if (ship.is_active && ship.defense_fleet_home_stellar_id == nav) {
        ++present;
      }
    }
    if (present < stellar.max_ship_count % 10) {
      (void)NovaStellar_SpawnDefenseFleetShip(state, nav);
      --stellar.present_ship_count;
      break;
    }
  }
  // TODO(decomp): the original then runs Mission_SpawnAmbientMissionShip and
  // the DAT_007353f4-latched ambient-traffic encounter escalation (<200
  // ships -> fleet 0xff, >0x31 -> fleet 0xfe at mode 4).
}

// Ghidra 0x0041ad50 Ship_DeactivateVacantShipsAndTally. Scans every NPC ship
// slot (1..kMaxShips-1) and deactivates the "vacant" ones, tallying them into
// their spawn-quota bucket before the cleanup:
//   * a parked ship (defense_fleet_home_stellar_id != -1) increments that
//   stellar's
//     present_ship_count, capped at its max_ship_count (the mounted garrison
//     size -- this is what feeds the hostile re-spawn bookkeeping);
//   * a mission ship (mission_owner_slot != -1) increments its mission fleet's
//     current-ship count (capped at the fleet max) -- mission fleets are not
//     reconstructed, so that arm is a logged no-op (TODO(decomp));
// then clears is_active and the targeting/mission/system slots.
//
// Vacancy predicate (bVar3 in the decomp): a slot is SPARED only when it is
// actively engaging the player -- ai_behavior_code > 4, squad_leader_ship_slot
// == 0, not docked at a stellar, not in a mission fleet -- AND is not disabled
// AND `keep_player_engaged` is false (the original's flag==0). State-8 slowdown
// ships are also vacant: they have no special exemption and are removed by this
// same outer sweep. Every other ship (idle wanderers/dudes, parked, mission,
// disabled) is vacant and deactivated. The original runs this on travel/landing
// arrival (Stellar_ProcessTravelAndLanding 0x00457580) and on system entry
// (NovaMainLoop_Run 0x00486880) with flag==0, then
// System_RebuildInitialNpcAndMissionPopulation immediately rebuilds the initial
// scattered population; System_TickNpcSpawnMaintenance handles later attrition.
void NovaShip_DeactivateVacantShipsAndTally(GameState &state,
                                            bool keep_player_engaged) {
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    Ship &ship = state.ShipAt(slot);

    // Vacancy predicate: spared only when actively engaging the player and
    // (with flag==0) not disabled.
    bool vacant = true;
    if (ship.ai_behavior_code > 4 && ship.squad_leader_ship_slot == 0 &&
        ship.defense_fleet_home_stellar_id == -1 &&
        ship.mission_fleet_slot == -1) {
      if (!NovaAiShip_IsDisabled(state, ship) && !keep_player_engaged) {
        vacant = false;
      }
    }
    if (!vacant) {
      continue;
    }

    // Tally into the spawn-quota buckets before clearing. The original checks
    // is_active/mission_owner_slot first: mission ships (active with an owner)
    // go to their mission fleet's current-ship counter; everything else parks
    // at a stellar only when it is active and docked there.
    if (!ship.is_active || ship.mission_owner_slot == -1) {
      if (ship.is_active && ship.defense_fleet_home_stellar_id != -1) {
        // defense_fleet_home_stellar_id is a stellar resource id (>= 0x80); the
        // clean-room stellars table is indexed by id - 0x80. The const
        // ScenarioData::Stellar() accessor cannot mutate the runtime count,
        // so index the vector directly (same pattern as the display-state
        // refresh in targeting.cpp).
        const std::int16_t sid = ship.defense_fleet_home_stellar_id;
        if (sid >= 0x80 && static_cast<std::size_t>(sid - 0x80) <
                               state.scenario.stellars.size()) {
          Stellar &st =
              state.scenario.stellars[static_cast<std::size_t>(sid - 0x80)];
          ++st.present_ship_count;
          if (st.present_ship_count > st.max_ship_count) {
            st.present_ship_count = st.max_ship_count;
          }
        }
      }
    } else {
      // Mission-fleet budget credit (0x0041ad50): a still-owned mission ship
      // leaving play alive credits its mission's aux respawn budget
      // (mission_ship_count_active), capped at mission_ship_count_max. This
      // keeps the aux-fleet top-up (System_TickNpcSpawnMaintenance) from
      // respawning ships that merely left the system.
      const auto owner = ship.mission_owner_slot;
      if (owner >= 0 &&
          owner < static_cast<std::int16_t>(state.active_missions.size()) &&
          state.active_mission_runtime_flags[owner].is_active) {
        ActiveMission &mission = state.active_missions[owner];
        if (mission.mission_ship_count_active <
            mission.mission_ship_count_max) {
          ++mission.mission_ship_count_active;
        }
      }
    }

    // Clear the slot fields, mirroring the original's cleanup writes
    // (velocity_match_target_ship_slot / field_0xbb / field_0xbc included;
    // the two byte flags are unmodelled AI latches, TODO(decomp)).
    ship.is_active = false;
    ship.mission_fleet_slot = -1;
    ship.defense_fleet_home_stellar_id = -1;
    ship.velocity_match_target_ship_slot = -1;
    ship.mission_owner_slot = -1;
    ship.current_system_id = -1;
    ship.squad_leader_ship_slot = -1;
  }
}

// Ghidra 0x004ab970 NovaRandom_Reseed. The original reseeds the global LCG
// with the current millisecond tick count at session bootstrap; we reseed the
// clean-room mt19937 with an unrelated entropy source (random_device + the
// steady clock) so a fresh game reseeds from an uncorrelated source rather
// than the default-42 deterministic spawn sequence.
void NovaGame_ReseedRandom(GameState &state) {
  std::random_device rd;
  const std::uint64_t clock_seed =
      std::chrono::steady_clock::now().time_since_epoch().count();
  std::seed_seq seq{rd(),
                    static_cast<std::uint32_t>(clock_seed),
                    static_cast<std::uint32_t>(clock_seed >> 32U)};
  state.rng.seed(seq);
  NovaLog::Debug("game random reseeded with entropy; ship/fleet spawns will "
                 "vary across sessions");
}

int NovaWeapon_SpawnShipFromCarrierBayWeapon(GameState &state,
                                             const Ship &launcher,
                                             std::int16_t weapon_bank) {
  if (weapon_bank < 0 || weapon_bank >= 0x100) {
    return -1;
  }
  const Weapon *bay_weapon =
      state.scenario.Weapon(static_cast<std::int16_t>(weapon_bank + 0x80));
  if (bay_weapon == nullptr) {
    return -1;
  }
  const int slot =
      NovaShip_AllocateShipSlot(state, launcher.current_system_id, 8);
  if (slot < 0) {
    return -1;
  }
  Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
  const ShipClass *cls = state.scenario.Ship(bay_weapon->ammo_type);

  ship.current_system_id = launcher.current_system_id;
  ship.pos_x = launcher.pos_x;
  ship.pos_y = launcher.pos_y;
  ship.vel_x = launcher.vel_x;
  ship.vel_y = launcher.vel_y;
  ship.speed = launcher.speed;
  ship.ship_class_id = static_cast<std::int16_t>(bay_weapon->ammo_type - 0x80);
  ship.defense_fleet_home_stellar_id = -1;
  ship.velocity_match_target_ship_slot = -1;
  ship.mission_hail_latch = 0;
  ship.afterburner_latch = NovaShip_CanShipUseAfterburner(state, ship) ? 1 : 0;
  ship.mining_scoop_active = NovaOutfit_HasMiningScoopOutfit(state, ship);
  ship.escort_origin_mark = 0; // ShipState +0xbb
  ship.mission_owner_slot = -1;
  ship.ai_behavior_code = 5;
  ship.faction_or_government_id = launcher.faction_or_government_id;
  ship.travel_transfer_mode = -1;
  ship.ai_station_hold_timer = 0.0F;
  ship.ai_state_code = 0;
  ship.ai_control_mode = 0;
  ship.boarded_target_latch = 0;
  ship.post_hit_mode_hint = -1;
  ship.cloak_transition_latch = 0;
  ship.cloak_fade_progress = 0.0F;
  ship.ai_hostility_accumulator = 0;
  ship.jump_destination_stellar_id = -2;
  ship.jump_destination_system_id = -2;
  // The original calls Ship_ComputeShipMaxShieldPoints / MaxArmor
  // (0x00463550/0x004637a0); for a freshly spawned, outfit-less fighter those
  // equal the class base.
  ship.shield_points =
      static_cast<float>(cls != nullptr ? cls->base_shield : 0);
  ship.armor_points = static_cast<float>(cls != nullptr ? cls->base_armor : 0);
  ship.ai_maneuver_timer_ms = static_cast<float>(bay_weapon->lifetime_ticks);
  ship.squad_leader_ship_slot = launcher.ship_instance_id;
  ship.heading = launcher.heading;
  ship.pers_def_slot = -1;
  ship.dude_class_id = -1;
  ship.mission_fleet_slot = -1;
  ship.ionization_points = 0.0F;
  ship.ionization_color = 0; // ShipState field_0xb0
  ship.sprite_animation_cycle_index = 0;
  ship.timed_action_counter =
      cls != nullptr ? cls->timed_action_counter_init : -1;
  ship.waypoint_arrival_marker_b = static_cast<std::int16_t>(
      (cls != nullptr ? cls->skill_variance_percent : 0) - 1);
  ship.waypoint_arrival_marker_a = 0;
  ship.turn_bank_animation_phase = 0.0F;
  ship.hit_reaction_timer = 0.0F;
  ship.player_aggro_accumulator = 0.0F;
  ship.ai_turn_bias_dir = 0;
  ship.sprite_animation_timer = 0.0F;
  ship.cloak_scanner_reveal_screen = -1;
  ship.cloak_scanner_reveal_radar = -1;
  ship.cloak_damage_deactivate_latch = -1;
  ship.escort_command_code = -1;
  ship.escort_command_pending = 0;
  // TODO(decomp): the original also zeroes ShipState fields not yet on the
  // clean-room Ship struct: field_0x60, weapon_exit_animation_phase,
  // alternate_sprite_cycle_index, weapon_sprite_flash_level,
  // escort_released_mark/escort_upgrade_mark, and the field_0xc924 short
  // (0xffff).
  ship.voice_type_mode = RandomBelow(state, 2);
  if (cls != nullptr && cls->inherent_attributes_govt != -1) {
    if (const Government *govt =
            state.scenario.GovernmentByIndex(cls->inherent_attributes_govt);
        govt != nullptr && govt->voice_type_mode != -1) {
      ship.voice_type_mode = govt->voice_type_mode;
    }
  }
  ship.jamming_score = {-1, -1, -1, -1};
  if (cls != nullptr && cls->skill_variance_percent > 0) {
    ship.sprite_animation_cycle_index =
        RandomBelow(state, cls->skill_variance_percent);
  }
  if (cls != nullptr && cls->combat_state_init_range > 0) {
    ship.sprite_animation_timer =
        static_cast<float>(RandomBelow(state, cls->combat_state_init_range));
  }

  // Squad/category bookkeeping. Player-carried category-0 (light) fighters
  // with no sibling clear the player's per-category command when it targets
  // category 3; otherwise the fighter inherits a same-squad, same-category
  // sibling's escort command.
  const std::int16_t leader_slot = ship.squad_leader_ship_slot;
  if (leader_slot == 0 && cls != nullptr && cls->class_category == 0) {
    bool has_sibling = false;
    for (std::size_t i = 1; i < GameState::kMaxShips; ++i) {
      if (static_cast<std::int16_t>(i) == slot) {
        continue;
      }
      const Ship &other = state.ShipAt(static_cast<std::size_t>(i));
      if (other.is_active && other.squad_leader_ship_slot == 0) {
        has_sibling = true;
        break;
      }
    }
    if (!has_sibling && state.target_category_command[0] == 3) {
      state.target_category_command[0] = -1;
    }
  } else if (cls != nullptr) {
    for (std::size_t i = 1; i < GameState::kMaxShips; ++i) {
      if (static_cast<std::int16_t>(i) == slot) {
        continue;
      }
      const Ship &other = state.ShipAt(static_cast<std::size_t>(i));
      if (!other.is_active || other.squad_leader_ship_slot != leader_slot) {
        continue;
      }
      const ShipClass *other_cls = state.scenario.Ship(
          static_cast<std::int16_t>(other.ship_class_id + 0x80));
      if (other_cls != nullptr &&
          other_cls->class_category == cls->class_category &&
          other.escort_command_code != -1) {
        ship.escort_command_code = other.escort_command_code;
        break;
      }
    }
  }

  ship.primary_target_ship_slot =
      launcher.ship_instance_id == 0 ? -1 : launcher.primary_target_ship_slot;

  // Launch spread (weapon Inaccuracy10): heading += rand(2*spread) - spread.
  if (bay_weapon->inaccuracy > 0) {
    ship.heading =
        static_cast<float>(
            RandomBelow(state,
                        static_cast<std::int32_t>(bay_weapon->inaccuracy) * 2) -
            bay_weapon->inaccuracy) +
        ship.heading;
  }

  // Launch velocity: Math_AddPolarVelocityWithClamp (0x0043b4e0) at the
  // truncated heading (the original's FISTP + sign-correction idiom nets to
  // truncation toward zero), weapon Speed_a/100, clamped per-axis to the
  // fighter's effective max speed.
  const float max_speed =
      cls != nullptr ? NovaShip_ComputeEffectiveStats(state, ship, *cls)
                           .max_speed_px_per_tick
                     : 0.0F;
  const float bearing_rad = static_cast<float>(static_cast<int>(ship.heading)) *
                            (3.14159265358979323846F / 180.0F);
  NovaPlayer_AddPolarVelocityClamped(bearing_rad,
                                     bay_weapon->projectile_speed / 100.0F,
                                     max_speed,
                                     ship.vel_x,
                                     ship.vel_y);

  // Stock loadout: the original copies all 0x100 bank ammo/secondary rows
  // from the class's eight default-weapon tables; the clean-room builds the
  // same loadout eagerly via the shared NPC bank initializer.
  NovaWeapon_EnsureNpcWeaponBanks(state, ship);

  // Clear a primary target that is the squad leader or a same-squad sibling.
  if (ship.primary_target_ship_slot != -1) {
    const Ship &target =
        state.ShipAt(static_cast<std::size_t>(ship.primary_target_ship_slot));
    if (leader_slot == ship.primary_target_ship_slot ||
        (leader_slot == target.squad_leader_ship_slot && leader_slot != -1)) {
      ship.primary_target_ship_slot = -1;
    }
  }

  NovaShip_ResetAiBehaviorRuntimeFields(ship);
  return slot;
}

bool NovaShip_LaunchShipFromCarrierBay(GameState &state, Ship &launcher) {
  if (launcher.primary_target_ship_slot == -1) {
    return false;
  }
  const Ship &target =
      state.ShipAt(static_cast<std::size_t>(launcher.primary_target_ship_slot));
  if (target.current_system_id != launcher.current_system_id ||
      !target.is_active) {
    return false;
  }

  // First loaded mode-99 bay bank: mounted ammo > 0, loaded secondary > 0,
  // NPC-mount gate (Flags2 0x100) clear.
  std::int16_t bay_bank = -1;
  const bool is_player = launcher.ship_instance_id == 0;
  for (std::int16_t bank = 0; bank < 0x100; ++bank) {
    const Weapon *def =
        state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
    if (def == nullptr || def->weapon_mode_code != 99 ||
        (def->flags_secondary & 0x100) != 0) {
      continue;
    }
    const std::int16_t mounted =
        is_player
            ? state.weapon_bank_ammo[static_cast<std::size_t>(bank) * 100]
            : launcher.npc_weapon_bank_ammo[static_cast<std::size_t>(bank)];
    const std::int16_t loaded =
        is_player
            ? state.weapon_bank_secondary[static_cast<std::size_t>(bank) * 100]
            : launcher
                  .npc_weapon_bank_secondary[static_cast<std::size_t>(bank)];
    if (mounted < 1 || loaded < 1) {
      continue;
    }
    bay_bank = bank;
    break;
  }
  if (bay_bank == -1) {
    return false;
  }
  const Weapon *def =
      state.scenario.Weapon(static_cast<std::int16_t>(bay_bank + 0x80));

  auto bank_cooldown = [&](std::int16_t bank) -> float & {
    return is_player
               ? state.weapon_bank_cooldown[static_cast<std::size_t>(bank)]
               : launcher
                     .npc_weapon_bank_cooldown[static_cast<std::size_t>(bank)];
  };
  if (bank_cooldown(bay_bank) > 0.0F) {
    // FCOMPP against 0.0: NaN and positive cooldowns fail; zero/negative pass.
    return false;
  }

  launcher.active_weapon_bank_slot = bay_bank;
  if (NovaWeapon_SpawnShipFromCarrierBayWeapon(state, launcher, bay_bank) < 0) {
    return false;
  }

  // Launch sound: the queued path plays def.fire_sound relative to the
  // launcher (original: NovaAudio_PlaySpatialByDistance, volume 5).
  if (def != nullptr && def->fire_sound >= 0) {
    state.pending_fire_sounds.push_back(
        {def->fire_sound, launcher.pos_x, launcher.pos_y, false});
  }

  // Original quirk: the cooldown divisor is the bank's MOUNTED count, not
  // the loaded secondary.
  const std::int16_t mounted =
      is_player
          ? state.weapon_bank_ammo[static_cast<std::size_t>(bay_bank) * 100]
          : launcher.npc_weapon_bank_ammo[static_cast<std::size_t>(bay_bank)];
  const float new_cooldown =
      def != nullptr
          ? static_cast<float>(def->reload_ticks) / static_cast<float>(mounted)
          : 0.0F;
  bank_cooldown(bay_bank) = new_cooldown;
  auto bank_loaded = [&](std::int16_t bank) -> std::int16_t & {
    return is_player
               ? state.weapon_bank_secondary[static_cast<std::size_t>(bank) *
                                             100]
               : launcher
                     .npc_weapon_bank_secondary[static_cast<std::size_t>(bank)];
  };
  --bank_loaded(bay_bank);

  // Shared cooldown (Flags3 0x20): every other bank's cooldown rises to
  // new_cooldown + 2.0 (DAT_00575040) when smaller.
  if (def != nullptr && (def->flags_tertiary & 0x20) != 0) {
    for (std::int16_t bank = 0; bank < 0x100; ++bank) {
      if (bank != bay_bank && bank_cooldown(bank) < new_cooldown + 2.0F) {
        bank_cooldown(bank) = new_cooldown + 2.0F;
      }
    }
  }
  return true;
}

void NovaShip_RecoverCarriedShipToBay(GameState &state, Ship &fighter) {
  const std::int16_t carrier_slot = fighter.squad_leader_ship_slot;
  if (carrier_slot < 0 ||
      static_cast<std::size_t>(carrier_slot) >= GameState::kMaxShips) {
    return;
  }
  Ship &carrier = state.ShipAt(static_cast<std::size_t>(carrier_slot));
  const bool carrier_is_player = carrier_slot == 0;

  // The carrier's mode-99 bay weapon matching the fighter's class (fallback:
  // the class's escort_type clone-source id), with mounted ammo remaining.
  const auto find_bay_bank = [&](std::int16_t class_id) -> std::int16_t {
    if (class_id < 0) {
      return -1;
    }
    for (std::int16_t bank = 0; bank < 0x100; ++bank) {
      const Weapon *def =
          state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
      if (def == nullptr || def->weapon_mode_code != 99 ||
          def->ammo_type != class_id) {
        continue;
      }
      const std::int16_t mounted =
          carrier_is_player
              ? state.weapon_bank_ammo[static_cast<std::size_t>(bank) * 100]
              : carrier.npc_weapon_bank_ammo[static_cast<std::size_t>(bank)];
      if (mounted > 0) {
        return bank;
      }
    }
    return -1;
  };
  const std::int16_t fighter_class_resource =
      static_cast<std::int16_t>(fighter.ship_class_id + 0x80);
  std::int16_t bay_bank = find_bay_bank(fighter_class_resource);
  if (bay_bank == -1) {
    if (const ShipClass *cls = state.scenario.Ship(fighter_class_resource);
        cls != nullptr) {
      bay_bank =
          find_bay_bank(static_cast<std::int16_t>(cls->escort_type + 0x80));
    }
  }
  if (bay_bank == -1) {
    return;
  }
  const Weapon *def =
      state.scenario.Weapon(static_cast<std::int16_t>(bay_bank + 0x80));

  auto bank_loaded = [&](std::int16_t bank) -> std::int16_t & {
    return carrier_is_player
               ? state.weapon_bank_secondary[static_cast<std::size_t>(bank) *
                                             100]
               : carrier
                     .npc_weapon_bank_secondary[static_cast<std::size_t>(bank)];
  };
  auto bank_cooldown = [&](std::int16_t bank) -> float & {
    return carrier_is_player
               ? state.weapon_bank_cooldown[static_cast<std::size_t>(bank)]
               : carrier
                     .npc_weapon_bank_cooldown[static_cast<std::size_t>(bank)];
  };
  // An empty bay re-arms the weapon's reload before the unit is restored.
  if (bank_loaded(bay_bank) == 0 && def != nullptr &&
      bank_cooldown(bay_bank) < static_cast<float>(def->reload_ticks)) {
    bank_cooldown(bay_bank) = static_cast<float>(def->reload_ticks);
  }
  ++bank_loaded(bay_bank);
  // g_shipAvailabilityCachesDirty = 1 on a player carrier (TODO(decomp)).

  fighter.is_active = false;
  fighter.squad_leader_ship_slot = -1;
  fighter.ai_behavior_code = -1;
}

bool NovaShipClass_HasPlayerBayCapacityFor(GameState &state,
                                           std::int16_t ship_class_id,
                                           std::int16_t outfit_slot,
                                           std::int16_t weapon_bank) {
  // Occupancy starts from the resolved holding: the bay's loaded fighter
  // count, or the outfit's owned count when both slots are given.
  std::int16_t resolved_outfit = outfit_slot;
  std::int16_t resolved_bank = weapon_bank;
  std::int16_t occupancy = 0;

  if (outfit_slot == -1 && weapon_bank == -1) {
    // Resolve the bay weapon that would hold this class: first by direct
    // class match, then by escort_type (clone-family) fallback -- the same
    // mapping Ship_LaunchCarriedShipFromBay uses.
    const ShipClass *target_cls =
        state.scenario.Ship(static_cast<std::int16_t>(ship_class_id + 0x80));
    const std::int16_t target_escort =
        target_cls != nullptr ? target_cls->escort_type : -1;
    // First scan: direct class match over all banks.
    for (std::int16_t bank = 0; bank < 0x100; ++bank) {
      const Weapon *def =
          state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
      if (def == nullptr || def->weapon_mode_code != 99 ||
          state.weapon_bank_ammo[static_cast<std::size_t>(bank) * 100] < 1) {
        continue;
      }
      if (static_cast<std::int16_t>(def->ammo_type - 0x80) == ship_class_id) {
        resolved_bank = bank;
        break;
      }
    }
    // Second scan: escort_type (clone-family) fallback.
    if (resolved_bank == -1) {
      for (std::int16_t bank = 0; bank < 0x100; ++bank) {
        const Weapon *def =
            state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
        if (def == nullptr || def->weapon_mode_code != 99 ||
            state.weapon_bank_ammo[static_cast<std::size_t>(bank) * 100] < 1) {
          continue;
        }
        const ShipClass *carried = state.scenario.Ship(def->ammo_type);
        if (carried != nullptr && carried->escort_type == target_escort) {
          resolved_bank = bank;
          break;
        }
      }
    }
    if (resolved_bank == -1) {
      return false;
    }

    // Then the fighter-bay outfit (ModType 3) bound to that bank.
    const std::size_t outfit_count =
        std::min<std::size_t>(state.scenario.outfits.size(), 0x200);
    for (std::size_t i = 0; i < outfit_count && resolved_outfit == -1; ++i) {
      const Outfit &outfit = state.scenario.outfits[i];
      const std::array<std::int16_t, 4> types{outfit.mod_type,
                                              outfit.alt_mod_types[0],
                                              outfit.alt_mod_types[1],
                                              outfit.alt_mod_types[2]};
      const std::array<std::int16_t, 4> vals{outfit.mod_val,
                                             outfit.alt_mod_vals[0],
                                             outfit.alt_mod_vals[1],
                                             outfit.alt_mod_vals[2]};
      for (std::size_t m = 0; m < 4; ++m) {
        if (types[m] == 3 && vals[m] == resolved_bank) {
          resolved_outfit = static_cast<std::int16_t>(i);
          break;
        }
      }
    }
    if (resolved_outfit == -1) {
      return false;
    }
    occupancy =
        state.weapon_bank_secondary[static_cast<std::size_t>(resolved_bank) *
                                    100];
  } else if (outfit_slot == -1 || weapon_bank == -1) {
    return false;
  } else {
    occupancy =
        state.inventory
            .outfit_owned_count[static_cast<std::size_t>(resolved_outfit)];
  }

  // Bay capacity: Bible MaxAmmo per mounted weapon instance, or the outfit's
  // Max field when MaxAmmo is 0/-1.
  const Weapon *bay_weapon =
      state.scenario.Weapon(static_cast<std::int16_t>(resolved_bank + 0x80));
  const int capacity =
      bay_weapon != nullptr && bay_weapon->max_ammo >= 1
          ? static_cast<int>(bay_weapon->max_ammo) *
                state.weapon_bank_ammo[static_cast<std::size_t>(resolved_bank) *
                                       100]
          : static_cast<int>(
                state.scenario
                    .outfits[static_cast<std::size_t>(resolved_outfit)]
                    .max_count);

  // Active carried fighters of this class following the player.
  int carried = 0;
  for (std::size_t i = 1; i < GameState::kMaxShips; ++i) {
    const Ship &other = state.ShipAt(i);
    if (other.is_active && other.ship_class_id == ship_class_id &&
        other.squad_leader_ship_slot == 0 && other.ai_behavior_code == 5 &&
        other.mission_fleet_slot == -1) {
      ++carried;
    }
  }
  return occupancy + carried < capacity;
}

} // namespace game
