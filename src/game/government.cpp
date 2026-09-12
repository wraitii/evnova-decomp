#include "government.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "log.hpp"
#include "outfit.hpp"
#include "ship_ai.hpp"

namespace game {
namespace {

// Mirrors the original's range guard (`-1 < id && id < 0x100`) and derelict
// exclusion (flags_primary & 0x0800) shared by both relation helpers. Returns
// the government entry for a zero-based id when it is valid and not derelict,
// else nullptr.
[[nodiscard]] const Government *RelationEligible(const ScenarioData &scenario,
                                                 std::int16_t govt_id) {
  if (govt_id < 0 || govt_id >= 0x100) {
    return nullptr;
  }
  const auto idx = static_cast<std::size_t>(govt_id);
  if (idx >= scenario.governments.size()) {
    return nullptr;
  }
  const Government &g = scenario.governments[idx];
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
  const Government *a = RelationEligible(scenario, govt_a);
  const Government *b = RelationEligible(scenario, govt_b);
  // Direct enemy-class hostility check (both directions), only when both sides
  // are relation-eligible (non-derelict, in range).
  if (a != nullptr && b != nullptr) {
    for (std::size_t i = 0; i < 4; ++i) {
      if (a->classes[i] != -1) {
        for (std::size_t j = 0; j < 4; ++j) {
          if (b->enemy_classes[j] == a->classes[i]) {
            return true;
          }
        }
      }
      if (b->classes[i] != -1) {
        for (std::size_t j = 0; j < 4; ++j) {
          if (a->enemy_classes[j] == b->classes[i]) {
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
  if (stellar.hazard_marker) {
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
  return stellar.field_0x47 != 0 &&
         (ship.ship_instance_id == 0 || ship.squad_leader_ship_slot == 0);
}

// Ghidra 0x0040fd20 Government_IsShipEligibleForGovernmentAid. See the header
// for the branch description. The mission-fleet branch (random-encounter
// fleet defs) and the GovtDef +0x83 byte gate are deferred: the fleet defs
// are not modelled, and the +0x83 byte has no clean-room field (TODO(decomp)).
bool NovaGovernment_IsShipEligibleForGovernmentAid(const GameState &state,
                                                   const Ship &ship) {
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
  if (ship.mission_fleet_slot != -1) {
    // Random-encounter fleet def branch (fleet escort flags 3/4): not
    // modelled; ships without a modelled fleet slot never reach here.
    NovaLog::Todo("government-aid: mission-fleet branch not reconstructed "
                  "(fleet defs not modelled)");
    return false;
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
  const System *sys = state.scenario.System(sys_id);
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

  // Xenophobic ship faction (flags_primary & 1): aid is admitted when the
  // system reputation clears the crime tolerance of the system government
  // (or of government entry 0 when the system has no government).
  if ((govt.flags_primary & 0x0001U) != 0) {
    if (sys_govt == ship.faction_or_government_id) {
      if (crime_tolerance(sys_govt) < rep) {
        return true;
      }
    } else if (sys_govt < 0) {
      if (crime_tolerance(0) < rep) {
        return true;
      }
    } else if (crime_tolerance(sys_govt) < rep) {
      return true;
    }
  }

  // The 0x0002 flag and the allied/hostile ladders all admit aid when
  // reputation + the relevant crime tolerance stays negative (the original's
  // SBORROW4 comparison, i.e. rep + crime_tol < 0).
  if (sys_govt < 0) {
    if ((govt.flags_primary & 0x0002U) == 0) {
      return true;
    }
    if (rep + crime_tolerance(static_cast<std::int16_t>(faction)) < 0) {
      return true;
    }
  } else if (NovaGovernment_AreGovtsAllied(
                 state.scenario, sys_govt, ship.faction_or_government_id)) {
    if (rep + crime_tolerance(sys_govt) < 0) {
      return true;
    }
  } else if (NovaGovernment_AreGovtsHostileOrXenophobic(
                 state.scenario, sys_govt, ship.faction_or_government_id)) {
    if (rep + crime_tolerance(sys_govt) < 0) {
      return true;
    }
  } else if ((govt.flags_primary & 0x0002U) == 0) {
    return true;
  } else if (rep + crime_tolerance(sys_govt) < 0) {
    return true;
  }

  // Ghidra Government_IsShipEligibleForGovernmentAid (0x0040fd20) also returns
  // true when GovtDef +0x83 is set -- the final gate of the eligibility
  // branch. TODO(decomp(0x0040fd20)) skipped: no clean-room +0x83 field, so
  // the gate is not reproduced.
  return false;
}

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

// Ghidra 0x00440750 Government_ApplyReputationCreditDelta. The mission
// PayVal opcode (EV Nova Bible, mïsn "PayVal"):
//   1..            flat credit award (inventory/loadout latch dirtied)
//   -1..-9999      no effect
//   -10128-g ..    systems of government g (and, in the -20xxx/-30xxx
//   -20128-g /     ally/class variants) have negative reputation cleared
//   -30128-g
//   -40001..-40099 deduct this percentage of the player's cash
//   <= -40100      no effect (the -50000- range is consumed at acceptance)
// Reputation/cash rounding matches the original's x87 FISTP half-to-even
// behavior; DAT_00575500 = 0.5 and DAT_00575508 = 0.01.
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
    state.stat_cache_valid = false;
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
    const double adjusted = static_cast<double>(state.player.credits) -
                            static_cast<double>(state.player.credits) *
                                static_cast<double>(percent) * 0.01;
    state.player.credits = static_cast<std::int32_t>(std::lrint(adjusted));
    return;
  }
}

} // namespace game
