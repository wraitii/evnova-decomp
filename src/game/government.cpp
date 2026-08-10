#include "government.hpp"

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

  // TODO(decomp): GovtDef +0x83 byte gate (returns true when set) -- the
  // field has no clean-room counterpart yet.
  return false;
}

} // namespace game
