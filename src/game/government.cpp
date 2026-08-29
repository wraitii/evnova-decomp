#include "government.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "log.hpp"
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

// Ghidra 0x0040fd20 Government_IsShipEligibleForGovernmentAid. See the header
// for the branch description. The mission-fleet branch (random-encounter
// fleet defs) and the GovtDef +0x83 byte gate are deferred: the fleet defs
// are not modelled, and the +0x83 byte has no clean-room field (TODO(decomp)).
bool NovaGovernment_IsShipEligibleForGovernmentAid(const GameState &state,
                                                   const Ship &ship) {
  if (NovaAiShip_ShouldKeepPressingTarget(state, ship)) {
    return false;
  }
  if (ship.ai_target_ship_slot == 0) {
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

  const auto flee_threshold = [&](std::int16_t govt_id) -> std::int16_t {
    if (govt_id < 0 || static_cast<std::size_t>(govt_id) >=
                           state.scenario.governments.size()) {
      return 0;
    }
    return state.scenario.governments[static_cast<std::size_t>(govt_id)]
        .flee_shield_threshold;
  };

  // Xenophobic ship faction (flags_primary & 1): aid is admitted when the
  // system reputation clears the flee threshold of the system government
  // (or of government entry 0 when the system has no government).
  if ((govt.flags_primary & 0x0001U) != 0) {
    if (sys_govt == ship.faction_or_government_id) {
      if (flee_threshold(sys_govt) < rep) {
        return true;
      }
    } else if (sys_govt < 0) {
      if (flee_threshold(0) < rep) {
        return true;
      }
    } else if (flee_threshold(sys_govt) < rep) {
      return true;
    }
  }

  // The 0x0002 flag and the allied/hostile ladders all admit aid when
  // reputation + the relevant flee threshold stays negative (the original's
  // SBORROW4 comparison, i.e. rep + threshold < 0).
  if (sys_govt < 0) {
    if ((govt.flags_primary & 0x0002U) == 0) {
      return true;
    }
    if (rep + flee_threshold(static_cast<std::int16_t>(faction)) < 0) {
      return true;
    }
  } else if (NovaGovernment_AreGovtsAllied(
                 state.scenario, sys_govt, ship.faction_or_government_id)) {
    if (rep + flee_threshold(sys_govt) < 0) {
      return true;
    }
  } else if (NovaGovernment_AreGovtsHostileOrXenophobic(
                 state.scenario, sys_govt, ship.faction_or_government_id)) {
    if (rep + flee_threshold(sys_govt) < 0) {
      return true;
    }
  } else if ((govt.flags_primary & 0x0002U) == 0) {
    return true;
  } else if (rep + flee_threshold(sys_govt) < 0) {
    return true;
  }

  // Ghidra Government_IsShipEligibleForGovernmentAid (0x0040fd20) also returns
  // true when GovtDef +0x83 is set -- the final gate of the eligibility
  // branch. TODO(decomp(0x0040fd20)) skipped: no clean-room +0x83 field, so
  // the gate is not reproduced.
  return false;
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
