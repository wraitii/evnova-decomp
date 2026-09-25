#include "government.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <string>

#include "hud_overlay.hpp"
#include "log.hpp"
#include "mission.hpp"
#include "nova_random.hpp"
#include "outfit.hpp"
#include "rank.hpp"
#include "ship_ai.hpp"
#include "travel.hpp"
#include "util/format.hpp"

namespace game {
namespace {

// Mirrors the original's range guard (`-1 < id && id < 0x100`) shared by the
// relation helpers. The C++ government table can be smaller than the
// original's 0x100 entries in synthetic data, so the size check stands in for
// the absent entries.
[[nodiscard]] bool GovernmentInRange(const ScenarioData &scenario,
                                     std::int16_t govt_id) {
  return govt_id >= 0 && govt_id < 0x100 &&
         static_cast<std::size_t>(govt_id) < scenario.governments.size();
}

// Returns the government entry for a zero-based id when it is valid and not
// derelict (flags_primary & 0x0800), else nullptr.
[[nodiscard]] const Government *RelationEligible(const ScenarioData &scenario,
                                                 std::int16_t govt_id) {
  if (!GovernmentInRange(scenario, govt_id)) {
    return nullptr;
  }
  const Government &g = scenario.governments[static_cast<std::size_t>(govt_id)];
  if ((g.flags_primary & 0x0800U) != 0) {
    return nullptr; // derelict
  }
  return &g;
}

} // namespace

// Ghidra 0x0046bc90 Government_AreGovtsAllied.
bool NovaGovernment_AreGovtsAllied(const ScenarioData &scenario,
                                   std::int16_t govt_a,
                                   std::int16_t govt_b) {
  if (govt_a == govt_b) {
    return true;
  }
  const Government *a = RelationEligible(scenario, govt_a);
  const Government *b = RelationEligible(scenario, govt_b);
  if (a == nullptr || b == nullptr) {
    return false;
  }
  // Either side's class id appearing in the other side's ally list (both
  // directions), matching the decompiled 4x4 double loop.
  for (std::size_t i = 0; i < 4; ++i) {
    if (a->classes[i] != -1) {
      for (std::size_t j = 0; j < 4; ++j) {
        if (b->ally_classes[j] == a->classes[i]) {
          return true;
        }
      }
    }
    if (b->classes[i] != -1) {
      for (std::size_t j = 0; j < 4; ++j) {
        if (a->ally_classes[j] == b->classes[i]) {
          return true;
        }
      }
    }
  }
  return false;
}

// Ghidra 0x0046bdf0 Government_AreGovtsHostileOrXenophobic.
bool NovaGovernment_AreGovtsHostileOrXenophobic(const ScenarioData &scenario,
                                                std::int16_t govt_a,
                                                std::int16_t govt_b) {
  if (govt_a == govt_b) {
    return false;
  }
  // Both governments must be in range to reach the class tables. The original
  // falls straight to a zero return when either side carries the derelict bit
  // (flags_primary 0x0800): a derelict is NOT hostile even to a xenophobic
  // government, because the xenophobic fallback below is skipped. Only
  // out-of-range ids fall through to that fallback (where the range-guarded
  // xenophobic test then rejects them).
  if (GovernmentInRange(scenario, govt_a) &&
      GovernmentInRange(scenario, govt_b)) {
    const Government &a =
        scenario.governments[static_cast<std::size_t>(govt_a)];
    const Government &b =
        scenario.governments[static_cast<std::size_t>(govt_b)];
    if ((a.flags_primary & 0x0800U) != 0U ||
        (b.flags_primary & 0x0800U) != 0U) {
      return false; // derelict: no relation checks and no xenophobic override
    }
    // Direct enemy-class hostility check (both directions).
    for (std::size_t i = 0; i < 4; ++i) {
      if (a.classes[i] != -1) {
        for (std::size_t j = 0; j < 4; ++j) {
          if (b.enemy_classes[j] == a.classes[i]) {
            return true;
          }
        }
      }
      if (b.classes[i] != -1) {
        for (std::size_t j = 0; j < 4; ++j) {
          if (a.enemy_classes[j] == b.classes[i]) {
            return true;
          }
        }
      }
    }
  }
  // Fallback: not allied + one side xenophobic -> hostile from sight.
  if (NovaGovernment_AreGovtsAllied(scenario, govt_a, govt_b)) {
    return false;
  }
  const auto xenophobic = [&scenario](std::int16_t id) {
    if (id < 0 || id >= 0x100 ||
        static_cast<std::size_t>(id) >= scenario.governments.size()) {
      return false;
    }
    return (scenario.governments[static_cast<std::size_t>(id)].flags_primary &
            0x0001U) != 0;
  };
  return xenophobic(govt_a) || xenophobic(govt_b);
}

// Ghidra 0x0046bff0 Government_DoGovtsShareClass.
bool NovaGovernment_DoGovtsShareClass(const ScenarioData &scenario,
                                      std::int16_t govt_a,
                                      std::int16_t govt_b) {
  if (govt_a == govt_b) {
    return true;
  }
  if (govt_a < 0 || govt_a >= 0x100 || govt_b < 0 || govt_b >= 0x100) {
    return false;
  }
  const auto idx_a = static_cast<std::size_t>(govt_a);
  const auto idx_b = static_cast<std::size_t>(govt_b);
  if (idx_a >= scenario.governments.size() ||
      idx_b >= scenario.governments.size()) {
    return false;
  }
  const Government &a = scenario.governments[idx_a];
  const Government &b = scenario.governments[idx_b];
  for (std::size_t i = 0; i < 4; ++i) {
    if (a.classes[i] != -1 && a.classes[i] == b.classes[i]) {
      return true;
    }
  }
  return false;
}

// Ghidra 0x0046E860 Government_GetGovernmentPolicyFlag.
bool NovaGovernment_GetPolicyFlag(const ScenarioData &scenario,
                                  std::int16_t govt_id,
                                  int flag_index) {
  if (govt_id < 0 || govt_id >= 0x100 ||
      static_cast<std::size_t>(govt_id) >= scenario.governments.size()) {
    return false;
  }
  if (flag_index < 0 || flag_index >= 2) {
    return false;
  }
  return scenario.governments[static_cast<std::size_t>(govt_id)]
             .policy_flags[static_cast<std::size_t>(flag_index)] != 0;
}

// Ghidra 0x004629E0 Government_IsCandidateHostileToTargeter. See the header for
// the branch description; this is the single caller's (the stellar defense
// battery tick) target filter. The original reaches the ship class's
// inherent-combat government and the governing system through the globals; the
// port resolves both through ScenarioData. The +0x8 targeter word compared
// against g_travel_selected_stellar_id is the passed-in stellar resource id.
bool NovaGovernment_IsCandidateHostileToTargeter(const GameState &state,
                                                 const Ship &ship,
                                                 const Stellar &stellar,
                                                 std::int16_t stellar_id) {
  if (stellar.dominated) {
    return false;
  }
  if (ship.ship_instance_id != 0 && NovaAiShip_IsShipInAiState8(ship)) {
    return false;
  }
  const std::int16_t current_system = state.player.current_system_id;
  if ((stellar.availability_flags & 0x200U) == 0U) {
    if (ship.ship_instance_id == 0 || ship.squad_leader_ship_slot == 0) {
      // Player or squad-leader: the reputation / government-relation ladder.
      std::int16_t targeter_govt = stellar.government_id;
      if (targeter_govt == -1) {
        const System *system = state.scenario.System(
            static_cast<std::int16_t>(current_system + 0x80));
        if (system != nullptr && system->government_id != -1) {
          targeter_govt = system->government_id;
        }
      }
      bool hostile = false;
      if (targeter_govt != -1 && static_cast<std::size_t>(targeter_govt) <
                                     state.scenario.governments.size()) {
        const Government &govt =
            state.scenario.governments[static_cast<std::size_t>(targeter_govt)];
        const std::int16_t rep =
            current_system >= 0 && static_cast<std::size_t>(current_system) <
                                       state.system_reputation.size()
                ? state.system_reputation[static_cast<std::size_t>(
                      current_system)]
                : 0;
        // The original's SBORROW4 comparison is `rep + crime_tol < 0`.
        hostile = static_cast<int>(rep) + static_cast<int>(govt.crime_tol) < 0;
        if (!hostile) {
          if ((govt.flags_primary & 0x0001U) == 0U) {
            const ShipClass *cls = state.scenario.Ship(
                static_cast<std::int16_t>(ship.ship_class_id + 0x80));
            const std::int16_t inherent =
                cls != nullptr ? cls->inherent_combat_govt : -1;
            hostile = NovaGovernment_AreGovtsHostileOrXenophobic(
                state.scenario, inherent, stellar.government_id);
          } else if (rep < 0) {
            hostile = true;
          }
        }
        if (hostile) {
          if (NovaGovernment_GetPolicyFlag(
                  state.scenario, stellar.government_id, 0)) {
            hostile = false;
          }
          if (govt.iff_scrambler_active) {
            hostile = false;
          }
        }
      }
      if (state.travel.selected_stellar_id == stellar_id) {
        hostile = false;
      }
      return hostile;
    }
    // Ordinary NPC: compare its faction directly against the stellar govt.
    return NovaGovernment_AreGovtsHostileOrXenophobic(
        state.scenario, stellar.government_id, ship.faction_or_government_id);
  }
  // Special (availability 0x200) stellar: only a derelict/abandoned sentinel
  // admits the player/leader as a hostile target.
  return stellar.defense_fleet_mounted != 0 &&
         (ship.ship_instance_id == 0 || ship.squad_leader_ship_slot == 0);
}

// @port 0x0040fd20 100%
// Ghidra 0x0040fd20 Ship_DoesShipLikePlayer. See the header for the branch
// description. The final GovtDef +0x83 gate is the per-government IFF-scrambler
// latch `Government.iff_scrambler_active` (written by
// Outfit_RecomputeOutfitDerivedState 0x0046d4b0 at 0x0046d901).
bool NovaShip_DoesShipLikePlayer(const GameState &state, const Ship &ship) {
  if (NovaAiShip_ShouldKeepPressingTarget(state, ship)) {
    return false;
  }
  if (ship.squad_leader_ship_slot == 0) {
    return true;
  }
  if (ship.faction_or_government_id != -1 &&
      NovaGovernment_GetPolicyFlag(
          state.scenario, ship.faction_or_government_id, 0)) {
    return true;
  }
  // Mission-fleet branch (0x0040fd72): a live mission ship aids only while its
  // ShipGoal is 3/4 and its ShipBehav is exactly 1.
  if (ship.mission_fleet_slot != -1) {
    const std::int16_t slot = ship.mission_fleet_slot;
    if (slot < 0 ||
        static_cast<std::size_t>(slot) >= GameState::kMaxActiveMissions) {
      return false;
    }
    const auto index = static_cast<std::size_t>(slot);
    if (!state.active_mission_runtime_flags[index].is_active) {
      return false;
    }
    const ActiveMission &mission = state.active_missions[index];
    if (mission.ship_goal != 3 && mission.ship_goal != 4) {
      return false;
    }
    if (mission.ship_behavior == -1) {
      return false;
    }
    return mission.ship_behavior == 1;
  }
  if (ship.faction_or_government_id == -1) {
    return true; // faction-less ships aid anyone
  }
  const auto faction = static_cast<std::size_t>(ship.faction_or_government_id);
  if (faction >= state.scenario.governments.size()) {
    return false;
  }
  const Government &govt = state.scenario.governments[faction];

  // Player's current system government + reputation (g_system_reputation,
  // indexed by the 0-based system id like the original's g_system_defs_ptr).
  const std::int16_t sys_id = state.player.current_system_id;
  const System *sys =
      state.scenario.System(static_cast<std::int16_t>(sys_id + 0x80));
  const std::int16_t sys_govt = sys != nullptr ? sys->government_id : -1;
  const std::int16_t rep =
      sys_id >= 0 &&
              static_cast<std::size_t>(sys_id) < state.system_reputation.size()
          ? state.system_reputation[static_cast<std::size_t>(sys_id)]
          : 0;

  const auto crime_tolerance = [&](std::int16_t govt_id) -> std::int16_t {
    if (govt_id < 0 || static_cast<std::size_t>(govt_id) >=
                           state.scenario.governments.size()) {
      return 0;
    }
    return state.scenario.governments[static_cast<std::size_t>(govt_id)]
        .crime_tol;
  };

  // Xenophobic ship faction (flags_primary & 0x0001, 0x0040fe18): aid is
  // admitted only when reputation clears the crime tolerance of the system
  // government; a foreign system government whose tolerance is also cleared
  // is explicitly NOT liked (a xenophobe resents the player's standing with a
  // rival). Falling through runs the common tail below.
  if ((govt.flags_primary & 0x0001U) != 0) {
    if (sys_govt == ship.faction_or_government_id) {
      if (crime_tolerance(sys_govt) < rep) {
        return true;
      }
    } else if (sys_govt < 0) {
      if (crime_tolerance(0) < rep) {
        return false;
      }
    } else if (crime_tolerance(sys_govt) < rep) {
      return false;
    }
  }

  // Common ladder (0x0040fe95). When the system has no government, the 0x0002
  // flag and the allied/neutral arms admit aid while reputation stays on the
  // non-criminal side of the relevant CrimeTol (`rep + CrimeTol >= 0`); only
  // a hostile government inverts that (it likes a player who is a criminal to
  // it). Each failing arm falls through to the +0x83 IFF-scrambler gate.
  if (sys_govt < 0) {
    if ((govt.flags_primary & 0x0002U) == 0) {
      return true;
    }
    if (rep + crime_tolerance(static_cast<std::int16_t>(faction)) >= 0) {
      return true;
    }
  } else if (NovaGovernment_AreGovtsAllied(
                 state.scenario, sys_govt, ship.faction_or_government_id)) {
    if (rep + crime_tolerance(sys_govt) >= 0) {
      return true;
    }
  } else if (NovaGovernment_AreGovtsHostileOrXenophobic(
                 state.scenario, sys_govt, ship.faction_or_government_id)) {
    if (rep + crime_tolerance(sys_govt) < 0) {
      return true;
    }
  } else if ((govt.flags_primary & 0x0002U) == 0) {
    return true;
  } else if (rep + crime_tolerance(sys_govt) >= 0) {
    return true;
  }

  return govt.iff_scrambler_active;
}

// @port 0x00413610 90% gameplay,moddata
// Ghidra 0x00413610 Government_TryTriggerGovtAssistanceEncounter.
bool NovaGovernment_TryTriggerAssistanceEncounter(GameState &state,
                                                  const Ship &ship,
                                                  bool force) {
  if (ship.faction_or_government_id < 0 || ship.current_system_id < 0) {
    return false;
  }
  const auto system_index = static_cast<std::size_t>(ship.current_system_id);
  if (system_index >= GameState::kMaxSystems) {
    return false;
  }
  const System *system = state.scenario.System(
      static_cast<std::int16_t>(ship.current_system_id + 0x80));
  if (system == nullptr || system->reinf_fleet < 0 ||
      state.reinforcement_countdown[system_index] > 0.0F ||
      state.reinforcement_retrigger_delay[system_index] > 0 ||
      ship.ai_odds_score <= 0.0F) {
    return false;
  }
  const FleetDef *fleet = state.scenario.Fleet(
      static_cast<std::int16_t>(system->reinf_fleet + 0x80));
  if (fleet == nullptr || fleet->government_id < 0 ||
      !NovaGovernment_AreGovtsAllied(state.scenario,
                                     ship.faction_or_government_id,
                                     fleet->government_id)) {
    return false;
  }
  const Government *reinforcement_govt =
      state.scenario.GovernmentByIndex(fleet->government_id);
  // The original compares the reinforcement government's MaxOdds (GovtDef 0x60,
  // payload +0x16) against the ship's combat odds score, not SkillMult.
  if (reinforcement_govt == nullptr ||
      (!force && reinforcement_govt->max_odds * 0.5F >= ship.ai_odds_score)) {
    return false;
  }

  // The original reads cached global/per-government ModType-44 inhibitor
  // bytes built by Outfit_RecomputeOutfitDerivedState. Re-evaluating the same
  // owned-outfit/class match here avoids hidden mutable government state, but
  // does not preserve the original's stale-cache quirk after outfit removal.
  // TODO(decomp(0x0046D4B0)) skipped: stale reinforcement-inhibitor cache.
  bool inhibited = false;
  for (std::size_t id = 0; id < state.inventory.outfit_owned_count.size() &&
                           id < state.scenario.outfits.size();
       ++id) {
    if (state.inventory.outfit_owned_count[id] <= 0) {
      continue;
    }
    const Outfit &outfit = state.scenario.outfits[id];
    const std::array<std::int16_t, 4> types{outfit.mod_type,
                                            outfit.alt_mod_types[0],
                                            outfit.alt_mod_types[1],
                                            outfit.alt_mod_types[2]};
    const std::array<std::int16_t, 4> values{outfit.mod_val,
                                             outfit.alt_mod_vals[0],
                                             outfit.alt_mod_vals[1],
                                             outfit.alt_mod_vals[2]};
    for (std::size_t effect = 0; effect < types.size(); ++effect) {
      if (types[effect] !=
          static_cast<std::int16_t>(OutfitEffect::kReinforceInhibitor)) {
        continue;
      }
      if (values[effect] == -1) {
        inhibited = true;
        break;
      }
      const auto matches_class = [&](std::int16_t govt_id) {
        const Government *govt = state.scenario.GovernmentByIndex(govt_id);
        return govt != nullptr &&
               std::find(govt->classes.begin(),
                         govt->classes.end(),
                         values[effect]) != govt->classes.end();
      };
      inhibited = matches_class(ship.faction_or_government_id) ||
                  matches_class(fleet->government_id);
      if (inhibited) {
        break;
      }
    }
    if (inhibited) {
      break;
    }
  }
  if (inhibited) {
    state.reinforcement_retrigger_delay[system_index] = 1;
    return false;
  }

  state.reinforcement_countdown[system_index] =
      static_cast<float>(system->reinf_time);
  return true;
}

// @port 0x00440750 100%
// Ghidra 0x00440750 Government_ApplyReputationCreditDelta. The mission
// PayVal opcode (EV Nova Bible, mïsn "PayVal"):
//   1..            flat credit award (inventory/loadout latch dirtied)
//   -1..-9999      no effect
//   -10128-g ..    systems of government g (and, in the -20xxx/-30xxx
//   -20128-g /     ally/class variants) have negative reputation cleared
//   -30128-g
//   -40001..-40099 deduct this percentage of the player's cash
//   <= -40100      no effect (the -50000- range is consumed at acceptance)
// Cash rounding matches the original's x87 FIST + residual/sign correction
// idiom, which truncates toward zero (see docs/x87_precision.md);
// DAT_00575500 = 0.5 and DAT_00575508 = 0.01.
void NovaGovernment_ApplyReputationCreditDelta(GameState &state,
                                               std::int32_t delta) {
  const auto clear_negative_reputation = [&state](auto &&matches) {
    const auto count = static_cast<std::int16_t>(
        std::min<std::size_t>(state.system_reputation.size(), 0x800));
    for (std::int16_t i = 0; i < count; ++i) {
      const std::int16_t system_govt =
          state.scenario.systems[static_cast<std::size_t>(i)].government_id;
      if (matches(system_govt) &&
          state.system_reputation[static_cast<std::size_t>(i)] < 0) {
        state.system_reputation[static_cast<std::size_t>(i)] = 0;
      }
    }
  };
  if (delta >= 1) {
    state.player.credits += delta;
    // g_playerInventoryAndLoadoutDirty in the original.
    state.InvalidateDerivedStatCaches();
    return;
  }
  if (delta > -20000) {
    if (delta >= -9999) {
      return;
    }
    const auto govt = static_cast<std::int16_t>(-delta - 10128);
    clear_negative_reputation(
        [govt](std::int16_t system_govt) { return system_govt == govt; });
    return;
  }
  if (delta > -30000) {
    const auto govt = static_cast<std::int16_t>(-delta - 20128);
    clear_negative_reputation([&state, govt](std::int16_t system_govt) {
      return NovaGovernment_AreGovtsAllied(state.scenario, system_govt, govt);
    });
    return;
  }
  if (delta > -40000) {
    const auto govt = static_cast<std::int16_t>(-delta - 30128);
    clear_negative_reputation([&state, govt](std::int16_t system_govt) {
      return NovaGovernment_DoGovtsShareClass(
          state.scenario, system_govt, govt);
    });
    return;
  }
  if (delta > -40100) {
    const auto percent = -delta - 40000;
    // 0x00440750 computes in 32-bit float and truncates toward zero (x87
    // FIST + residual/sign correction), not round-to-nearest.
    const float adjusted = static_cast<float>(state.player.credits) -
                           static_cast<float>(state.player.credits) *
                               static_cast<float>(percent) * 0.01F;
    state.player.credits = static_cast<std::int32_t>(adjusted);
    return;
  }
}

// Ghidra 0x0046f100 Government_IsShipGovernmentDerelict.
bool NovaGovernment_IsGovernmentDerelict(const ScenarioData &scenario,
                                         std::int16_t government_id) {
  const Government *government = scenario.GovernmentByIndex(government_id);
  return government != nullptr && (government->flags_primary & 0x0800U) != 0;
}

// Ghidra: the runtime GovtDef +0x48..+0x50 crime-penalty words, indexed by
// event code. The loader writes the payload +0x0a..+0x12 block into these
// fields in order (smug/disab/board/kill/shoot).
std::int16_t NovaGovernment_CrimePenalty(const Government &government,
                                         std::int16_t event_code) {
  switch (event_code) {
  case 0:
    return government.smug_penalty;
  case 1:
    return government.disab_penalty;
  case 2:
    return government.board_penalty;
  case 3:
    return government.kill_penalty;
  case 4:
    return government.shoot_penalty;
  default:
    return 0;
  }
}

namespace {

// P[govt][event] for a 0-based government index; the loader clamps missing
// governments to -1 and the penalty words then read 0.
[[nodiscard]] std::int16_t CombatPenalty(const ScenarioData &scenario,
                                         std::int16_t government_id,
                                         std::int16_t event_code) {
  const Government *government = scenario.GovernmentByIndex(government_id);
  return government != nullptr
             ? NovaGovernment_CrimePenalty(*government, event_code)
             : static_cast<std::int16_t>(0);
}

[[nodiscard]] bool GovernmentFlagBit(const ScenarioData &scenario,
                                     std::int16_t government_id,
                                     std::uint16_t bit) {
  const Government *government = scenario.GovernmentByIndex(government_id);
  return government != nullptr && (government->flags_primary & bit) != 0;
}

} // namespace

// Ghidra 0x00467140 Government_PropagateFactionCombatInfluenceToNearbySystems.
// See the header for the contract. Faithful to the original branch ladder:
//  - visit-mask re-entry guard on the 0x800-entry system table;
//  - the input system's visibility root is the chain head (its twins follow
//    visible_parent_system_id), and only is_visible systems contribute;
//  - the delta per system is selected from the system government's or the
//    event faction's crime-penalty word, halved/quartered by relation, then
//    scaled by `scale`;
//  - a delta whose magnitude rounds below 1.0 is ignored (reputation is an
//    integer) and does not arm the adjacency recursion;
//  - when anything changed, every non-empty adjacency link is resolved to its
//    discovery slot and re-flooded with scale * 0.65.
void NovaGovernment_PropagateFactionCombatInfluence(
    GameState &state,
    std::int16_t system_id,
    std::int16_t faction_or_government_id,
    std::int16_t event_code,
    double scale,
    std::span<bool> visit_mask) {
  auto &systems = state.scenario.systems;
  if (system_id < 0 || system_id >= 0x800 ||
      static_cast<std::size_t>(system_id) >= systems.size()) {
    return;
  }
  const auto sys_index = static_cast<std::size_t>(system_id);
  if (visit_mask[sys_index]) {
    return;
  }
  visit_mask[sys_index] = true;

  const bool faction_xenophobic =
      faction_or_government_id != -1 &&
      GovernmentFlagBit(state.scenario, faction_or_government_id, 0x0001U);

  bool changed = false;
  std::int16_t chain = systems[sys_index].visibility_root_system_id != -1
                           ? systems[sys_index].visibility_root_system_id
                           : system_id;
  while (chain != -1 && static_cast<std::size_t>(chain) < systems.size()) {
    visit_mask[static_cast<std::size_t>(chain)] = true;
    System &sys = systems[static_cast<std::size_t>(chain)];
    if (sys.is_visible) {
      const std::int16_t sysgov = sys.government_id;
      float delta = 0.0F;
      if (sysgov == -1) {
        if (faction_or_government_id == -1) {
          delta =
              static_cast<float>(CombatPenalty(state.scenario, 0, event_code)) *
              0.5F;
        } else if (faction_xenophobic) {
          delta = -static_cast<float>(CombatPenalty(
                      state.scenario, faction_or_government_id, event_code)) *
                  0.5F;
        } else {
          delta = static_cast<float>(CombatPenalty(
                      state.scenario, faction_or_government_id, event_code)) *
                  0.25F;
        }
      } else if (faction_or_government_id == sysgov) {
        delta = static_cast<float>(CombatPenalty(
            state.scenario, faction_or_government_id, event_code));
      } else if (faction_or_government_id == -1) {
        if (GovernmentFlagBit(state.scenario, sysgov, 0x0001U)) {
          delta = -static_cast<float>(
                      CombatPenalty(state.scenario, sysgov, event_code)) *
                  0.5F;
        } else if (GovernmentFlagBit(state.scenario, sysgov, 0x0002U)) {
          delta = static_cast<float>(
                      CombatPenalty(state.scenario, sysgov, event_code)) *
                  0.5F;
        }
      } else if (!NovaGovernment_AreGovtsAllied(
                     state.scenario, faction_or_government_id, sysgov)) {
        delta = -static_cast<float>(
                    CombatPenalty(state.scenario, sysgov, event_code)) *
                0.5F;
      } else if (NovaGovernment_AreGovtsHostileOrXenophobic(
                     state.scenario, faction_or_government_id, sysgov)) {
        delta = -static_cast<float>(
                    CombatPenalty(state.scenario, sysgov, event_code)) *
                0.5F;
      } else if (NovaGovernment_AreGovtsAllied(
                     state.scenario, faction_or_government_id, sysgov)) {
        delta = static_cast<float>(CombatPenalty(
            state.scenario, faction_or_government_id, event_code));
      } else {
        delta = static_cast<float>(CombatPenalty(
                    state.scenario, faction_or_government_id, event_code)) *
                0.5F;
      }
      delta *= static_cast<float>(scale);
      if (std::fabs(delta) >= 1.0F &&
          static_cast<std::size_t>(chain) < state.system_reputation.size()) {
        changed = true;
        const std::int16_t rep =
            state.system_reputation[static_cast<std::size_t>(chain)];
        // 0x00467140 truncates the new reputation toward zero (x87 FIST +
        // residual/sign correction), not round-to-nearest.
        long updated = static_cast<long>(static_cast<float>(rep) - delta);
        updated = std::clamp(updated, -32000L, 32000L);
        state.system_reputation[static_cast<std::size_t>(chain)] =
            static_cast<std::int16_t>(updated);
      }
    }
    chain = sys.visible_parent_system_id;
  }

  if (!changed) {
    return;
  }
  const System &origin = systems[sys_index];
  for (const std::int16_t link : origin.links) {
    if (link < 0x80) {
      continue;
    }
    const std::int16_t resolved = NovaSystem_ResolveDiscoverySlot(
        state, static_cast<std::int16_t>(link - 0x80));
    NovaGovernment_PropagateFactionCombatInfluence(state,
                                                   resolved,
                                                   faction_or_government_id,
                                                   event_code,
                                                   scale * 0.65,
                                                   visit_mask);
  }
}

// Ghidra 0x00466fc0 Government_ProcessFactionCombatEvent. See the header.
void NovaGovernment_ProcessFactionCombatEvent(
    GameState &state,
    std::int16_t system_id,
    std::int16_t faction_or_government_id,
    std::int16_t event_code,
    std::int16_t mission_fleet_slot) {
  // Derelict (Flags 0x0800) governments never track reputation.
  if (faction_or_government_id != -1 &&
      GovernmentFlagBit(state.scenario, faction_or_government_id, 0x0800U)) {
    return;
  }
  std::array<bool, 0x800> visit_mask{};
  if (mission_fleet_slot != -1) {
    // A live mission ship's victim slot makes the event inert: the original
    // skips both the flood and the rank revocation. Kill/disable/board pass the
    // victim's +0xC8D2; smuggle and stellar-destruction pass -1.
    return;
  }
  NovaGovernment_PropagateFactionCombatInfluence(
      state, system_id, faction_or_government_id, event_code, 1.0, visit_mask);
  // Crime revocation: deactivate every active, defined, non-permanent rank
  // credited to a government allied to the event faction whose status flags
  // mark it crime-sensitive (0x0040 any crime, or 0x0004 for disable/kill).
  auto &ranks = state.scenario.ranks;
  for (std::size_t i = 0; i < ranks.size(); ++i) {
    const RankDef &rank = ranks[i];
    if (!rank.active || !rank.defined) {
      continue;
    }
    if ((rank.flags & 0x0008U) != 0) {
      continue; // permanent
    }
    if (!NovaGovernment_AreGovtsAllied(
            state.scenario, faction_or_government_id, rank.government_id)) {
      continue;
    }
    const bool crime_sensitive =
        (rank.flags & 0x0040U) != 0 ||
        ((rank.flags & 0x0004U) != 0 && (event_code == 1 || event_code == 3));
    if (crime_sensitive) {
      Rank_Deactivate(state, static_cast<std::int16_t>(i));
    }
  }
}

namespace {

// Ship_ScanPlayerForContraband scan range (FLOAT_0057501c). The hyperspace
// guard delegates to NovaTravel_PlayerPastJumpOnset so the ramp constants stay
// in one place.
constexpr float kContrabandScanRange = 100.0F;

// The ship class name (original ShipClassDef +0x6c), used as the fallback
// prefix for both the mission and the pers/outfit/junk message arms.
[[nodiscard]] std::string ClassName(const GameState &state, const Ship &ship) {
  if (ship.ship_class_id >= 0 && static_cast<std::size_t>(ship.ship_class_id) <
                                     state.scenario.ships.size()) {
    return state.scenario.ships[static_cast<std::size_t>(ship.ship_class_id)]
        .display_name;
  }
  return {};
}

// Outfit/junk arm prefix name: the assigned përs display name, else the class.
[[nodiscard]] std::string ScannerName(const GameState &state,
                                      const Ship &ship) {
  if (ship.pers_def_slot >= 0 && static_cast<std::size_t>(ship.pers_def_slot) <
                                     state.scenario.pers_defs.size()) {
    return state.scenario
        .pers_defs[static_cast<std::size_t>(ship.pers_def_slot)]
        .display_name;
  }
  return ClassName(state, ship);
}

// Mission arm prefix name: the active mission's fleet name when set, else the
// scanning ship's class name (original: mission_fleet_name[0] < 1 fallback).
[[nodiscard]] std::string MissionScanName(const GameState &state,
                                          const Ship &ship,
                                          const ActiveMission &mission) {
  if (!mission.mission_fleet_name.empty()) {
    return mission.mission_fleet_name;
  }
  return ClassName(state, ship);
}

// One STR# 0x7d2 entry, empty when absent (parity with Resource_AppendString-
// Entry, which leaves the buffer untouched for a missing entry).
[[nodiscard]] std::string ScanString(std::uint16_t entry) {
  return NovaHud_LoadStringEntry(0x7d2, entry).value_or(std::string{});
}

[[nodiscard]] std::string ScanPrefix(const GameState &state,
                                     const std::string &name) {
  std::string text = name;
  text += ":  ";
  text += state.player.ship_name;
  text += ", ";
  return text;
}

// Mission/junk percentage fine (scan_fine < 0): -scan_fine percent of credits,
// truncated toward zero and scaled to hundreds, minimum 1. Mirrors the x87
// int conversion at 0x00402258 (FIST + truncation correction), not the flat
// positive arm.
[[nodiscard]] std::int32_t ContrabandPercentageFine(std::int16_t scan_fine,
                                                    std::int32_t credits) {
  // Disassembly (0x00402258): (credits * -scan_fine * 1e-4) is computed in
  // double, FIST'd to an int (the inlined _ftol truncation correction makes it
  // trunc-toward-zero of the double), then scaled by *100 as a 32-bit imul.
  // The float store between FIST and the correction feeds only the sign test,
  // not the integer value. Preserve the 32-bit multiply wrap (observable: a
  // wrapped negative fine collapses to the min-1 credit fine), but clamp the
  // double before the int cast so the cast itself is not UB.
  const double raw = static_cast<double>(credits) *
                     static_cast<double>(-static_cast<int>(scan_fine)) * 1.0e-4;
  double truncated = std::trunc(raw);
  if (truncated > 2147483647.0) {
    truncated = 2147483647.0;
  } else if (truncated < -2147483648.0) {
    truncated = -2147483648.0;
  }
  const std::int32_t base = static_cast<std::int32_t>(truncated);
  const std::uint32_t wrapped = static_cast<std::uint32_t>(base) * 100U;
  std::int32_t fine = std::bit_cast<std::int32_t>(wrapped);
  if (fine < 1) {
    fine = 1;
  }
  return fine;
}

void ApplyScanFine(GameState &state, std::int32_t fine) {
  state.player.credits -= fine;
  if (state.player.credits < 0) {
    state.player.credits = 0;
  }
  // Ghidra 0x00401800 sets g_playerInventoryAndLoadoutDirty after a fine. Its
  // only reader, NovaUi_RefreshGameplayPanels (0x0045d320), marks the cargo HUD
  // dirty; the port redraws it every frame, so the cell-specific scan latches
  // are deliberately left untouched here (details in docs/contraband_scan.md).
}

void AppendFineText(std::string &text, std::int32_t fine) {
  text += " ";
  text += ScanString(0x17c);
  text += " ";
  text += evnova::util::GroupThousands(fine);
  text += " ";
  text += fine < 2 ? "credit" : "credits";
  text += ".";
}

// The original matches 0x18a ("an") / 0x189 ("a") from the lowercased first
// character of the singular outfit name (MWRuntime_FUN_004d6230 = tolower).
[[nodiscard]] bool StartsWithVowel(const std::string &name) {
  if (name.empty()) {
    return false;
  }
  const char c =
      static_cast<char>(std::tolower(static_cast<unsigned char>(name.front())));
  return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
}

// The original plays g_nova_control_bits[164] (fine/warning) or [152] (mission
// failure) through NovaAudio_FillVoiceSlotDescriptor(handle, 1,
// g_centered_audio_gain). Those byte offsets are
// g_transition_sound_handle_table slots: NovaAudio_PreloadGameplayData
// (0x004b0740) fills g_nova_control_bits+0x94+i*4 with
// NovaSound_LoadDecodedById(0x96+i) (snd 150+i), so +0x98 is index 1 (snd 151)
// and +0xa4 is index 4 (snd 154). The port queues the matching
// transition_sounds entry on the shared pending_ui_sounds channel (drained by
// the spaceflight loop, which owns the audio device); width 1 matches the
// original descriptor.
void ShowScanMessage(GameState &state,
                     std::string message,
                     std::int16_t control_bit_offset) {
  const std::int16_t transition_index = control_bit_offset == 152 ? 1 : 4;
  state.pending_ui_sounds.push_back(
      GameState::PendingUiSound{transition_index, 1});
  NovaHud_ShowOverlayMessage(
      state, std::move(message), static_cast<std::uint64_t>(0x190));
}

} // namespace

// @port 0x00401800 100%
// Ghidra 0x00401800 Ship_ScanPlayerForContraband.
void NovaShip_ScanPlayerForContraband(GameState &state,
                                      Ship &ship,
                                      std::uint32_t now_ms) {
  // Only warships (AI type 3) and interceptors (AI type 4) scan.
  if (ship.ai_behavior_code != 3 && ship.ai_behavior_code != 4) {
    return;
  }
  if (ship.faction_or_government_id == -1) {
    return;
  }
  const Government *govt =
      state.scenario.GovernmentByIndex(ship.faction_or_government_id);
  if (govt == nullptr || govt->smug_penalty == 0) {
    return;
  }
  const Ship &player = state.player;
  if (std::abs(ship.pos_x - player.pos_x) > kContrabandScanRange ||
      std::abs(ship.pos_y - player.pos_y) > kContrabandScanRange) {
    return;
  }
  // Original passes (player, scanner) to
  // Ship_CanShipEngageTargetUnderCloakRules.
  if (!NovaAiShip_CanEngageTargetUnderCloakRules(state, player, ship)) {
    return;
  }
  // Single 100-sided draw; scan on <= 75. No other RNG in this function.
  if (RandomBelow(state, 100) > 75) {
    return;
  }
  // Hyperspace-committed guard: skip once the player's jump hold has passed
  // its onset (the original gates on ai_station_hold_timer > 0, which the port
  // tracks as TravelState::JumpPhase::kHold; the Ship field is dormant here).
  if (state.travel.jump_phase == TravelState::JumpPhase::kHold &&
      !NovaAiShip_IsDisabled(state, player) &&
      NovaTravel_PlayerPastJumpOnset(state)) {
    return;
  }

  const std::int16_t scan_mask = govt->scan_mask;

  // --- Mission cargo: first matching active mission, then fall through. ---
  for (std::size_t slot = 0; slot < state.active_missions.size(); ++slot) {
    MissionRuntimeFlags &runtime = state.active_mission_runtime_flags[slot];
    const ActiveMission &mission = state.active_missions[slot];
    if (!runtime.is_active) {
      continue;
    }
    if ((static_cast<std::uint16_t>(mission.scan_mask) &
         static_cast<std::uint16_t>(scan_mask)) == 0U) {
      continue;
    }
    if (!mission.carrying_resources || mission.cargo_type_id == -1) {
      continue;
    }

    const bool fails_on_scan = (mission.flags_primary & 0x20U) != 0U;
    if (!fails_on_scan || runtime.is_failed) {
      // Warning/fine arm: the mission is not failed.
      std::string message =
          ScanPrefix(state, MissionScanName(state, ship, mission));
      message += ScanString(0x178);
      message += " ";
      message += ScanString(0x179);
      message += " ";
      message += ScanString(0x17a);
      message += " ";
      message += ScanString(0x17b);
      message += ".";
      ship.primary_target_ship_slot = 0;
      ship.ai_secondary_target_slot = -1;
      ship.ai_state_code = 4;
      if (govt->scan_fine != 0) {
        const std::int32_t fine =
            govt->scan_fine > 0 ? static_cast<std::int32_t>(govt->scan_fine)
                                : ContrabandPercentageFine(
                                      govt->scan_fine, state.player.credits);
        AppendFineText(message, fine);
        ApplyScanFine(state, fine);
      }
      ShowScanMessage(state, std::move(message), 164);
    } else {
      // Failure arm: fail the mission; the voice/message is suppressed by the
      // accept-time flag 0x400.
      runtime.is_failed = true;
      if ((runtime.flags_primary_at_accept & 0x400U) == 0U) {
        ShowScanMessage(state, ScanString(0x11e), 152);
      }
      Mission_FailMissionSlotQuick(
          state, static_cast<std::int16_t>(slot), now_ms);
      ship.primary_target_ship_slot = 0;
      ship.ai_secondary_target_slot = -1;
      ship.ai_state_code = 4;
    }
    break;
  }

  // --- Outfit scan (only when no junk is scannable), else junk scan. ---
  if (!state.inventory.has_scannable_junk) {
    if (!state.inventory.has_scannable_outfit) {
      return;
    }
    for (std::size_t i = 0; i < state.inventory.outfit_owned_count.size();
         ++i) {
      if (state.inventory.outfit_owned_count[i] <= 0) {
        continue;
      }
      if (i >= state.scenario.outfits.size()) {
        continue;
      }
      const Outfit &outfit = state.scenario.outfits[i];
      if ((static_cast<std::uint16_t>(outfit.scan_mask) &
           static_cast<std::uint16_t>(scan_mask)) == 0U) {
        continue;
      }
      NovaGovernment_ProcessFactionCombatEvent(state,
                                               player.current_system_id,
                                               ship.faction_or_government_id,
                                               0,
                                               -1);
      std::string message = ScanPrefix(state, ScannerName(state, ship));
      message += ScanString(0x177);
      message += " ";
      const bool singular = state.inventory.outfit_owned_count[i] == 1;
      if (singular) {
        message += ScanString(StartsWithVowel(outfit.lc_name) ? 0x18a : 0x189);
        message += " ";
        message += outfit.lc_name;
      } else {
        message += outfit.lc_plural;
      }
      message += " ";
      message += ScanString(0x17b);
      message += ".";
      ship.primary_target_ship_slot = 0;
      ship.ai_secondary_target_slot = -1;
      ship.ai_state_code = 4;
      // Outfit fines are flat-positive only; the percentage arm is not used.
      if (govt->scan_fine > 0) {
        const std::int32_t fine = govt->scan_fine;
        AppendFineText(message, fine);
        ApplyScanFine(state, fine);
      }
      ShowScanMessage(state, std::move(message), 164);
      state.inventory.has_scannable_outfit = false;
      return;
    }
    return;
  }

  for (std::size_t i = 0; i < state.inventory.junk_counts.size(); ++i) {
    if (state.inventory.junk_counts[i] <= 0) {
      continue;
    }
    if (i >= state.scenario.junk_defs.size()) {
      continue;
    }
    const JunkDef &junk = state.scenario.junk_defs[i];
    if ((static_cast<std::uint16_t>(junk.scan_mask) &
         static_cast<std::uint16_t>(scan_mask)) == 0U) {
      continue;
    }
    NovaGovernment_ProcessFactionCombatEvent(
        state, player.current_system_id, ship.faction_or_government_id, 0, -1);
    std::string message = ScanPrefix(state, ScannerName(state, ship));
    message += ScanString(0x178);
    message += " ";
    message += junk.lc_name;
    message += " ";
    message += ScanString(0x17a);
    message += " ";
    message += ScanString(0x17b);
    message += ".";
    ship.primary_target_ship_slot = 0;
    ship.ai_secondary_target_slot = -1;
    ship.ai_state_code = 4;
    if (govt->scan_fine != 0) {
      const std::int32_t fine =
          govt->scan_fine > 0
              ? static_cast<std::int32_t>(govt->scan_fine)
              : ContrabandPercentageFine(govt->scan_fine, state.player.credits);
      AppendFineText(message, fine);
      ApplyScanFine(state, fine);
    }
    ShowScanMessage(state, std::move(message), 164);
    state.inventory.has_scannable_junk = false;
    return;
  }
}

} // namespace game
