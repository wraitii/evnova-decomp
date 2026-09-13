#include "mission.hpp"

#include "../brgr_archive.hpp"
#include "boarding_plunder.hpp"
#include "game_state.hpp"
#include "government.hpp"
#include "hud_overlay.hpp"
#include "log.hpp"
#include "mission_script.hpp"
#include "outfit.hpp"
#include "rank.hpp"
#include "ship_ai.hpp"
#include "ship_spawn.hpp"
#include "targeting.hpp"
#include "travel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>

namespace game {
namespace {

constexpr std::int16_t kResourceIdBase = 0x80;

// Mirrors NovaRandom_Range (0x004683b0) -> integer in [0, n), drawn from
// GameState.rng so runs stay reproducible (see ship_spawn.cpp).
[[nodiscard]] std::int16_t RandomBelow(GameState &state, std::int32_t bound) {
  if (bound <= 0) {
    return 0;
  }
  std::uniform_int_distribution<std::int32_t> dist{0, bound - 1};
  return static_cast<std::int16_t>(dist(state.rng));
}

// Draws the 1-based STR# entry for a mission name pool, mirroring the
// Mission_PopulateMissionSlotFromDef (0x0043f8c0) fleet-name rolls
// (NovaRandom_Range(count) + 1). Returns -1 when the mïsn pool id is
// unset or the pool is missing/empty (the original leaves the entry at -1),
// so callers can assign unconditionally over reused slots.
[[nodiscard]] std::int16_t RollMissionStringPoolEntry(GameState &state,
                                                      std::int16_t pool_id) {
  if (pool_id < 0) {
    return -1;
  }
  const std::uint16_t count =
      NovaHud_StringPoolEntryCount(static_cast<std::uint16_t>(pool_id));
  if (count == 0) {
    return -1;
  }
  return static_cast<std::int16_t>(RandomBelow(state, count) + 1);
}

// Reads a NUL-terminated 255-byte text/script buffer (MisnActive text blocks
// copied from the mïsn payload) as a string_view.
[[nodiscard]] std::string_view
TextOf(const std::array<std::byte, 255> &buffer) {
  const auto *begin = reinterpret_cast<const char *>(buffer.data());
  std::size_t length = 0;
  while (length < buffer.size() && begin[length] != '\0') {
    ++length;
  }
  return {begin, length};
}

[[nodiscard]] ControlExpressionState
MissionControlExpressionState(const GameState &state) {
  ControlExpressionState expression;
  expression.get_control_bit = [&state](std::uint32_t bit) {
    return state.control.ControlBit(bit);
  };
  expression.is_registered = [&state](std::uint32_t) {
    return state.control.registered;
  };
  expression.is_male = [&state] { return state.control.male; };
  expression.owns_outfit = [&state](std::int16_t id) {
    return Outfit_PlayerHasOutfitForControlExpression(state, id);
  };
  expression.has_explored = [&state](std::int16_t id) {
    return id >= 0 && id < 0x800 &&
           state.control.explored_systems.test(static_cast<std::size_t>(id));
  };
  return expression;
}

[[nodiscard]] bool IsActiveMission(const GameState &state,
                                   std::int16_t mission_id) {
  for (std::size_t slot = 0; slot < state.active_missions.size(); ++slot) {
    if (state.active_mission_runtime_flags[slot].is_active &&
        state.active_missions[slot].mission_template_id == mission_id) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool
Mission_PassesAcceptanceResourceGates(const GameState &state,
                                      const MissionDef &definition) {
  if (definition.cargo_qty_tons > 0 && state.player.ship_class_id >= 0) {
    const auto *ship_class = state.scenario.Ship(static_cast<std::int16_t>(
        state.player.ship_class_id + kResourceIdBase));
    if (ship_class != nullptr) {
      // Mission_ActivateMissionAtSlot checks both the ship's total cargo
      // capacity and its remaining free cargo space before opening its
      // original error dialog.
      const std::int32_t total_capacity =
          Outfit_ComputePlayerTotalCargoCapacity(state);
      if (total_capacity < definition.cargo_qty_tons ||
          Outfit_ComputeRemainingCargoSpace(state) <
              definition.cargo_qty_tons) {
        return false;
      }
    }
  }
  // Mission_ActivateMissionAtSlot charges the amount below -50000 as an
  // acceptance cost. The remaining positive/neutral values are rewards or
  // deferred accounting and do not block the BBS entry.
  if (definition.resource_delta_or_cost < -50000 &&
      state.player.credits < -50000 - definition.resource_delta_or_cost) {
    return false;
  }
  return true;
}

[[nodiscard]] std::int16_t ResolveContainingSystem(const GameState &state,
                                                   std::int16_t stellar_id) {
  if (stellar_id < 0 ||
      stellar_id >= static_cast<std::int16_t>(state.scenario.stellars.size())) {
    return -1;
  }
  return state.scenario.stellars[static_cast<std::size_t>(stellar_id)]
      .system_id;
}

} // namespace

// Ghidra 0x00448090 NovaResources_EvaluateAvailability, stellar/system
// membership prologue: every stellar's runtime owning-system slot is reset,
// each SystemDef.is_visible is recomputed from the Visibility NCB (the
// has_explored_flag gate passes for every loaded syst - the loader sets the
// flag at decode time), then every visible system re-homes its nav stellars
// whose slot is still unset (first visible system wins; a system claims its
// hidden visibility-parent chain with it). Systems hidden by the NCB keep
// system_id = -1 on their stellars and are therefore invisible to
// locator-family resolution. External linkage: the flight loop, the starmap
// and the new-game flow all re-run it outside mission.cpp.
// The mïsn availability-expression arm of 0x00448090 runs inline in
// Mission_EvaluateMissionLists.
void NovaResources_EvaluateAvailability(GameState &state) {
  const ControlExpressionState expression =
      MissionControlExpressionState(state);
  for (auto &stellar : state.scenario.stellars) {
    stellar.system_id = -1;
  }
  auto &systems = state.scenario.systems;
  std::vector<bool> claimed(systems.size(), false);
  for (std::size_t i = 0; i < systems.size(); ++i) {
    auto &system = systems[i];
    if (!system.has_explored_flag) {
      system.is_visible = false;
      continue;
    }
    system.is_visible =
        NovaControlExpression_Evaluate(system.visibility_expr, expression);
  }
  for (std::size_t i = 0; i < systems.size(); ++i) {
    auto &system = systems[i];
    if (!system.has_explored_flag || !system.is_visible || claimed[i]) {
      continue;
    }
    claimed[i] = true;
    for (std::int16_t parent = system.visible_parent_system_id;
         parent >= 0 && static_cast<std::size_t>(parent) < systems.size();
         parent = systems[static_cast<std::size_t>(parent)]
                      .visible_parent_system_id) {
      claimed[static_cast<std::size_t>(parent)] = true;
    }
    for (const std::int16_t nav : system.nav_defs) {
      if (nav < kResourceIdBase || nav >= kResourceIdBase + 0x800) {
        continue;
      }
      const std::size_t stellar_index =
          static_cast<std::size_t>(nav - kResourceIdBase);
      if (stellar_index < state.scenario.stellars.size() &&
          state.scenario.stellars[stellar_index].system_id == -1) {
        state.scenario.stellars[stellar_index].system_id =
            static_cast<std::int16_t>(i);
      }
    }
  }
  // The per-frame display pass (scope 3 of
  // System_UpdateSystemAndStellarDisplayState 0x00432470) refreshes
  // is_available/hazard_marker from the new membership map; the original
  // relies on the last per-tick pass having run with the same map.
  NovaTargeting_UpdateStellarAvailability(state);

  // Tail of 0x00448090: if the player's current system just failed its
  // Visibility NCB, resolve it to the visible member of its twin group and
  // relocate everything there; if the whole group is invisible, force the
  // discovery slot visible so the pilot is never stranded in hidden space.
  const std::int16_t current = state.player.current_system_id;
  if (current < 0 ||
      static_cast<std::size_t>(current) >= state.scenario.systems.size()) {
    return;
  }
  if (!state.scenario.systems[static_cast<std::size_t>(current)].is_visible) {
    std::int16_t resolved = NovaSystem_ResolveVisibleForTravel(state, current);
    if (resolved < 0) {
      resolved = NovaSystem_ResolveDiscoverySlot(state, current);
      if (resolved < 0) {
        resolved = current;
      }
      NovaLog::Warn(
          "current system {} invisible with no visible twin; forcing slot "
          "visible",
          current);
      state.scenario.systems[static_cast<std::size_t>(resolved)].is_visible =
          true;
    }
    state.player.current_system_id = resolved;
    // TODO(decomp): the original also rewrites every active ship state's and
    // shot's current-system field to the resolved system (0x0044821a).
  }
}

namespace {

// Mission_SelectMissionStellarByLocator (0x0043d510) filters all stellars
// against a locator family. The clean-room model has no separate travel graph
// yet, so availability/system membership remain the verified gates; the
// owning-system visibility gate and the random-destination persistence rule
// (Mission_IsStellarValidRandomDestination, 0x00468b50) are applied here. The
// reference/current stellar (the original's param_2) is not plumbed through
// this helper, so the persistence check runs the chain-only arm.
[[nodiscard]] std::vector<std::int16_t> CollectStellarLocatorCandidates(
    const GameState &state, std::int16_t locator, std::int16_t excluded) {
  const auto same_government_class = [&state](std::int16_t lhs,
                                              std::int16_t rhs,
                                              bool require_match) {
    if (lhs < 0 || rhs < 0 ||
        lhs >= static_cast<std::int16_t>(state.scenario.governments.size()) ||
        rhs >= static_cast<std::int16_t>(state.scenario.governments.size())) {
      return false;
    }
    const auto &left =
        state.scenario.governments[static_cast<std::size_t>(lhs)];
    const auto &right =
        state.scenario.governments[static_cast<std::size_t>(rhs)];
    for (const auto left_class : left.classes) {
      if (left_class < 0) {
        continue;
      }
      for (const auto right_class : right.classes) {
        if (left_class == right_class) {
          return require_match;
        }
      }
    }
    return !require_match;
  };
  const auto matches = [&](const Stellar &stellar) {
    if (!stellar.is_available || stellar.system_id < 0 ||
        stellar.system_id >=
            static_cast<std::int16_t>(state.scenario.systems.size()) ||
        (stellar.flags & 0x20U) != 0U) {
      return false;
    }
    const auto stellar_id =
        static_cast<std::int16_t>(&stellar - state.scenario.stellars.data());
    if (stellar_id == excluded) {
      return false;
    }
    if (!NovaSystem_IsSystemVisible(state, stellar.system_id)) {
      return false;
    }
    if (!Mission_IsStellarValidRandomDestination(state, stellar_id, -1)) {
      return false;
    }
    const auto govt = stellar.government_id;
    if (locator == -2) {
      return (stellar.flags & 0x10U) == 0U;
    }
    if (locator == -3) {
      return (stellar.flags & 0x10U) != 0U;
    }
    if (locator >= 10000 && locator < 15000) {
      return govt == static_cast<std::int16_t>(locator - 10000);
    }
    if (locator >= 15000 && locator < 20000) {
      return NovaGovernment_AreGovtsAllied(
          state.scenario, govt, static_cast<std::int16_t>(locator - 15000));
    }
    if (locator >= 20000 && locator < 25000) {
      return govt != static_cast<std::int16_t>(locator - 20000);
    }
    if (locator >= 25000 && locator < 30000) {
      return NovaGovernment_AreGovtsHostileOrXenophobic(
          state.scenario, govt, static_cast<std::int16_t>(locator - 25000));
    }
    if (locator >= 30000 && locator < 31000) {
      const auto wanted = static_cast<std::int16_t>(locator - 30000);
      return same_government_class(govt, wanted, true);
    }
    if (locator >= 31000 && locator < 32000) {
      const auto wanted = static_cast<std::int16_t>(locator - 31000);
      return same_government_class(govt, wanted, false);
    }
    return false;
  };

  std::vector<std::int16_t> candidates;
  for (std::size_t i = 0; i < state.scenario.stellars.size(); ++i) {
    if (matches(state.scenario.stellars[i])) {
      candidates.push_back(static_cast<std::int16_t>(i));
    }
  }
  return candidates;
}

[[nodiscard]] std::int16_t ResolveMissionStellar(GameState &state,
                                                 std::int16_t locator,
                                                 std::int16_t excluded,
                                                 std::int16_t fallback) {
  if (locator == -1 || locator == -4) {
    return fallback;
  }

  if (locator >= kResourceIdBase && locator < kResourceIdBase + 0x800) {
    const auto stellar_id =
        static_cast<std::int16_t>(locator - kResourceIdBase);
    if (stellar_id != excluded && stellar_id >= 0 &&
        stellar_id <
            static_cast<std::int16_t>(state.scenario.stellars.size())) {
      return stellar_id;
    }
    return fallback;
  }

  const std::vector<std::int16_t> candidates =
      CollectStellarLocatorCandidates(state, locator, excluded);
  if (candidates.empty()) {
    return fallback;
  }
  std::uniform_int_distribution<std::size_t> roll(0, candidates.size() - 1);
  return candidates[roll(state.rng)];
}

// Ghidra 0x00441b40 Mission_CheckMissionShipInteractionEligibility, offering
// slice (the BBS list builder calls it with the interaction context clear and
// param_2 = 0). Evaluates the original's ten definition-level gates in order:
// [0] AvailStel locator vs the selected/landed stellar, [1] cached
// availability expression, [2] AvailRecord vs system reputation, [3]
// AvailRating vs combat rating, [4] AvailRandom vs the per-definition roll,
// [5] free cargo space (Flags2 0x0001), [6] the 64-bit Require mask,
// [7] the Ship restriction (+0x5a), [8] Flags 0x2000/0x4000 ship-class arms,
// [9] the PayVal acceptance-credit gate, then the travel/return locator
// candidate sanity checks and the same-system denial arm. The earlier top
// gates (AvailStel < -31999, AvailRandom < 1, AvailLoc -1, AvailLoc-2 context,
// page-lane selection) also live here.
[[nodiscard]] bool CheckOfferingEligibility(const GameState &state,
                                            std::size_t def_index,
                                            std::int16_t page_group,
                                            bool interaction_context) {
  const MissionDef &def = state.scenario.missions[def_index];
  const auto def_id = static_cast<std::int16_t>(def_index);

  // ---- Top gates (0x00441b4f..) ------------------------------------------
  if (def.link_system_filter < -31999 || def.avail_random < 1 ||
      def.avail_location == -1) {
    return false;
  }
  // AvailLoc 2 (offered from a ship) only in the mission-ship interaction
  // context; in the list context AvailLoc selects the lane (0 = mission
  // computer, other = bar/services lane).
  if (def.avail_location == 2) {
    if (!interaction_context) {
      return false;
    }
  } else if (interaction_context) {
    return false;
  } else if ((page_group == 0) != (def.avail_location == 0)) {
    return false;
  }

  // ---- Gate 0: AvailStel locator vs the selected/landed stellar ----------
  // The original reads g_travel_selected_stellar_ptr / the player's
  // ai_secondary_target_slot (the travel slot); the port keeps the selected
  // travel stellar (0x80-based resource id) in GameState::travel.
  const std::int16_t selected_stellar = state.travel.selected_stellar_id;
  const std::int16_t selected_index =
      selected_stellar >= kResourceIdBase
          ? static_cast<std::int16_t>(selected_stellar - kResourceIdBase)
          : static_cast<std::int16_t>(-1);
  std::int16_t selected_govt = -1;
  if (selected_index >= 0 &&
      selected_index <
          static_cast<std::int16_t>(state.scenario.stellars.size())) {
    selected_govt =
        state.scenario.stellars[static_cast<std::size_t>(selected_index)]
            .government_id;
  }
  const auto same_government_class = [&](std::int16_t lhs, std::int16_t rhs) {
    if (lhs < 0 || rhs < 0 ||
        lhs >= static_cast<std::int16_t>(state.scenario.governments.size()) ||
        rhs >= static_cast<std::int16_t>(state.scenario.governments.size())) {
      return false;
    }
    const auto &left =
        state.scenario.governments[static_cast<std::size_t>(lhs)];
    const auto &right =
        state.scenario.governments[static_cast<std::size_t>(rhs)];
    for (const auto left_class : left.classes) {
      if (left_class < 0) {
        continue;
      }
      for (const auto right_class : right.classes) {
        if (left_class == right_class) {
          return true;
        }
      }
    }
    return false;
  };
  const std::int16_t filter = def.link_system_filter;
  if (filter != -1 && !interaction_context) {
    bool location_ok = false;
    if (filter >= kResourceIdBase && filter < kResourceIdBase + 0x800) {
      location_ok = selected_index == filter - kResourceIdBase;
    } else if (filter >= 5000 && filter <= 0x270e) {
      // Bible 5000-7047: a stellar in the system adjacent to the player's
      // current one (the binary scans the 16-entry link list).
      if (state.player.current_system_id >= 0 &&
          state.player.current_system_id <
              static_cast<std::int16_t>(state.scenario.systems.size())) {
        const auto adjacent_resource_id =
            static_cast<std::int16_t>(filter - 5000);
        const auto &current_system =
            state.scenario.systems[static_cast<std::size_t>(
                state.player.current_system_id)];
        location_ok =
            std::find(current_system.links.begin(),
                      current_system.links.end(),
                      adjacent_resource_id) != current_system.links.end();
      }
    } else if (filter >= 10000 && filter < 15000) {
      location_ok = selected_govt == filter - 10000;
    } else if (filter >= 15000 && filter < 20000) {
      location_ok =
          selected_govt != -1 && NovaGovernment_AreGovtsAllied(
                                     state.scenario,
                                     static_cast<std::int16_t>(filter - 15000),
                                     selected_govt);
    } else if (filter >= 20000 && filter < 25000) {
      location_ok = selected_govt != filter - 20000;
    } else if (filter >= 25000 && filter < 30000) {
      location_ok =
          selected_govt != -1 && NovaGovernment_AreGovtsHostileOrXenophobic(
                                     state.scenario,
                                     static_cast<std::int16_t>(filter - 25000),
                                     selected_govt);
    } else if (filter >= 30000 && filter < 31000) {
      location_ok = same_government_class(filter - 30000, selected_govt);
    } else if (filter >= 31000 && filter < 32000) {
      // Binary quirk (0x00441e26): the not-my-class lane subtracts 30000, not
      // 31000, so the id lands outside the govt table; the bounds-safe helper
      // therefore makes this lane pass whenever a governed stellar is
      // selected. Quirk preserved.
      location_ok = selected_govt != -1 &&
                    !same_government_class(filter - 30000, selected_govt);
    }
    if (!location_ok) {
      return false;
    }
  }

  // ---- Gate 1: cached availability expression ----------------------------
  if (!def.is_available_runtime) {
    return false;
  }

  // ---- Gate 2: AvailRecord vs the current system's reputation ------------
  if (def.avail_record != 0) {
    if (def.avail_record == -32000 || def.avail_record == -32001) {
      // Bible: offered when the player has dominated the (selected) stellar /
      // any stellar. Domination state is not modeled yet (TODO(decomp)):
      // fail open like the domination arms passing.
    } else {
      std::int16_t reputation = 0;
      if (state.player.current_system_id >= 0 &&
          state.player.current_system_id <
              static_cast<std::int16_t>(state.system_reputation.size())) {
        reputation = state.system_reputation[static_cast<std::size_t>(
            state.player.current_system_id)];
      }
      if (def.avail_record < 0 ? !(reputation <= def.avail_record)
                               : !(def.avail_record <= reputation)) {
        return false;
      }
    }
  }

  // ---- Gate 3: AvailRating vs combat rating ------------------------------
  if (def.avail_rating >= 1 &&
      def.avail_rating > state.player_combat_rating_points) {
    return false;
  }

  // ---- Gate 4: AvailRandom vs the per-definition warp roll ---------------
  if (def.avail_random < 100) {
    const std::int16_t roll = def_index < state.mission_offering_rolls.size()
                                  ? state.mission_offering_rolls[def_index]
                                  : 0;
    if (roll > def.avail_random) {
      return false;
    }
  }

  // ---- Gate 5: free cargo space ------------------------------------------
  // List context: only Flags2 0x0001 requests the check; interaction context
  // always requires room for the cargo load.
  if (interaction_context ? def.cargo_qty_tons >= 1
                          : (def.flags_secondary & 0x0001U) != 0U) {
    if (Outfit_ComputeRemainingCargoSpace(state) < def.cargo_qty_tons) {
      return false;
    }
  }

  // ---- Gate 6: 64-bit Require mask ---------------------------------------
  if (!NovaOutfit_EvaluateRequireMask(
          state, def.require_mask_lo, def.require_mask_hi)) {
    return false;
  }

  // ---- Gate 7: Ship restriction (+0x5a) ----------------------------------
  {
    const std::int16_t v = def.ship_restriction_filter;
    bool ship_ok = true;
    const ShipClass *player_class =
        state.player.ship_class_id >= 0
            ? state.scenario.Ship(static_cast<std::int16_t>(
                  state.player.ship_class_id + kResourceIdBase))
            : nullptr;
    const std::int16_t player_class_index = state.player.ship_class_id;
    if (v > 0x7f && v < 0x381) {
      ship_ok = player_class_index == v - 0x80;
    } else if (v >= 0x468 && v <= 0x768) {
      ship_ok = player_class_index != v - 0x468;
    } else if (v > 0x84f && v < 0x951) {
      const std::int16_t wanted = static_cast<std::int16_t>(v - 0x850);
      ship_ok = player_class != nullptr &&
                (player_class->inherent_combat_govt == wanted ||
                 player_class->inherent_attributes_govt == wanted);
    } else if (v >= 0xc38 && v <= 0xd38) {
      const std::int16_t wanted = static_cast<std::int16_t>(v - 0xc38);
      ship_ok = player_class == nullptr ||
                (player_class->inherent_combat_govt != wanted &&
                 player_class->inherent_attributes_govt != wanted);
    }
    if (!ship_ok) {
      return false;
    }
  }

  // ---- Gate 8: Flags 0x2000/0x4000 ship-class arms -----------------------
  {
    const ShipClass *player_class =
        state.player.ship_class_id >= 0
            ? state.scenario.Ship(static_cast<std::int16_t>(
                  state.player.ship_class_id + kResourceIdBase))
            : nullptr;
    const std::int16_t ai_behavior =
        player_class != nullptr ? player_class->default_ai_behavior : 0;
    if ((def.flags_primary & 0x2000U) != 0U && ai_behavior < 3) {
      return false;
    }
    if ((def.flags_primary & 0x4000U) != 0U && ai_behavior > 2) {
      return false;
    }
  }

  // ---- Gate 9: PayVal acceptance-credit gate -----------------------------
  if (def.resource_delta_or_cost < -50000 &&
      state.player.credits < -50000 - def.resource_delta_or_cost) {
    return false;
  }

  // ---- Travel/Return locator candidate sanity ----------------------------
  // A locator family with no reachable stellar denies the offering.
  const auto locator_has_candidate = [&](std::int16_t locator) {
    if (locator >= kResourceIdBase && locator < kResourceIdBase + 0x800) {
      const auto stellar_id =
          static_cast<std::int16_t>(locator - kResourceIdBase);
      return stellar_id >= 0 &&
             stellar_id <
                 static_cast<std::int16_t>(state.scenario.stellars.size());
    }
    if (locator == -2 || locator == -3 || locator > 0) {
      return !CollectStellarLocatorCandidates(state, locator, -1).empty();
    }
    return true;
  };
  if (!locator_has_candidate(def.travel_stellar_locator) ||
      !locator_has_candidate(def.return_stellar_locator)) {
    return false;
  }

  // ---- Same-system denial arm --------------------------------------------
  // When AvailStel is not a direct stellar, a TravelStel/ReturnStel inside the
  // player's current system (same visibility root) is never offered. The
  // binary resolves both systems through System_ResolveSystemDiscoverySlot
  // (0x0046b9b0), which maps hidden systems to their visibility root.
  const auto discovery_root = [&](std::int16_t system_id) -> std::int16_t {
    if (system_id < 0 ||
        system_id >= static_cast<std::int16_t>(state.scenario.systems.size())) {
      return -1;
    }
    const System &system =
        state.scenario.systems[static_cast<std::size_t>(system_id)];
    return system.visibility_root_system_id != -1
               ? system.visibility_root_system_id
               : system_id;
  };
  if (def.link_system_filter < kResourceIdBase ||
      def.link_system_filter >= kResourceIdBase + 0x800) {
    const std::int16_t player_root =
        discovery_root(state.player.current_system_id);
    for (const std::int16_t locator :
         {def.travel_stellar_locator, def.return_stellar_locator}) {
      if (locator > 0x7f && locator < 0x880) {
        const std::int16_t stellar_index =
            static_cast<std::int16_t>(locator - kResourceIdBase);
        if (stellar_index >= 0 &&
            stellar_index <
                static_cast<std::int16_t>(state.scenario.stellars.size())) {
          const std::int16_t destination_system =
              state.scenario.stellars[static_cast<std::size_t>(stellar_index)]
                  .system_id;
          if (destination_system >= 0 &&
              discovery_root(destination_system) == player_root) {
            return false;
          }
        }
      }
    }
  }

  // ---- Duplicate-active check --------------------------------------------
  return !IsActiveMission(state, def_id);
}

// Ghidra 0x0043cf00 Mission_EvaluateMissionLists lane builder. Collects every
// definition that passes the 0x00441b40 eligibility chain for `page_group`
// (0 = mission computer, 1 = bar/services lane) and orders it by list
// priority: the original's selection sort (0x0043d0c0 bucket pass) emits the
// highest MisnDef +0x128 priority first, ties in definition order.
[[nodiscard]] std::vector<std::int16_t>
EvaluateMissionPage(const GameState &state, std::int16_t page_group) {
  std::vector<std::int16_t> result;
  for (std::size_t index = 0; index < state.scenario.missions.size(); ++index) {
    const auto &mission = state.scenario.missions[index];
    // Absent definitions mirror the loader's zeroed AvailRandom, which the
    // eligibility top gate rejects.
    if (!mission.present) {
      continue;
    }
    if (CheckOfferingEligibility(state, index, page_group, false)) {
      result.push_back(static_cast<std::int16_t>(index));
    }
  }
  // Stable descending priority order (ties keep definition order).
  std::stable_sort(result.begin(), result.end(), [&](auto lhs, auto rhs) {
    const auto &left = state.scenario.missions[static_cast<std::size_t>(lhs)];
    const auto &right = state.scenario.missions[static_cast<std::size_t>(rhs)];
    return left.list_priority > right.list_priority;
  });
  return result;
}

// Ghidra 0x0043d4c0 Mission_ResolveMissionSpecialShipCount.
[[nodiscard]] std::int16_t ResolveSpecialShipCount(GameState &state,
                                                   std::int16_t encoded) {
  if (encoded >= 0) {
    return encoded;
  }
  if (encoded == -1) {
    return 0;
  }
  const auto magnitude = static_cast<std::int16_t>(-encoded);
  if (magnitude <= 0) {
    return 0;
  }
  std::uniform_int_distribution<int> roll(0, magnitude - 1);
  return static_cast<std::int16_t>(roll(state.rng) +
                                   (static_cast<int>(magnitude) + 1) / 2);
}

// Ghidra 0x0043d490 Mission_ResolveMissionSpecialShipSystem.
[[nodiscard]] std::int16_t ResolveSpecialShipSystem(GameState &state,
                                                    std::int16_t encoded) {
  if (encoded >= 0 && encoded < 1000) {
    return encoded;
  }
  if (encoded != 1000) {
    return -1;
  }
  std::uniform_int_distribution<int> roll(0, 5);
  return static_cast<std::int16_t>(roll(state.rng));
}

[[nodiscard]] std::int16_t ResolveMissionSystemByLocator(GameState &state,
                                                         std::int16_t locator,
                                                         std::int16_t fallback);

[[nodiscard]] std::int16_t
ResolveMissionCurrentSystem(GameState &state,
                            const MissionDef &definition,
                            const MissionTargetResolution &target) {
  const auto locator = definition.current_system_locator;
  if (locator == -1) {
    return state.player.current_system_id;
  }
  if (locator == -3) {
    return target.travel_system_id;
  }
  if (locator == -4) {
    return target.return_system_id;
  }
  if (locator == -2) {
    return ResolveMissionSystemByLocator(
        state, locator, state.player.current_system_id);
  }
  if (locator >= kResourceIdBase && locator < kResourceIdBase + 0x800) {
    return ResolveContainingSystem(
        state, static_cast<std::int16_t>(locator - kResourceIdBase));
  }
  return ResolveMissionSystemByLocator(state, locator, -1);
}

[[nodiscard]] std::int16_t SelectMissionShipType(GameState &state,
                                                 std::int16_t dude_id,
                                                 std::uint16_t flags) {
  if ((flags & 0x0800U) == 0 || dude_id < 0) {
    return -1;
  }
  const auto *dude =
      state.scenario.Dude(static_cast<std::int16_t>(dude_id + kResourceIdBase));
  if (dude == nullptr || !dude->present) {
    return -1;
  }
  // Mission_PopulateMissionSlotFromDef first permits available ship types,
  // then retries with the ignore-availability selector when none remain.
  const auto selected = NovaDude_SelectShipTypeIndex(*dude, false, state.rng);
  return selected >= 0 ? static_cast<std::int16_t>(selected)
                       : static_cast<std::int16_t>(NovaDude_SelectShipTypeIndex(
                             *dude, true, state.rng));
}

// Ghidra 0x0043e6f0 Mission_SelectMissionSystemByLocator.
[[nodiscard]] std::int16_t ResolveMissionSystemByLocator(
    GameState &state, std::int16_t locator, std::int16_t fallback) {
  if (locator >= kResourceIdBase && locator < kResourceIdBase + 0x800) {
    const auto system_id = static_cast<std::int16_t>(locator - kResourceIdBase);
    return system_id >= 0 && system_id < static_cast<std::int16_t>(
                                             state.scenario.systems.size())
               ? system_id
               : fallback;
  }
  const auto current = state.player.current_system_id;
  const auto choose_random = [&state](
                                 const std::vector<std::int16_t> &candidates,
                                 std::int16_t no_match) {
    if (candidates.empty()) {
      return no_match;
    }
    std::uniform_int_distribution<std::size_t> roll(0, candidates.size() - 1);
    return candidates[roll(state.rng)];
  };
  if (locator == -2) {
    std::vector<std::int16_t> candidates;
    for (std::size_t i = 0; i < state.scenario.systems.size(); ++i) {
      const auto &system = state.scenario.systems[i];
      if (static_cast<std::int16_t>(i) != current &&
          NovaSystem_IsSystemVisible(state, static_cast<std::int16_t>(i)) &&
          system.has_explored_flag) {
        candidates.push_back(static_cast<std::int16_t>(i));
      }
    }
    return choose_random(candidates, fallback);
  }
  if (locator == -5) {
    if (current < 0 ||
        current >= static_cast<std::int16_t>(state.scenario.systems.size())) {
      return fallback;
    }
    std::vector<std::int16_t> candidates;
    for (const auto linked_resource_id :
         state.scenario.systems[static_cast<std::size_t>(current)].links) {
      if (linked_resource_id < kResourceIdBase) {
        continue;
      }
      const auto linked =
          static_cast<std::int16_t>(linked_resource_id - kResourceIdBase);
      if (linked >= 0 &&
          linked < static_cast<std::int16_t>(state.scenario.systems.size()) &&
          NovaSystem_IsSystemVisible(state, linked)) {
        candidates.push_back(linked);
      }
    }
    return choose_random(candidates, fallback);
  }
  const auto same_class = [&state](std::int16_t lhs,
                                   std::int16_t rhs,
                                   bool require_same) {
    if (lhs < 0 || rhs < 0 ||
        lhs >= static_cast<std::int16_t>(state.scenario.governments.size()) ||
        rhs >= static_cast<std::int16_t>(state.scenario.governments.size())) {
      return false;
    }
    for (const auto left_class :
         state.scenario.governments[static_cast<std::size_t>(lhs)].classes) {
      for (const auto right_class :
           state.scenario.governments[static_cast<std::size_t>(rhs)].classes) {
        if (left_class >= 0 && left_class == right_class) {
          return require_same;
        }
      }
    }
    return !require_same;
  };
  const auto matches = [&](std::int16_t index) {
    const auto &system =
        state.scenario.systems[static_cast<std::size_t>(index)];
    if (!NovaSystem_IsSystemVisible(state, index) || system.government_id < 0) {
      return false;
    }
    const auto govt = system.government_id;
    if (locator >= 10000 && locator < 15000) {
      return govt == locator - 10000;
    }
    if (locator >= 15000 && locator < 20000) {
      return NovaGovernment_AreGovtsAllied(
          state.scenario, govt, locator - 15000);
    }
    if (locator >= 20000 && locator < 25000) {
      return govt != locator - 20000;
    }
    if (locator >= 25000 && locator < 30000) {
      return NovaGovernment_AreGovtsHostileOrXenophobic(
          state.scenario, govt, locator - 25000);
    }
    if (locator >= 30000 && locator < 31000) {
      return same_class(govt, locator - 30000, true);
    }
    if (locator >= 31000 && locator < 32000) {
      return same_class(govt, locator - 31000, false);
    }
    return false;
  };
  std::vector<std::int16_t> candidates;
  for (std::size_t i = 0; i < state.scenario.systems.size(); ++i) {
    if (static_cast<std::int16_t>(i) != current &&
        matches(static_cast<std::int16_t>(i))) {
      candidates.push_back(static_cast<std::int16_t>(i));
    }
  }
  return choose_random(candidates, fallback);
}

} // namespace

// Ghidra 0x00468b50 Mission_IsStellarValidRandomDestination.
bool Mission_IsStellarValidRandomDestination(const GameState &state,
                                             std::int16_t candidate,
                                             std::int16_t reference) {
  const auto &stellars = state.scenario.stellars;
  const auto &systems = state.scenario.systems;
  const auto stellar_in_range = [&stellars](std::int16_t id) {
    return id >= 0 && static_cast<std::size_t>(id) < stellars.size();
  };
  const auto system_in_range = [&systems](std::int16_t id) {
    return id >= 0 && static_cast<std::size_t>(id) < systems.size();
  };
  const auto system_of = [&stellars](std::int16_t stellar) {
    return stellars[static_cast<std::size_t>(stellar)].system_id;
  };

  if (!stellar_in_range(candidate)) {
    // The original returns true for a candidate outside g_stellar_defs.
    return true;
  }
  if (!stellars[static_cast<std::size_t>(candidate)].is_defined) {
    return false;
  }

  // Requires the candidate to be a nav default in every system of the
  // candidate system's visibility-parent (same-coordinate twin) chain.
  const auto chain_is_persistent = [&](std::int16_t start_system) {
    std::int16_t current = NovaSystem_ResolveDiscoverySlot(state, start_system);
    if (current < 0) {
      current = start_system;
    }
    std::int16_t visited = 0;
    std::int16_t present = 0;
    while (system_in_range(current)) {
      ++visited;
      const auto &nav_defs =
          systems[static_cast<std::size_t>(current)].nav_defs;
      if (std::find(nav_defs.begin(),
                    nav_defs.end(),
                    static_cast<std::int16_t>(candidate + 0x80)) !=
          nav_defs.end()) {
        ++present;
      }
      const std::int16_t parent =
          systems[static_cast<std::size_t>(current)].visible_parent_system_id;
      if (parent == -1) {
        break;
      }
      current = parent;
    }
    return visited > 0 && present == visited;
  };

  const std::int16_t candidate_system = system_of(candidate);
  if (!stellar_in_range(reference)) {
    return system_in_range(candidate_system) &&
           chain_is_persistent(candidate_system);
  }

  const std::int16_t reference_system = system_of(reference);
  if (!system_in_range(candidate_system) ||
      !system_in_range(reference_system)) {
    return false;
  }
  if (candidate_system == reference_system) {
    return false;
  }
  const std::int16_t candidate_slot =
      NovaSystem_ResolveDiscoverySlot(state, candidate_system);
  const std::int16_t reference_slot =
      NovaSystem_ResolveDiscoverySlot(state, reference_system);
  if (candidate_slot == reference_slot) {
    return false;
  }
  const auto &candidate_links = systems[candidate_system].links;
  const auto &reference_links = systems[reference_system].links;
  for (std::size_t j = 0; j < candidate_links.size(); ++j) {
    if (candidate_links[j] >= 0x80 &&
        NovaSystem_ResolveDiscoverySlot(
            state, static_cast<std::int16_t>(candidate_links[j] - 0x80)) ==
            reference_slot) {
      return false;
    }
    if (reference_links[j] >= 0x80 &&
        NovaSystem_ResolveDiscoverySlot(
            state, static_cast<std::int16_t>(reference_links[j] - 0x80)) ==
            candidate_slot) {
      return false;
    }
  }
  return chain_is_persistent(candidate_system);
}

// Ghidra 0x0043c3e0 Misn_ResolveMissionStellarLocators.
void Mission_ResolveMissionStellarLocators(GameState &state) {
  for (std::size_t index = 0; index < state.scenario.missions.size(); ++index) {
    const auto &definition = state.scenario.missions[index];
    if (!definition.present) {
      continue;
    }
    auto &target = state.mission_target_resolutions[index];
    target = {};
    // MisnDef +0x0c/+0x0e are the TravelStel/ReturnStel locators (mïsn
    // payload +0x0c/+0x0e); the "on_fail/on_success condition" naming was a
    // misnomer.
    target.travel_stellar_id =
        ResolveMissionStellar(state, definition.travel_stellar_locator, -1, -1);
    target.travel_system_id =
        ResolveContainingSystem(state, target.travel_stellar_id);
    target.return_stellar_id =
        definition.return_stellar_locator == -1
            ? target.travel_stellar_id
            : ResolveMissionStellar(state,
                                    definition.return_stellar_locator,
                                    target.travel_stellar_id,
                                    target.travel_stellar_id);
    target.return_system_id =
        ResolveContainingSystem(state, target.return_stellar_id);
    target.cargo_type_id =
        ResolveSpecialShipSystem(state, definition.cargo_type_resource);
    target.cargo_qty_tons =
        ResolveSpecialShipCount(state, definition.cargo_qty_tons);
    // The per-definition deadline for the offer-row <DL> token (0x0043d240
    // tail): today + TimeLimit, untouched (zeroed by the reset above) when
    // the mission has no TimeLimit.
    if (definition.time_limit_days > 0) {
      const GameDate deadline =
          Mission_ComputeDateAfterSteps(state, definition.time_limit_days);
      target.deadline_year = deadline.year;
      target.deadline_month = deadline.month;
      target.deadline_day = deadline.day;
    }
  }
}

// Ghidra 0x0043cf00 Mission_EvaluateMissionLists.
MissionListEvaluation Mission_EvaluateMissionLists(GameState &state) {
  // The original opens with NovaResources_EvaluateAvailability (0x00448090):
  // refresh stellar system membership + system visibility, then re-cache the
  // mïsn availability expressions (the loop below).
  NovaResources_EvaluateAvailability(state);
  const ControlExpressionState expression =
      MissionControlExpressionState(state);
  for (auto &mission : state.scenario.missions) {
    mission.is_available_runtime =
        mission.present && mission.link_system_filter != -32000 &&
        NovaControlExpression_Evaluate(mission.availability_expr, expression);
  }
  Mission_ResolveMissionStellarLocators(state);
  MissionListEvaluation result;
  // Two offering lanes per the original (g_mission_slot_list[2][1000],
  // evaluated with g_misn_list_page_group = 0 then 1 so the eligibility
  // AvailLoc gate filters per lane): lane 0 = mission computer, lane 1 =
  // bar/services locations.
  result.page_zero = EvaluateMissionPage(state, 0);
  result.page_one = EvaluateMissionPage(state, 1);
  // The original derives this from g_return_mission_list (built by the
  // landing/interaction pass), not from the bar lane. Provisional.
  result.has_return_mission = !result.page_one.empty();
  return result;
}

bool Mission_PopulateActiveSlot(GameState &state,
                                std::int16_t mission_id,
                                std::size_t active_slot) {
  if (active_slot >= GameState::kMaxActiveMissions) {
    return false;
  }
  // Ghidra's Mission_PopulateMissionSlotFromDef receives the zero-based
  // mission definition index; translate only when loading the mïsn resource.
  const auto definition_index = static_cast<std::int32_t>(mission_id);
  if (definition_index < 0 ||
      definition_index >=
          static_cast<std::int32_t>(state.scenario.missions.size())) {
    return false;
  }
  const auto *definition = state.scenario.Mission(
      static_cast<std::int16_t>(mission_id + kResourceIdBase));
  if (definition == nullptr || !definition->present) {
    return false;
  }

  auto &active = state.active_missions[active_slot];
  active = {};
  const auto &target =
      state.mission_target_resolutions[static_cast<std::size_t>(mission_id)];

  active.travel_stellar_id = target.travel_stellar_id;
  active.return_stellar_id = target.return_stellar_id;
  active.target_ship_count = definition->target_ship_count;
  active.dude_def_index = definition->special_ship_dude;
  if (active.dude_def_index >= kResourceIdBase) {
    active.dude_def_index =
        static_cast<std::int16_t>(active.dude_def_index - kResourceIdBase);
  }
  active.spawn_behavior = definition->spawn_behavior;
  active.fleet_spawn_goal = definition->fleet_spawn_goal;
  active.special_ship_spawn_mode = definition->special_ship_spawn_mode;
  active.current_system_id =
      ResolveMissionCurrentSystem(state, *definition, target);
  // Resolved Bible CargoType/CargoQty (m\xefsn +0x10/+0x12 via the target
  // table +0x04/+0x06); the prior names special_ship_system_id/count were
  // misnomers.
  active.cargo_type_id =
      target.cargo_type_id >= 0
          ? target.cargo_type_id
          : ResolveSpecialShipSystem(state, definition->cargo_type_resource);
  active.cargo_qty_tons =
      target.cargo_qty_tons > 0
          ? target.cargo_qty_tons
          : ResolveSpecialShipCount(state, definition->cargo_qty_tons);
  // Bible PickupMode/DropOffMode/ScanMask (payload +0x14/+0x16/+0x18);
  // the previous port wrongly filled these with resolved system ids.
  active.pickup_mode = definition->pickup_mode;
  active.drop_off_mode = definition->drop_off_mode;
  active.scan_mask = definition->scan_mask;
  active.comp_govt_id = definition->competing_government_id;
  if (active.comp_govt_id < kResourceIdBase || active.comp_govt_id > 0x17f) {
    active.comp_govt_id = -1;
  } else {
    active.comp_govt_id =
        static_cast<std::int16_t>(active.comp_govt_id - kResourceIdBase);
  }
  active.comp_reward_delta = definition->competing_reputation_delta;
  active.on_resolve_repeat_count = definition->on_resolve_repeat_count;
  active.flags_primary = definition->flags_primary;
  active.flags_secondary = definition->flags_secondary;
  active.resource_delta_or_cost = definition->resource_delta_or_cost;
  active.goal_counter_a = 0;
  active.goal_counter_b = 0;
  active.goal_counter_c = 0;
  active.goal_count_remaining = definition->target_ship_count;
  active.goal_counter_e = 0;
  active.mission_target_count = definition->target_ship_count;
  active.can_abort = definition->can_abort;
  active.carrying_resources = false;
  active.mission_template_id = mission_id;
  active.mission_ship_count_max = definition->mission_ship_count_max;
  active.aux_ships_dude_def_index = definition->auxiliary_ship_dude;
  if (active.aux_ships_dude_def_index >= kResourceIdBase) {
    active.aux_ships_dude_def_index = static_cast<std::int16_t>(
        active.aux_ships_dude_def_index - kResourceIdBase);
  }
  active.mission_ship_count_active = active.mission_ship_count_max;
  active.mission_fleet_metric_b = definition->mission_fleet_metric;
  active.mission_fleet_metric_c = 0;
  active.special_ship_type_index =
      SelectMissionShipType(state, active.dude_def_index, active.flags_primary);
  active.special_ship_name_string_id = definition->special_ship_name_string_id;
  active.random_text_string_id = definition->random_text_string_id;
  // Fleet-name rolls (0x0043f8c0): both name buffers start empty; when the
  // mïsn pool id is not -1 the pool id is latched and a random 1-based entry
  // is drawn (NovaRandom_Range(count) + 1). The entry stays -1 when the pool
  // is absent, and assigning unconditionally also clears any stale entry
  // from the slot's previous occupant. The <SN> wildcard arm re-resolves
  // the stored (pool, entry) pair (cf. MissionShipPoolString).
  active.special_ship_name_entry =
      RollMissionStringPoolEntry(state, active.special_ship_name_string_id);
  active.random_text_entry =
      RollMissionStringPoolEntry(state, active.random_text_string_id);
  // Desc ids +0x35..+0x43 (payload +0x34..+0x3e, +0x58, +0x44), normalized
  // to -1 when unset. Slots: Brief, QuickBrief, LoadCarg, DumpCargo, Comp,
  // Fail, aux (+0x41), ShipDone.
  const auto normalize_desc_id = [](std::int16_t id) {
    return id < 1 ? static_cast<std::int16_t>(-1) : id;
  };
  for (std::size_t i = 0; i < 6; ++i) {
    active.brief_description_ids[i] =
        normalize_desc_id(definition->text_description_ids[i]);
  }
  active.brief_description_ids[6] =
      normalize_desc_id(definition->slot_aux_text_id);
  active.brief_description_ids[7] =
      normalize_desc_id(definition->ship_done_text_id);
  active.brief_description_id = definition->initial_briefing_id;
  // TimeLimit seeds the deadline countdown; < 1 means no deadline (-32000).
  active.time_limit_days_remaining = definition->time_limit_days < 1
                                         ? static_cast<std::int16_t>(-32000)
                                         : definition->time_limit_days;
  if (definition->special_ship_spawn_mode < 1) {
    active.spawn_rearm_timer = -1;
  } else if (definition->fleet_spawn_goal == 1 &&
             definition->spawn_behavior == 3) {
    active.spawn_rearm_timer = 30;
  } else {
    std::uniform_int_distribution<int> roll(0, 99);
    active.spawn_rearm_timer = static_cast<std::int16_t>(100 + roll(state.rng));
  }
  std::uniform_int_distribution<int> rearm_roll(0, 69);
  active.rearm_roll_clock =
      static_cast<std::int16_t>(70 + rearm_roll(state.rng));
  const auto copy_text_buffer = [&](auto &destination,
                                    std::size_t source_offset) {
    if (source_offset < definition->raw_payload.size()) {
      const auto available = definition->raw_payload.size() - source_offset;
      std::copy_n(definition->raw_payload.begin() +
                      static_cast<std::ptrdiff_t>(source_offset),
                  std::min(destination.size(), available),
                  destination.begin());
    }
  };
  copy_text_buffer(active.on_accept_text, 0x15b);
  copy_text_buffer(active.mission_payload_text_b, 0x25a);
  copy_text_buffer(active.on_success_text, 0x359);
  copy_text_buffer(active.on_failure_text, 0x458);
  copy_text_buffer(active.resolve_script_buffer_start, 0x557);
  copy_text_buffer(active.state_latch, 0x660);
  std::copy(definition->raw_payload.begin(),
            definition->raw_payload.end(),
            active.raw_payload.begin());
  return true;
}

bool Mission_ActivateAtSlot(GameState &state,
                            std::int16_t mission_id,
                            std::int16_t landed_stellar_id) {
  if (mission_id < 0 ||
      mission_id >= static_cast<std::int16_t>(state.scenario.missions.size())) {
    return false;
  }
  const auto &definition =
      state.scenario.missions[static_cast<std::size_t>(mission_id)];
  if (!definition.present || IsActiveMission(state, mission_id) ||
      !Mission_PassesAcceptanceResourceGates(state, definition)) {
    return false;
  }
  std::size_t free_slot = GameState::kMaxActiveMissions;
  for (std::size_t slot = 0; slot < GameState::kMaxActiveMissions; ++slot) {
    if (!state.active_mission_runtime_flags[slot].is_active) {
      free_slot = slot;
      break;
    }
  }
  if (free_slot == GameState::kMaxActiveMissions ||
      !Mission_PopulateActiveSlot(state, mission_id, free_slot)) {
    return false;
  }

  auto &runtime = state.active_mission_runtime_flags[free_slot];
  runtime = {};
  runtime.is_active = true;
  runtime.flags_primary_at_accept =
      state.active_missions[free_slot].flags_primary;
  // Ghidra 0x0043f100: with a TimeLimit, the absolute deadline date
  // (today + TimeLimit days) is computed at acceptance into the runtime
  // flags (+0x06/+0x08/+0x0a) for the <DL> token.
  auto &active = state.active_missions[free_slot];
  if (active.time_limit_days_remaining > 0) {
    const GameDate deadline =
        Mission_ComputeDateAfterSteps(state, active.time_limit_days_remaining);
    runtime.deadline_year = deadline.year;
    runtime.deadline_month = deadline.month;
    runtime.deadline_day = deadline.day;
  }
  // Bible PickupMode 0: the mission cargo is aboard from mission start. The
  // LoadCargText desc dialog (payload +0x38) is UI-owned and runs in
  // NovaMission_RunAcceptanceDialogs (docked_mission_dialog.cpp) after
  // activation.
  if (active.pickup_mode == 0) {
    active.carrying_resources = true;
  }
  // The initial destination briefing is skipped when the mission has no
  // TravelStel, or when the player accepts it while docked there. Callers
  // pass `landed_stellar_id` as a 0x80-based resource id; resolved targets
  // are 0-based stellar indices (see Mission_TickReactionSlotsForTravel-
  // Interaction for the convention note).
  const std::int16_t landed_index =
      landed_stellar_id >= kResourceIdBase
          ? static_cast<std::int16_t>(landed_stellar_id - kResourceIdBase)
          : landed_stellar_id;
  runtime.initial_briefing_done = active.travel_stellar_id == -1 ||
                                  active.travel_stellar_id == landed_index;
  // The original treats values below -50000 as an immediate acceptance fee;
  // the encoded value includes the -50000 sentinel, so preserve its unusual
  // arithmetic and clamp the resulting credit balance at zero.
  const auto acceptance_value =
      state.active_missions[free_slot].resource_delta_or_cost;
  if (acceptance_value < -50000) {
    const auto charge = -50000 - acceptance_value;
    state.player.credits =
        std::max<std::int32_t>(0, state.player.credits - charge);
  }
  // Ghidra 0x0043f100: the on-accept payload (MisnActive +0x1ec, mïsn
  // payload +0x15b) runs at the end of the original activate function,
  // before the caller's acceptance dialogs. Tutorial missions use it to set
  // chain bits and reveal the destination system (X opcode).
  Mission_RunMisnScriptPayload(
      state,
      TextOf(state.active_missions[free_slot].on_accept_text),
      static_cast<std::int16_t>(free_slot));
  return true;
}

std::int16_t Misn_ResolveVisibleSystemForTravel(const GameState &state,
                                                std::int16_t system_id) {
  const auto count = static_cast<std::int16_t>(state.scenario.systems.size());
  if (system_id < 0 || system_id >= 0x800 || system_id >= count) {
    return -1;
  }
  const System &entry =
      state.scenario.systems[static_cast<std::size_t>(system_id)];
  const std::int16_t root = entry.visibility_root_system_id;
  if (root == -1) {
    return entry.is_visible ? system_id : -1;
  }
  std::int16_t current = root;
  while (current != -1) {
    // Bounds-guarded chain walk; the original trusts the loader-written ids.
    if (current < 0 || current >= count) {
      return -1;
    }
    if (state.scenario.systems[static_cast<std::size_t>(current)].is_visible) {
      return current;
    }
    current = state.scenario.systems[static_cast<std::size_t>(current)]
                  .visible_parent_system_id;
  }
  return -1;
}

// Ghidra 0x00447a30 Mission_DoesSystemMatchMissionLocator. Tests a system
// against an active mission's spawn locator (MisnActive +0x65, the m\xefsn
// mission-fleet locator copied from payload +0x4a):
//   -1 / -6      the player's current system
//   -2           the system containing the resolved TravelStel
//   -3           the system containing the resolved ReturnStel
//   0x80..0x87f  that system, or its visibility-remap target
//   5000..9999   that system (- 5000), or any of its 16 links
//   10000..14999 system government == code - 10000
//   15000..19999 system government == or allied to code - 15000
//   20000..24999 system government != code - 20000
//   25000..29999 system government hostile/xenophobic to code - 25000, or a
//                xenophobic government (flags_primary bit 0) not allied to it
//   30000..30999 shares a government class with code - 30000
//   31000..31999 does not share a government class with code - 31000
bool Mission_DoesSystemMatchMissionLocator(const GameState &state,
                                           std::int16_t system_id,
                                           std::int16_t mission_slot) {
  if (mission_slot < 0 ||
      mission_slot >= static_cast<std::int16_t>(state.active_missions.size()) ||
      system_id < 0 ||
      system_id >= static_cast<std::int16_t>(state.scenario.systems.size())) {
    return false;
  }
  const ActiveMission &mission = state.active_missions[mission_slot];
  const std::int16_t locator = mission.mission_fleet_metric_b;
  const System &system =
      state.scenario.systems[static_cast<std::size_t>(system_id)];
  const std::int16_t govt = system.government_id;

  if (locator == -1 || locator == -6) {
    return system_id == state.player.current_system_id;
  }
  if (locator == -2) {
    return mission.travel_stellar_id != -1 &&
           ResolveContainingSystem(state, mission.travel_stellar_id) ==
               system_id;
  }
  if (locator == -3) {
    return mission.return_stellar_id != -1 &&
           ResolveContainingSystem(state, mission.return_stellar_id) ==
               system_id;
  }
  if (locator >= kResourceIdBase && locator < kResourceIdBase + 0x800) {
    const auto target = static_cast<std::int16_t>(locator - kResourceIdBase);
    return system_id == target ||
           system_id == Misn_ResolveVisibleSystemForTravel(state, target);
  }
  if (locator >= 5000 && locator <= 9999) {
    if (system_id == static_cast<std::int16_t>(locator - 5000)) {
      return true;
    }
    const System *base = state.scenario.System(
        static_cast<std::int16_t>(locator - 5000 + kResourceIdBase));
    if (base == nullptr) {
      return false;
    }
    for (const std::int16_t link : base->links) {
      if (link >= kResourceIdBase &&
          system_id == static_cast<std::int16_t>(link - kResourceIdBase)) {
        return true;
      }
    }
    return false;
  }
  if (locator >= 10000 && locator < 15000) {
    return govt >= 0 && govt == static_cast<std::int16_t>(locator - 10000);
  }
  if (locator >= 15000 && locator < 20000) {
    return govt >= 0 && (govt == static_cast<std::int16_t>(locator - 15000) ||
                         NovaGovernment_AreGovtsAllied(
                             state.scenario,
                             govt,
                             static_cast<std::int16_t>(locator - 15000)));
  }
  if (locator >= 20000 && locator < 25000) {
    return govt != static_cast<std::int16_t>(locator - 20000);
  }
  if (locator >= 25000 && locator < 30000) {
    const auto other = static_cast<std::int16_t>(locator - 25000);
    if (govt < 0 || other < 0 ||
        other >= static_cast<std::int16_t>(state.scenario.governments.size())) {
      return false;
    }
    const Government &other_def =
        state.scenario.governments[static_cast<std::size_t>(other)];
    if ((other_def.flags_primary & 0x0001U) != 0U && other != govt &&
        !NovaGovernment_AreGovtsAllied(state.scenario, govt, other)) {
      return true;
    }
    return NovaGovernment_AreGovtsHostileOrXenophobic(
        state.scenario, govt, other);
  }
  if (locator >= 30000 && locator < 31000) {
    return govt >= 0 && NovaGovernment_DoGovtsShareClass(
                            state.scenario,
                            govt,
                            static_cast<std::int16_t>(locator - 30000));
  }
  if (locator >= 31000 && locator < 32000) {
    return govt >= 0 && !NovaGovernment_DoGovtsShareClass(
                            state.scenario,
                            govt,
                            static_cast<std::int16_t>(locator - 31000));
  }
  return false;
}

void Misn_TickActiveMissionTimers(GameState &state) {
  for (std::size_t slot = 0; slot < state.active_missions.size(); ++slot) {
    if (!state.active_mission_runtime_flags[slot].is_active) {
      continue;
    }
    ActiveMission &mission = state.active_missions[slot];
    const std::int16_t raw_system = mission.current_system_id;
    const std::int16_t resolved =
        (raw_system < 0 || raw_system >= 0x800)
            ? -1
            : Misn_ResolveVisibleSystemForTravel(state, raw_system);
    // A mission with live target ships whose destination is the player's
    // current system (or the "here" sentinel -6) rearms its spawn clock:
    // behavior-3 fleets on goal 1 use the fixed 30-tick clock, everything else
    // rolls 100..199. The goal-countdown latch clears with it.
    if (mission.target_ship_count > 0 &&
        ((resolved == state.player.current_system_id || raw_system == -6) &&
         mission.special_ship_spawn_mode == 1)) {
      if (mission.fleet_spawn_goal == 1 && mission.spawn_behavior == 3) {
        mission.spawn_rearm_timer = 30;
      } else {
        mission.spawn_rearm_timer =
            static_cast<std::int16_t>(RandomBelow(state, 100) + 100);
      }
      mission.goal_count_remaining = 0;
    }
    if ((mission.flags_primary & 0x10) != 0) {
      mission.mission_ship_count_active = mission.mission_ship_count_max;
    }
    mission.rearm_roll_clock =
        static_cast<std::int16_t>(RandomBelow(state, 0x46) + 0x46);
    mission.mission_fleet_metric_c = 0;
  }
}

bool Mission_CheckReactionConditionSatisfied(const GameState &state,
                                             std::string_view condition) {
  if (condition.empty()) {
    return true;
  }
  switch (condition.front()) {
  case 'b':
  case 'B':
  case '(':
  case '!':
  case 'P':
  case 'p':
  case 'G':
  case 'g':
  case 'O':
  case 'o':
  case 'E':
  case 'e':
    break;
  default:
    return false;
  }
  // The original copies the condition into the shared script scratch buffer,
  // inserting a space between adjacent open parens so the tokenizer sees
  // nested groups. The clean-room evaluator takes the string directly.
  std::string normalized;
  normalized.reserve(condition.size() + 1);
  char previous = ' ';
  for (const char c : condition) {
    if (c == '(' && previous == '(') {
      normalized.push_back(' ');
    }
    normalized.push_back(c);
    previous = c;
  }
  return NovaControlExpression_Evaluate(normalized,
                                        MissionControlExpressionState(state));
}

// Ghidra 0x00440aa0 Mission_ClearMisnSlotAssignments. Releases every ship
// assigned to the mission-fleet slot: clears its fleet link, restores the
// class-default AI behavior when it held a target, and re-enters AI state 2.
// While the travel scene owns the world (state.in_travel_scene, the
// original's g_travel_scene_ctx) the released ships are despawned instead:
// the landing pass runs while the destination window owns the world, so the
// released fleet must not linger into the flight scene.
void Mission_ClearMisnSlotAssignments(GameState &state,
                                      std::int16_t mission_slot,
                                      bool emit_completion_payload,
                                      std::uint32_t now_ms) {
  for (Ship &ship : state.ships_) {
    if (!ship.is_active || ship.mission_fleet_slot != mission_slot) {
      continue;
    }
    ship.mission_fleet_slot = -1;
    if (ship.squad_leader_ship_slot != -1) {
      const auto *ship_class = state.scenario.Ship(
          static_cast<std::int16_t>(ship.ship_class_id + kResourceIdBase));
      if (ship_class != nullptr) {
        ship.ai_behavior_code = ship_class->default_ai_behavior;
      }
      ship.squad_leader_ship_slot = -1;
      NovaAi_EnterState2ClearPrimaryTarget(ship, now_ms);
    }
    if (state.in_travel_scene) {
      ship.is_active = false;
    }
  }
  if (emit_completion_payload) {
    Mission_RunMisnScriptPayload(
        state,
        TextOf(state.active_missions[static_cast<std::size_t>(mission_slot)]
                   .resolve_script_buffer_start),
        mission_slot);
  }
  state.active_missions[static_cast<std::size_t>(mission_slot)]
      .carrying_resources = false;
  state.active_mission_runtime_flags[static_cast<std::size_t>(mission_slot)]
      .is_active = false;
  // The original also invalidates g_last_system_for_ambient_rolls (0xffff);
  // the clean-room ambient-roll cache latch is not modelled (TODO(decomp)).
}

// Ghidra 0x00440410 Mission_ResolveMissionSuccess.
void Mission_ResolveMissionSuccess(GameState &state,
                                   std::int16_t mission_slot,
                                   const MissionDebriefSink &debrief) {
  const auto slot = static_cast<std::size_t>(mission_slot);
  ActiveMission &mission = state.active_missions[slot];
  // MisnActive +0x3d selects the success debrief selection dialog
  // (Ui_LoadSelectionDialogResource + Stellar_BuildTravelDestination-
  // Description + Ui_RunTravelSelectionDialog). Composed while the slot is
  // still active so the wildcard pass reads the slot data. The dialog is
  // gated on +0x3d != -1; a missing resource yields empty text, which shows
  // no dialog (the original's empty-text arm).
  const std::int16_t comp_text_id = mission.brief_description_ids[4];
  std::string comp_text;
  std::int16_t comp_variant = 0;
  if (comp_text_id != -1) {
    if (const auto desc = NovaResource_LoadDescription(
            static_cast<std::uint16_t>(comp_text_id))) {
      comp_text = desc->text;
      comp_variant = desc->dialog_variant;
      if (!desc->status.empty()) {
        NovaLog::Todo("mission success debrief desc {} status string not "
                      "displayed (0x004982a0)",
                      comp_text_id);
      }
      Mission_ExpandStringPlaceholders(state, comp_text);
      comp_text =
          Mission_ExpandMissionWildcards(state, comp_text, false, mission_slot);
    }
  }
  if (comp_variant >= 0x80) {
    NovaLog::Todo("mission success debrief desc {} variant {:#x}: DLOG 0xbbc "
                  "+ PICT 0x214f art path not reconstructed",
                  comp_text_id,
                  comp_variant);
  }
  if (!comp_text.empty()) {
    if (debrief) {
      debrief(comp_text);
    } else {
      NovaLog::Todo("mission success debrief dialog (misn {} id {}) has no "
                    "UI sink",
                    mission.mission_template_id,
                    comp_text_id);
    }
  } else if (comp_text_id != -1) {
    NovaLog::Todo("mission success debrief dësc {} loaded empty", comp_text_id);
  }
  state.active_mission_runtime_flags[slot].is_active = false;
  Mission_RunMisnScriptPayload(
      state, TextOf(mission.on_success_text), mission_slot);
  // On-resolve repeat count (Bible DatePostInc) re-runs the daily world
  // update (0x00466cb0) once per count, advancing the calendar.
  for (std::int16_t i = 0; i < mission.on_resolve_repeat_count; ++i) {
    Mission_TickDailyWorldUpdate(state);
  }
  const std::int16_t govt = mission.comp_govt_id;
  const std::int16_t delta = mission.comp_reward_delta;
  if (govt >= 0 && govt < 0x100) {
    const auto count = static_cast<std::int16_t>(
        std::min<std::size_t>(state.system_reputation.size(), 0x800));
    for (std::int16_t i = 0; i < count; ++i) {
      const std::int16_t system_govt =
          state.scenario.systems[static_cast<std::size_t>(i)].government_id;
      auto &rep = state.system_reputation[static_cast<std::size_t>(i)];
      if (system_govt == govt) {
        rep = static_cast<std::int16_t>(rep + delta);
      } else if (system_govt != -1) {
        if (NovaGovernment_AreGovtsHostileOrXenophobic(
                state.scenario, system_govt, govt)) {
          // 0x00440410 truncates toward zero (x87 FIST + residual/sign
          // correction), not round-to-nearest.
          rep = static_cast<std::int16_t>(static_cast<float>(rep) -
                                          static_cast<float>(delta) * 0.5F);
        } else if (NovaGovernment_AreGovtsAllied(
                       state.scenario, system_govt, govt)) {
          rep = static_cast<std::int16_t>(static_cast<float>(rep) +
                                          static_cast<float>(delta) * 0.5F);
        }
      }
    }
  }
  NovaGovernment_ApplyReputationCreditDelta(state,
                                            mission.resource_delta_or_cost);
  // Ambient-roll latch invalidation is not modelled (TODO(decomp)).
}

// Ghidra 0x00440930 Mission_ResolveMissionFailure.
void Mission_ResolveMissionFailure(GameState &state,
                                   std::int16_t mission_slot,
                                   std::uint32_t now_ms,
                                   const MissionDebriefSink &debrief) {
  const auto slot = static_cast<std::size_t>(mission_slot);
  ActiveMission &mission = state.active_missions[slot];
  Mission_RunMisnScriptPayload(
      state, TextOf(mission.on_failure_text), mission_slot);
  const std::int16_t govt = mission.comp_govt_id;
  if (govt != -1) {
    // Failure subtracts half the reputation delta (integer division rounded
    // toward zero) from every system owned by the competing government.
    const std::int16_t half_delta =
        static_cast<std::int16_t>(mission.comp_reward_delta / 2);
    const auto count = static_cast<std::int16_t>(
        std::min<std::size_t>(state.system_reputation.size(), 0x800));
    for (std::int16_t i = 0; i < count; ++i) {
      const std::int16_t system_govt =
          state.scenario.systems[static_cast<std::size_t>(i)].government_id;
      if (system_govt == govt) {
        auto &rep = state.system_reputation[static_cast<std::size_t>(i)];
        rep = static_cast<std::int16_t>(rep - half_delta);
      }
    }
  }
  state.active_mission_runtime_flags[slot].is_active = false;
  // MisnActive +0x3f selects the failure debrief dialog. Composed after the
  // slot is torn down like the original, so the wildcard pass takes its
  // inactive arm and mission-specific tokens expand to "[Error]"
  // (Stellar_BuildTravelDestinationDescription 0x004444f0 jumps to the
  // shared tail when the slot is not active -- quirk kept). Gated on +0x3f
  // != -1; a missing resource yields empty text, which shows no dialog.
  const std::int16_t fail_text_id = mission.brief_description_ids[5];
  std::string fail_text;
  std::int16_t fail_variant = 0;
  if (fail_text_id != -1) {
    if (const auto desc = NovaResource_LoadDescription(
            static_cast<std::uint16_t>(fail_text_id))) {
      fail_text = desc->text;
      fail_variant = desc->dialog_variant;
      if (!desc->status.empty()) {
        NovaLog::Todo("mission failure debrief desc {} status string not "
                      "displayed (0x004982a0)",
                      fail_text_id);
      }
      Mission_ExpandStringPlaceholders(state, fail_text);
      fail_text =
          Mission_ExpandMissionWildcards(state, fail_text, false, mission_slot);
    }
  }
  if (fail_variant >= 0x80) {
    NovaLog::Todo("mission failure debrief desc {} variant {:#x}: DLOG 0xbbc "
                  "+ PICT 0x214f art path not reconstructed",
                  fail_text_id,
                  fail_variant);
  }
  if (!fail_text.empty()) {
    if (debrief) {
      debrief(fail_text);
    } else {
      NovaLog::Todo("mission failure debrief dialog (misn {} id {}) has no "
                    "UI sink",
                    mission.mission_template_id,
                    fail_text_id);
    }
  } else if (fail_text_id != -1) {
    NovaLog::Todo("mission failure debrief dësc {} loaded empty", fail_text_id);
  }
  Mission_ClearMisnSlotAssignments(state, mission_slot, false, now_ms);
}

bool NovaStellar_AreStellarsEquivalent(const GameState &state,
                                       std::int16_t stellar_a,
                                       std::int16_t stellar_b) {
  if (stellar_a < 0 || stellar_a >= 0x800 || stellar_b < 0 ||
      stellar_b >= 0x800) {
    return false;
  }
  if (stellar_a == stellar_b) {
    return true;
  }
  const Stellar *a = state.scenario.Stellar(
      static_cast<std::int16_t>(stellar_a + kResourceIdBase));
  const Stellar *b = state.scenario.Stellar(
      static_cast<std::int16_t>(stellar_b + kResourceIdBase));
  if (a == nullptr || b == nullptr) {
    return false;
  }
  // Duplicate-resource twins: same body position and same display name.
  return a->pos_x == b->pos_x && a->pos_y == b->pos_y && a->name == b->name;
}

bool Mission_TryConsumeMissionInteractionResources(GameState &state,
                                                   std::int16_t count) {
  if (count > 0) {
    if (Outfit_ComputePlayerTotalCargoCapacity(state) < count) {
      // The original shows the STR# 0x7d2 0x165 "not enough cargo space"
      // selection dialog. UI-owned; not reconstructed (TODO(decomp)).
      NovaLog::Todo("mission interaction denied: cargo capacity below {} tons",
                    count);
      return false;
    }
    if (Outfit_ComputeRemainingCargoSpace(state) < count) {
      // STR# 0x7d2 0x166 "not enough free cargo space" dialog.
      NovaLog::Todo("mission interaction denied: free cargo space below {} "
                    "tons",
                    count);
      return false;
    }
  }
  // g_playerInventoryAndLoadoutDirty + Outfit_RecomputeOutfitDerivedState.
  state.stat_cache_valid = false;
  return true;
}

// Ghidra 0x00440bf0 Mission_FailMissionSlotQuick. Immediate failure path:
// runs the failure payload, latches the failed flag, and releases assigned
// ships when the misn CanAbort latch is set (the original reuses that flag
// here as the fleet-release gate).
void Mission_FailMissionSlotQuick(GameState &state,
                                  std::int16_t mission_slot,
                                  std::uint32_t now_ms) {
  const auto slot = static_cast<std::size_t>(mission_slot);
  ActiveMission &mission = state.active_missions[slot];
  Mission_RunMisnScriptPayload(
      state, TextOf(mission.on_failure_text), mission_slot);
  state.active_mission_runtime_flags[slot].is_failed = true;
  if (mission.can_abort) {
    Mission_ClearMisnSlotAssignments(state, mission_slot, false, now_ms);
  }
  // Ambient-roll latch invalidation is not modelled (TODO(decomp)).
}

// Ghidra 0x00447d90 Mission_ResolveMisnSlot. Completes an auto-abort/goal
// mission: resolve payload, optional daily rerolls, the auto-abort fuel
// penalty, auto-abort pay, and slot teardown.
void Mission_ResolveMisnSlot(GameState &state,
                             std::int16_t mission_slot,
                             std::uint32_t now_ms) {
  const auto slot = static_cast<std::size_t>(mission_slot);
  ActiveMission &mission = state.active_missions[slot];
  Mission_RunMisnScriptPayload(
      state, TextOf(mission.resolve_script_buffer_start), mission_slot);
  // On-resolve repeat count (Bible DatePostInc) re-runs the daily world
  // update (0x00466cb0) once per count, advancing the calendar.
  for (std::int16_t i = 0; i < mission.on_resolve_repeat_count; ++i) {
    Mission_TickDailyWorldUpdate(state);
  }
  // Bible m\x89sn Flags 0x0008: auto-abort drains 100 units of fuel
  // (DAT_00575510). The g_player_fuel_panel_dirty latch has no clean-room
  // equivalent; the HUD recomputes fuel every frame.
  if ((mission.flags_primary & 0x0008U) != 0U) {
    state.player.fuel_points -= 100.0F;
  }
  // Bible Flags2 0x0002: apply the mission pay on auto-abort.
  if ((mission.flags_secondary & 0x0002U) != 0U) {
    NovaGovernment_ApplyReputationCreditDelta(state,
                                              mission.resource_delta_or_cost);
  }
  Mission_ClearMisnSlotAssignments(state, mission_slot, false, now_ms);
}

// Ghidra 0x00458802 (inside Stellar_ProcessTravelAndLanding): draws the
// per-definition offering roll (NovaRandom_Range(100) + 1, i.e. 1..100) for
// every mission definition, then re-runs Mission_EvaluateMissionLists. Called
// at game start and on every system arrival; the port evaluates lists on
// demand, so only the rolls are refreshed here.
void Mission_RerollOfferingRolls(GameState &state) {
  std::uniform_int_distribution<int> roll(1, 100);
  const std::size_t count = std::min<std::size_t>(
      state.mission_offering_rolls.size(), state.scenario.missions.size());
  for (std::size_t i = 0; i < count; ++i) {
    state.mission_offering_rolls[i] =
        static_cast<std::int16_t>(roll(state.rng));
  }
}

// Ghidra 0x00448670 Mission_TriggerReturnMissionInteractions. See the header
// comment. The original walks the persistent lane-1 list (g_return_mission_list
// = g_mission_slot_list[1], rebuilt by Mission_EvaluateMissionLists on every
// arrival); the port rebuilds the lane fresh here with the same page-group
// gate, which also re-applies the eligibility chain the original re-checks at
// walk time. The original's walk call passes the interaction flag, but the
// Spaceport loop clears the interaction context before entering, so the list-
// context gate set applies.
bool Mission_TriggerLandingInteractions(
    GameState &state,
    std::int16_t context,
    std::uint32_t now_ms,
    const std::function<MissionOfferResult(std::int16_t)> &run_offer) {
  // Context latch (DAT_00774ae2): switching to any context other than 3
  // clears the per-definition shown latches, so a mission declined at one
  // location type is offered again at the next matching landing.
  if (context != state.mission_interaction_context) {
    if (context != 3) {
      state.mission_interaction_shown.fill(0);
    }
    state.mission_interaction_context = context;
  }

  // Walk the lane-1 list for the first definition whose AvailLoc == context
  // that has not been shown yet. The original walks the lane list rebuilt by
  // Mission_EvaluateMissionLists (which also re-caches the availability
  // expressions); the port rebuilds it fresh here, and the shown latch is
  // applied exactly as the original's DAT_00773eed check.
  const std::vector<std::int16_t> lane =
      Mission_EvaluateMissionLists(state).page_one;
  std::int16_t candidate = -1;
  for (const std::int16_t def : lane) {
    if (def < 0 ||
        def >= static_cast<std::int16_t>(state.scenario.missions.size())) {
      continue;
    }
    if (state.scenario.missions[static_cast<std::size_t>(def)].avail_location ==
            context &&
        state.mission_interaction_shown[static_cast<std::size_t>(def)] == 0) {
      candidate = def;
      break;
    }
  }
  if (candidate < 0) {
    return false;
  }

  const MissionOfferResult result = run_offer(candidate);
  if (result == MissionOfferResult::kActivationFailed) {
    // The original latches only the -1 (activation-failed) return; an accept
    // removes the entry from the offering list and a plain decline leaves it
    // re-offerable (see 0x00448670's -1 arm).
    state.mission_interaction_shown[static_cast<std::size_t>(candidate)] = 1;
  }
  // Recheck timer: DAT_00776af4 = NovaTime_GetTickCount60Hz() +
  // NovaRandom_Range(30)
  // + 30. Consumed by the services windows; stored for the future consumers.
  state.mission_interaction_recheck_at_ms =
      static_cast<std::int32_t>(now_ms) +
      static_cast<std::int32_t>(RandomBelow(state, 0x1e)) + 0x1e;
  return true;
}

// Ghidra 0x0044a4d0 Ship_ExpandStringPlaceholders. See the header comment for
// the grammar. Character-state machine over the text (the original walks the
// shared DAT_007c8a10 buffer in place):
//   0 copy-through ('{' -> 1)   1 header: g/G/p/P/b/B/'!'
//   2 digit accumulation        3 scan to opening quote of the chosen arm
//   4 copy arm (\\-escapes)     5 scan past the opening quote (rejected arm)
//   6 skip rejected arm         7 post-arm: '}' ends, '"' copies another arm
//   8 discard until '}'
void Mission_ExpandStringPlaceholders(const GameState &state,
                                      std::string &text) {
  std::string out;
  out.reserve(text.size());
  int machine = 0;
  bool negate = false; // '!' seen in the current header (never reset: quirk)
  bool escaped = false;
  int count = 0;
  char condition = 'P'; // 'P' = {pN} registration, 'B' = {bN} control bit
  for (const char ch : text) {
    switch (machine) {
    case 1:
      if (ch == 'g' || ch == 'G') {
        // DAT_00734c1c ('m' latch): 1 = male -> first arm; '!' swaps.
        const bool male = state.control.male != negate;
        machine = male ? 3 : 5;
      } else if (ch == 'p' || ch == 'P') {
        count = 0;
        condition = 'P';
        machine = 2;
      } else if (ch == 'b' || ch == 'B') {
        count = 0;
        condition = 'B';
        machine = 2;
      } else if (ch == '!') {
        negate = true;
      }
      // Any other header character is swallowed and the machine stays in 1
      // (the original has no terminator branch in this state).
      break;
    case 2: {
      if (ch >= '0' && ch <= '9') {
        count = count * 10 + (ch - '0');
        break;
      }
      // Condition bodies (Bible dësc grammar):
      //   {pN}/{PN}: registration test. The original refuses the first arm
      //     only when the current expression ship class is unlicensed
      //     (is_licensed_runtime == 0) and the shareware day counter has
      //     reached N (ncb Pxxx semantics); the licensed game always passes.
      //     The port models a registered game, so it keys on
      //     control.registered and treats an unregistered run as false.
      //   {bN}/{BN}: Nova control bit N. The original reads DAT_005914cc[N],
      //     the 10,000-entry control-bit array mirrored by control.bits.
      const bool pass =
          condition == 'P'
              ? state.control.registered
              : (count >= 0 &&
                 state.control.ControlBit(static_cast<std::uint32_t>(count)));
      bool take_first_arm = pass;
      if (negate) {
        take_first_arm = !take_first_arm;
      }
      if (take_first_arm) {
        machine = ch == '"' ? 4 : 3;
      } else {
        machine = ch == '"' ? 6 : 5;
      }
      break;
    }
    case 3:
      if (ch == '"') {
        machine = 4;
      }
      break;
    case 4:
      if (ch == '\\') {
        escaped = true;
      } else if (ch != '"' || escaped) {
        out += ch;
        escaped = false;
      } else {
        machine = 8;
      }
      break;
    case 5:
      if (ch == '"') {
        machine = 6;
      }
      break;
    case 6:
      if (ch == '\\') {
        escaped = true;
      } else if (ch != '"' || escaped) {
        escaped = false;
      } else {
        machine = 7;
      }
      break;
    case 7:
      if (ch == '}') {
        machine = 0;
      } else if (ch == '"') {
        machine = 4;
      }
      break;
    case 8:
      if (ch == '}') {
        machine = 0;
      }
      break;
    default:
      if (ch == '{') {
        machine = 1;
      } else {
        out += ch;
      }
      break;
    }
  }
  // The original's trailing <PSRK...>/<SSRK...> scan latched the government id
  // in g_expanded_psrk_ship_class / _ssrk for a later substitution pass. The
  // port parses each <PRKnnn>/<SRKnnn> token directly in
  // Mission_ExpandMissionWildcards instead (see
  // ReplacePerGovernmentRankTokens).
  text = std::move(out);
}

// ---------------------------------------------------------------------------
// Mission-text wildcard expansion (Bible "special symbols")
// ---------------------------------------------------------------------------

namespace {

// Ghidra 0x00445d70 Mission_ReplaceSubstringInMissionText: replace every
// occurrence of `token` in `text` with `replacement` (the original loops
// CStringBuffer_ReplaceSubstring 0x004bc100 over the shared scratch buffer
// until no match remains).
void ReplaceMissionToken(std::string &text,
                         const std::string &token,
                         const std::string &replacement) {
  if (token.empty()) {
    return;
  }
  std::size_t pos = 0;
  while ((pos = text.find(token, pos)) != std::string::npos) {
    text.replace(pos, token.size(), replacement);
    pos += replacement.size();
  }
}

// Stellar name for <DST>/<RST>; the locals in 0x004444f0 start as "[Error]"
// and keep it only when the id is out of range -- the original copies an
// empty display name verbatim.
[[nodiscard]] std::string MissionStellarName(const GameState &state,
                                             std::int16_t stellar_id) {
  if (stellar_id < 0 ||
      stellar_id >= static_cast<std::int16_t>(state.scenario.stellars.size())) {
    return "[Error]";
  }
  const auto &stellar =
      state.scenario.stellars[static_cast<std::size_t>(stellar_id)];
  return stellar.name;
}

// System name for <DSY>/<RSY>. The original resolves the stellar's owning
// system through System_ResolveVisibleSystemForTravel, then the discovery
// slot, then System_FindSystemContainingStellar; the port's membership map is
// already the visible-system resolution, so only the visible resolve runs
// here; when it fails the original keeps "[Error]" (no raw-id fallback).
// Like stellar names, an empty system name is copied verbatim.
[[nodiscard]] std::string MissionSystemName(const GameState &state,
                                            std::int16_t system_id) {
  // No raw-id fallback: when visibility resolution fails the original keeps
  // "[Error]".
  const std::int16_t resolved =
      Misn_ResolveVisibleSystemForTravel(state, system_id);
  if (resolved < 0 ||
      resolved >= static_cast<std::int16_t>(state.scenario.systems.size())) {
    return "[Error]";
  }
  return state.scenario.systems[static_cast<std::size_t>(resolved)].name;
}

// Ghidra 0x00465c10 CString_AppendFormattedQuantity: plain digits below 1000,
// comma grouping below one million, x.xxM above.
[[nodiscard]] std::string FormatMissionQuantity(std::uint32_t value) {
  if (value < 1000) {
    return std::to_string(value);
  }
  if (value < 1000000) {
    const auto thousands = value / 1000;
    const auto remainder = value % 1000;
    return std::to_string(thousands) + "," +
           std::string(remainder < 100 ? 1 : 0, '0') +
           std::string(remainder < 10 ? 1 : 0, '0') + std::to_string(remainder);
  }
  const auto millions = value / 1000000;
  const auto fraction = (value % 1000000) / 10000;
  return std::to_string(millions) + "." +
         std::string(fraction < 10 ? 1 : 0, '0') + std::to_string(fraction) +
         "M";
}

// <PAY>: PayVal encoding shared with the acceptance-credit gate - positive is
// the credit amount, below -50000 is an acceptance cost (abs - 50000), and
// -40035..-40001 is a percentage (|PayVal| - 40000 hundredths, DOUBLE_00575508)
// of the player's current credits. The remaining negative encodings show 0.
[[nodiscard]] std::string MissionPayText(const GameState &state,
                                         std::int32_t pay_val) {
  std::int64_t shown = 0;
  if (pay_val > 0) {
    shown = pay_val;
  } else if (pay_val < -50000) {
    shown = -static_cast<std::int64_t>(pay_val) - 50000;
  } else if (pay_val < -40000 && pay_val >= -40035) {
    // Percent-of-holdings arm: float arithmetic with half-even rounding like
    // the x87 FISTP path (cf. the reputation loop in
    // Mission_ResolveMissionSuccess).
    const float magnitude = static_cast<float>(-pay_val - 40000);
    const float scaled =
        static_cast<float>(state.player.credits) * magnitude * 0.01F;
    shown = std::lrint(scaled);
    if (shown < 0) {
      shown = 0;
    }
  }
  return FormatMissionQuantity(static_cast<std::uint32_t>(shown));
}

// <CT>: the STR# 0xfa1 commodity name for the mission cargo type, with the
// leading '*' quantityless marker stripped (Bible note on quantityless cargo).
[[nodiscard]] std::string MissionCargoName(std::int16_t cargo_type) {
  if (cargo_type < 0 || cargo_type >= 0x100) {
    return "[Error]";
  }
  auto name = NovaHud_LoadStringEntry(
      0xfa1, static_cast<std::uint16_t>(cargo_type + 1));
  if (!name) {
    return "[Error]";
  }
  if (!name->empty() && name->front() == '*') {
    name->erase(name->begin());
  }
  return *name;
}

// STR# 0x7d2 entry 0x155 ("captain"): the original's fallback when no rank
// applies to <PRK>/<SRK>/<RRK> or a per-government <PRKnnn>/<SRKnnn>.
[[nodiscard]] std::string MissionRankFallback() {
  return NovaHud_LoadStringEntry(0x7d2, 0x155).value_or("captain");
}

// <PRKnnn>/<SRKnnn> per-government rank names (Bible 1796-1799). `nnn` is the
// government resource id (0x80-based), so the government index is nnn - 0x80;
// a well-formed token with an out-of-range id is left untouched (the original
// pre-scan stores -1 and skips it). DIVERGENCE: the original pre-scanned
// <PRK...>/<SRK...> into g_expanded_psrk_ship_class / _ssrk and substituted
// from crossed buffers in Stellar_BuildTravelDestinationDescription
// (0x004444f0, byte verified: <PRKnnn> read the ssrk-gated frame and both
// per-government loops used ShortName +0xde). That broke shipped text (the
// Federation <PRK128> briefing would read "captain" with no <SRK...> present),
// so each token is parsed directly here and uses the Bible field (ConvName for
// PRK, ShortName for SRK).
void ReplacePerGovernmentRankTokens(const GameState &state,
                                    std::string &text,
                                    std::string_view prefix,
                                    bool use_short_name) {
  std::size_t pos = 0;
  while ((pos = text.find(prefix, pos)) != std::string::npos) {
    std::size_t i = pos + prefix.size();
    const std::size_t digits_begin = i;
    std::uint32_t id = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
      if (id <= 0x17fU) {
        id = id * 10U + static_cast<std::uint32_t>(text[i] - '0');
      }
      ++i;
    }
    // The original requires digits and a closing '>' before it substitutes.
    if (i == digits_begin || i >= text.size() || text[i] != '>') {
      pos += prefix.size();
      continue;
    }
    if (id < 0x80U || id > 0x17fU) {
      pos = i + 1;
      continue;
    }
    const std::int16_t slot = Rank_HighestWeightedActiveSlotForGovernment(
        state, static_cast<std::int16_t>(id - 0x80U), use_short_name);
    std::string replacement = MissionRankFallback();
    if (slot >= 0 &&
        static_cast<std::size_t>(slot) < state.scenario.ranks.size()) {
      const RankDef &rank =
          state.scenario.ranks[static_cast<std::size_t>(slot)];
      replacement = use_short_name ? rank.short_name : rank.conv_name;
    }
    text.replace(pos, i + 1 - pos, replacement);
    pos += replacement.size();
  }
}

} // namespace

// Ghidra 0x004444f0 Stellar_BuildTravelDestinationDescription (wildcard pass)
// + 0x00445d70 Mission_ReplaceSubstringInMissionText. Expands the Bible
// mission-text wildcards in `text` and returns the result. `offering_list`
// selects the offer-row arm (mission_id = definition index, targets read from
// mission_target_resolutions, pay from the definition); the active arm reads
// the accepted mission slot. Tokens without a modeled source expand to the
// original's "[Error]" sentinel.
std::string Mission_ExpandMissionWildcards(const GameState &state,
                                           std::string_view text,
                                           bool offering_list,
                                           std::int16_t mission_id) {
  std::int16_t travel_stellar = -1;
  std::int16_t travel_system = -1;
  std::int16_t return_stellar = -1;
  std::int16_t return_system = -1;
  std::int16_t cargo_type = -1;
  std::int16_t cargo_qty = 0;
  std::int32_t pay_val = 0;
  std::string special_ship_name;
  const ActiveMission *active = nullptr;
  if (offering_list) {
    if (mission_id >= 0 &&
        static_cast<std::size_t>(mission_id) < state.scenario.missions.size()) {
      const auto &target =
          state
              .mission_target_resolutions[static_cast<std::size_t>(mission_id)];
      travel_stellar = target.travel_stellar_id;
      travel_system = target.travel_system_id;
      return_stellar = target.return_stellar_id;
      return_system = target.return_system_id;
      cargo_type = target.cargo_type_id;
      cargo_qty = target.cargo_qty_tons;
      pay_val = state.scenario.missions[static_cast<std::size_t>(mission_id)]
                    .resource_delta_or_cost;
    }
  } else if (mission_id >= 0 &&
             static_cast<std::size_t>(mission_id) <
                 state.active_missions.size() &&
             state
                 .active_mission_runtime_flags[static_cast<std::size_t>(
                     mission_id)]
                 .is_active) {
    active = &state.active_missions[static_cast<std::size_t>(mission_id)];
    travel_stellar = active->travel_stellar_id;
    return_stellar = active->return_stellar_id;
    travel_system = ResolveContainingSystem(state, travel_stellar);
    return_system = ResolveContainingSystem(state, return_stellar);
    cargo_type = active->cargo_type_id;
    cargo_qty = active->cargo_qty_tons;
    pay_val = active->resource_delta_or_cost;
  }

  // <SN>: g_active_misn +0x40 mission_fleet_name, drawn at acceptance from
  // the +0x2a pool (Mission_PopulateMissionSlotFromDef 0x0043f8c0); empty
  // when no pool was drawn. Re-resolving the stored (pool, entry) pair
  // yields the same text (cf. MissionShipPoolString in hud_renderer.cpp).
  // Inactive slots keep the "[Error]" sentinel (the original's LAB arm
  // skips the mission-token fill when the slot is not active).
  if (active != nullptr && active->special_ship_name_entry >= 1) {
    if (auto name = NovaHud_LoadStringEntry(
            static_cast<std::uint16_t>(active->special_ship_name_string_id),
            static_cast<std::uint16_t>(active->special_ship_name_entry))) {
      special_ship_name = *name;
    }
  }
  std::string destination = MissionStellarName(state, travel_stellar);
  std::string destination_system = MissionSystemName(state, travel_system);
  const std::string return_dest = MissionStellarName(state, return_stellar);
  const std::string return_system_name =
      MissionSystemName(state, return_system);
  // An unresolvable destination falls back to the return destination when that
  // one resolves (the original's "[Error]" equality swap).
  if (destination == "[Error]" && return_dest != "[Error]") {
    destination = return_dest;
  }
  if (destination_system == "[Error]" && return_system_name != "[Error]") {
    destination_system = return_system_name;
  }

  // Player identity: pilot.first_name is the full name (DAT_007d20b7),
  // pilot.last_name the nickname (DAT_007d21b7; <PNN> falls back to the full
  // name when unset).
  const std::string player_name = state.pilot.first_name;
  const std::string nickname = state.pilot.last_name.empty()
                                   ? state.pilot.first_name
                                   : state.pilot.last_name;
  std::string ship_type = "[Error]";
  if (state.player.ship_class_id >= 0) {
    if (const auto *ship_class = state.scenario.Ship(static_cast<std::int16_t>(
            state.player.ship_class_id + kResourceIdBase));
        ship_class != nullptr && !ship_class->display_name.empty()) {
      ship_type = ship_class->display_name;
    }
  }

  std::string result(text);
  ReplaceMissionToken(result, "<DST>", destination);
  ReplaceMissionToken(result, "<DSY>", destination_system);
  ReplaceMissionToken(result, "<RST>", return_dest);
  ReplaceMissionToken(result, "<RSY>", return_system_name);
  ReplaceMissionToken(result, "<CT>", MissionCargoName(cargo_type));
  ReplaceMissionToken(
      result, "<CQ>", cargo_type >= 0 ? std::to_string(cargo_qty) : "[Error]");
  ReplaceMissionToken(
      result, "<SN>", active != nullptr ? special_ship_name : "[Error]");
  // <DL> (0x004444f0 active-slot arm): the runtime flags' absolute deadline
  // date formatted with the full month names; when the deadline equals the
  // current date the original's format buffer keeps its empty init content,
  // so the token expands to "".
  if (active != nullptr && mission_id >= 0 &&
      static_cast<std::size_t>(mission_id) <
          state.active_mission_runtime_flags.size()) {
    const MissionRuntimeFlags &slot =
        state
            .active_mission_runtime_flags[static_cast<std::size_t>(mission_id)];
    if (slot.deadline_year != 0) {
      const GameDate deadline{
          slot.deadline_year, slot.deadline_month, slot.deadline_day};
      std::string deadline_text;
      if (deadline.year != state.date.year ||
          deadline.month != state.date.month ||
          deadline.day != state.date.day) {
        deadline_text = NovaText_FormatDateString(deadline, false);
      }
      ReplaceMissionToken(result, "<DL>", deadline_text);
    } else {
      ReplaceMissionToken(result, "<DL>", "[Error]");
    }
  } else {
    // Offer-row arm: the per-definition target block's deadline, formatted
    // the same way. The buffer keeps its "[Error]" init only when the stored
    // deadline equals today; no-TimeLimit missions have zeroed block fields
    // (0x0043d240 leaves them untouched for steps < 1), so their <DL>
    // expands to the zero date, matching the original's garbage.
    std::string deadline_text = std::string("[Error]");
    if (mission_id >= 0 && static_cast<std::size_t>(mission_id) <
                               state.mission_target_resolutions.size()) {
      const MissionTargetResolution &target =
          state
              .mission_target_resolutions[static_cast<std::size_t>(mission_id)];
      const GameDate deadline{
          target.deadline_year, target.deadline_month, target.deadline_day};
      if (deadline.year != state.date.year ||
          deadline.month != state.date.month ||
          deadline.day != state.date.day) {
        deadline_text = NovaText_FormatDateString(deadline, false);
      }
    }
    ReplaceMissionToken(result, "<DL>", deadline_text);
  }
  ReplaceMissionToken(result, "<PN>", player_name);
  ReplaceMissionToken(result, "<PNN>", nickname);
  ReplaceMissionToken(result,
                      "<PSN>",
                      state.player.ship_name.empty() ? std::string("[Error]")
                                                     : state.player.ship_name);
  ReplaceMissionToken(result, "<PST>", ship_type);
  // <OSN>: the speaking mission ship's personality display name, read from
  // the DAT_0077430e speaker latch (valid slots 1..0x3f only). Outside an
  // announcement context it keeps the [Error] sentinel.
  std::string speaker_name = "[Error]";
  if (state.mission_speaker_ship_slot >= 0 &&
      state.mission_speaker_ship_slot < 0x40) {
    const Ship &speaker =
        state.ShipAt(static_cast<std::size_t>(state.mission_speaker_ship_slot));
    if (speaker.pers_def_slot >= 0 &&
        static_cast<std::size_t>(speaker.pers_def_slot) <
            state.scenario.pers_defs.size()) {
      const std::string &name =
          state.scenario
              .pers_defs[static_cast<std::size_t>(speaker.pers_def_slot)]
              .display_name;
      if (!name.empty()) {
        speaker_name = name;
      }
    }
  }
  ReplaceMissionToken(result, "<OSN>", speaker_name);
  // <PRK>/<SRK>/<RRK>: the highest-weighted active rank's ConvName / ShortName
  // (Stellar_BuildTravelDestinationDescription 0x004444f0), and the full name
  // at GameState.recently_activated_rank_id for <RRK>. With no applicable rank
  // the original falls back to STR# 0x7d2 entry 0x155 ("captain").
  const std::int16_t prk_slot = Rank_HighestWeightedActiveSlot(state, false);
  const std::int16_t srk_slot = Rank_HighestWeightedActiveSlot(state, true);
  const std::int16_t rrk_slot = state.recently_activated_rank_id;
  ReplaceMissionToken(
      result,
      "<PRK>",
      prk_slot < 0
          ? MissionRankFallback()
          : state.scenario.ranks[static_cast<std::size_t>(prk_slot)].conv_name);
  ReplaceMissionToken(
      result,
      "<SRK>",
      srk_slot < 0 ? MissionRankFallback()
                   : state.scenario.ranks[static_cast<std::size_t>(srk_slot)]
                         .short_name);
  ReplaceMissionToken(
      result,
      "<RRK>",
      rrk_slot < 0
          ? MissionRankFallback()
          : state.scenario.ranks[static_cast<std::size_t>(rrk_slot)].full_name);
  ReplaceMissionToken(result, "<PAY>", MissionPayText(state, pay_val));
  // FUN_004d45a0: the registration name, or the shared "EV Nova Community"
  // string when no name is registered. The port has no registration system.
  ReplaceMissionToken(result, "<REG>", "EV Nova Community");
  // <PRKnnn>/<SRKnnn> per-government rank names (Bible 1796-1799). The
  // original pre-scanned the id into g_expanded_psrk_ship_class / _ssrk and
  // substituted from crossed buffers; see ReplacePerGovernmentRankTokens for
  // the divergence. The unregistered letter-scramble block (DAT_007354a4)
  // still needs the shareware model.
  ReplacePerGovernmentRankTokens(
      state, result, "<PRK", /*use_short_name=*/false);
  ReplacePerGovernmentRankTokens(
      state, result, "<SRK", /*use_short_name=*/true);
  return result;
}

// Ghidra 0x00443c60 Mission_HandleMissionOrSurrenderShipReaction. Per-tick
// objective evaluation for one active mission slot: drives the
// objective-complete/failed runtime latches from the mission goal's counters
// (Bible ShipGoal: 0 destroy, 1 disable, 2 board, 3 escort, 4 observe,
// 5 rescue, 6 chase-off; -1 no goal), fails overdue missions, and runs the
// completion payload + auto-abort resolution when the objective first
// completes. The deadline arm is suppressed while the travel scene owns the
// world (state.in_travel_scene): the landing gate (0x00443780) runs with the
// destination window up, which clears g_travel_scene_ctx only after the
// pass.
void Mission_HandleMissionOrSurrenderShipReaction(GameState &state,
                                                  std::int16_t mission_slot,
                                                  std::uint32_t now_ms) {
  const auto slot = static_cast<std::size_t>(mission_slot);
  MissionRuntimeFlags &runtime = state.active_mission_runtime_flags[slot];
  if (!runtime.is_active) {
    return;
  }
  ActiveMission &mission = state.active_missions[slot];
  // Auto-abort missions with no goal and exhausted target count drop their
  // completion latch before re-evaluation.
  if (mission.spawn_behavior == -1 && mission.target_ship_count > 0 &&
      mission.goal_count_remaining < 1 &&
      (mission.flags_primary & 0x0001U) != 0U) {
    runtime.objective_complete = false;
  }
  const bool was_objective_complete = runtime.objective_complete;
  const std::int16_t goal = mission.spawn_behavior;
  if (goal == -1 && mission.mission_target_count < 1) {
    runtime.objective_complete = true;
  } else {
    const std::int16_t target_count = mission.mission_target_count;
    if (target_count < 1) {
      runtime.objective_complete = true;
    } else if (goal == -1) {
      // Auto-abort missions complete immediately unless they still owe
      // spawn-mode-1 targets with a pending countdown.
      if ((mission.flags_primary & 0x0001U) == 0U ||
          mission.goal_count_remaining > 0 ||
          mission.special_ship_spawn_mode != 1) {
        runtime.objective_complete = true;
      }
    } else if (mission.goal_count_remaining < target_count &&
               mission.target_ship_count != 0) {
      runtime.objective_complete = false;
    } else {
      // Goal 0 (destroy): all targets destroyed.
      if (goal == 0 && target_count <= mission.goal_counter_a) {
        runtime.objective_complete = true;
      }
      // Goal 1 (disable): disabled-count suffices; destroying a target
      // fails the mission.
      if (goal == 1) {
        if (mission.goal_counter_a < 1) {
          if (target_count <= mission.goal_counter_c) {
            runtime.objective_complete = true;
          }
        } else {
          runtime.is_failed = true;
        }
      }
      // Goals 2 (board) / 5 (rescue): boarded/rescued count.
      if ((goal == 2 || goal == 5) && target_count <= mission.goal_counter_b) {
        runtime.objective_complete = true;
      }
      // Goal 3 (escort): fails if any escort was destroyed or disabled;
      // otherwise complete when escorts remain alive.
      if (goal == 3) {
        if (mission.goal_counter_a < 1 && mission.goal_counter_c < 1) {
          std::int16_t survivors = 0;
          for (std::size_t i = 1; i < state.ships_.size(); ++i) {
            const Ship &ship = state.ships_[i];
            if (ship.is_active && ship.mission_fleet_slot == mission_slot) {
              ++survivors;
            }
          }
          runtime.objective_complete = survivors != 0;
        } else {
          runtime.is_failed = true;
        }
      }
      // Goal 4 (observe): in the mission system, some fleet ship must be
      // seen (uncloaked, or cloaked but onscreen).
      if (goal == 4 && !runtime.objective_complete) {
        const std::int16_t raw_system = mission.current_system_id;
        const std::int16_t resolved =
            (raw_system < 0 || raw_system >= 0x800)
                ? -1
                : Misn_ResolveVisibleSystemForTravel(state, raw_system);
        if ((resolved == state.player.current_system_id || raw_system == -6) &&
            mission.target_ship_count <= mission.goal_count_remaining) {
          for (std::size_t i = 1; i < state.ships_.size(); ++i) {
            const Ship &ship = state.ships_[i];
            if (!ship.is_active || ship.mission_fleet_slot != mission_slot) {
              continue;
            }
            if (!NovaAiShip_CanEngageTargetUnderCloakRules(
                    state, ship, state.player)) {
              continue;
            }
            if (!NovaAiShip_CanMaintainCloakState(state, ship)) {
              // Uncloaked ships are seen directly.
              runtime.objective_complete = true;
              break;
            }
            // Cloaked ships require the original's sprite-rect intersection
            // with the gameplay surface. TODO(decomp(0x00443c60)) skipped:
            // the clean-room Ship has no sprite-rect/viewport model yet, so
            // cloaked targets are never "seen" and observe missions whose
            // targets stay cloaked cannot complete.
          }
        }
      }
      // Goal 6 (chase-off): chased-off (jumped out) + destroyed count.
      if (goal == 6) {
        runtime.objective_complete =
            static_cast<std::int32_t>(mission.goal_counter_e) +
                mission.goal_counter_a >=
            target_count;
      }
    }
  }
  // Deadline arm: an expired TimeLimit quick-fails the mission, except while
  // the travel scene owns the world (g_travel_scene_ctx != 0 in the original:
  // the landing pass resolves missions docked, never on the clock). The
  // STR# 0x7d2 0x11d "mission failed" overlay + transition-table cue 3 are
  // skipped for invisible missions (flags-accept 0x0400).
  if (!runtime.is_failed && !state.in_travel_scene &&
      mission.time_limit_days_remaining < 1 &&
      mission.time_limit_days_remaining > -32000) {
    runtime.is_failed = true;
    if ((runtime.flags_primary_at_accept & 0x0400U) == 0U) {
      // NovaAudio_QueueCenteredSound(g_transition_sound_handle_table[3], 1)
      // then STR# 0x7d2 entry 0x11d (1-based) = "Time limit exceeded -
      // mission failed." (Mission_HandleMissionOrSurrenderShipReaction
      // 0x00443c60).
      EnsureTransitionSounds(state);
      state.pending_ui_sounds.push_back({3, 1});
      if (auto text = NovaHud_LoadStringEntry(0x7d2, 0x11d)) {
        NovaHud_ShowOverlayMessage(state,
                                   *text,
                                   /*duration_frames=*/std::uint64_t{0xf0});
      }
    }
    Mission_FailMissionSlotQuick(state, mission_slot, now_ms);
  }
  // First-completion arm: run the ShipDone desc and, for auto-abort
  // missions, resolve the slot. The +0x43 completion dialog is UI-owned.
  if (!runtime.is_failed && runtime.objective_complete &&
      !was_objective_complete) {
    if (mission.brief_description_ids[7] != -1) {
      NovaLog::Todo("mission goal-complete dialog (misn {} id {}) not "
                    "reconstructed yet",
                    mission.mission_template_id,
                    mission.brief_description_ids[7]);
    }
    if (mission.spawn_behavior != -1) {
      Mission_RunMisnScriptPayload(
          state, TextOf(mission.state_latch), mission_slot);
    }
    if ((mission.flags_primary & 0x0001U) != 0U) {
      Mission_ResolveMisnSlot(state, mission_slot, now_ms);
    }
  }
}

// Ghidra 0x00443760 Mission_TickShipInteractionReactions. Per-tick driver
// over the 16 active-mission slots (TickSystems scope 0xb).
void Mission_TickShipInteractionReactions(GameState &state,
                                          std::uint32_t now_ms) {
  for (std::size_t slot = 0; slot < GameState::kMaxActiveMissions; ++slot) {
    Mission_HandleMissionOrSurrenderShipReaction(
        state, static_cast<std::int16_t>(slot), now_ms);
  }
}

// Ghidra 0x004438d0 Mission_ProcessInteractionReactionSlotResources. Landing
// interaction pass for one slot: handles mission-cargo pickup/drop-off at the
// TravelStel and final delivery at the ReturnStel. The pickup/drop-off desc
// dialogs are UI-owned and logged when they would fire (TODO(decomp)).
// `landed_stellar_id` is a 0-based stellar index (the original passes
// ai_secondary_target_slot, which indexes g_stellar_defs directly).
void Mission_ProcessInteractionReactionSlotResources(
    GameState &state,
    std::int16_t mission_slot,
    std::int16_t landed_stellar_id) {
  const auto slot = static_cast<std::size_t>(mission_slot);
  ActiveMission &mission = state.active_missions[slot];
  MissionRuntimeFlags &runtime = state.active_mission_runtime_flags[slot];
  if (NovaStellar_AreStellarsEquivalent(
          state, mission.travel_stellar_id, landed_stellar_id)) {
    if (mission.drop_off_mode == 1 && !mission.carrying_resources) {
      // Pick up here: gate on hauling the special-ship count as tonnage.
      if (Mission_TryConsumeMissionInteractionResources(
              state, mission.cargo_qty_tons)) {
        mission.carrying_resources = true;
        runtime.initial_briefing_done = true;
        if (mission.brief_description_ids[2] != -1) {
          NovaLog::Todo("mission cargo-loaded desc (misn {} id {}) not "
                        "reconstructed yet",
                        mission.mission_template_id,
                        mission.brief_description_ids[2]);
        }
      }
    } else {
      runtime.initial_briefing_done = true;
    }
    if (mission.drop_off_mode == 0 && mission.carrying_resources) {
      mission.carrying_resources = false;
      if (mission.brief_description_ids[3] != -1) {
        NovaLog::Todo("mission cargo-dropped desc (misn {} id {}) not "
                      "reconstructed yet",
                      mission.mission_template_id,
                      mission.brief_description_ids[3]);
      }
      state.stat_cache_valid = false;
    }
  }
  if (NovaStellar_AreStellarsEquivalent(
          state, mission.return_stellar_id, landed_stellar_id) &&
      mission.drop_off_mode == 1 && mission.carrying_resources &&
      (runtime.objective_complete || mission.spawn_behavior == -1 ||
       (mission.spawn_behavior == 3 && mission.goal_counter_a < 1))) {
    mission.carrying_resources = false;
    if (mission.brief_description_ids[3] != -1) {
      NovaLog::Todo("mission cargo-dropped desc (misn {} id {}) not "
                    "reconstructed yet",
                    mission.mission_template_id,
                    mission.brief_description_ids[3]);
    }
    state.stat_cache_valid = false;
  }
}

// Ghidra 0x00443780 Mission_TickReactionSlotsForTravelInteraction. The
// landing gate, run from the travel-destination interaction loop with the
// current stellar: evaluates objectives, processes cargo interactions, and
// resolves success/failure when the player is docked at a mission's
// ReturnStel. Re-runs the mission-list evaluation after any success. The
// original also calls NovaResources_EvaluateAvailability (0x00448090) every
// pass; the clean-room availability passes run lazily instead
// (TODO(decomp)).
void Mission_TickReactionSlotsForTravelInteraction(
    GameState &state,
    std::int16_t landed_stellar_id,
    std::uint32_t now_ms,
    const MissionDebriefSink &debrief) {
  // Callers pass the docked stellar in the port's travel-context convention
  // (0x80-based resource id, like GameState::travel.selected_stellar_id);
  // resolved mission targets are 0-based stellar indices (the original's
  // MisnActive convention: Stellar_AreStellarsEquivalent 0x0046efd0 and the
  // 0x0043d240 target table both index g_stellar_defs directly). Rebase here.
  const std::int16_t landed_index =
      landed_stellar_id >= kResourceIdBase
          ? static_cast<std::int16_t>(landed_stellar_id - kResourceIdBase)
          : landed_stellar_id;
  // The original runs this pass with g_travel_scene_ctx set to the landing
  // window handle (NovaUi_RunTravelDestinationInteractionLoop 0x00491f30
  // clears it only after the pass): the deadline arm stays suppressed and
  // released mission ships despawn while the destination window owns the
  // world.
  state.in_travel_scene = true;
  bool resolved_a_success = false;
  for (std::size_t slot = 0; slot < GameState::kMaxActiveMissions; ++slot) {
    if (!state.active_mission_runtime_flags[slot].is_active) {
      continue;
    }
    const auto mission_slot = static_cast<std::int16_t>(slot);
    Mission_HandleMissionOrSurrenderShipReaction(state, mission_slot, now_ms);
    Mission_ProcessInteractionReactionSlotResources(
        state, mission_slot, landed_index);
    ActiveMission &mission = state.active_missions[slot];
    MissionRuntimeFlags &runtime = state.active_mission_runtime_flags[slot];
    if (NovaStellar_AreStellarsEquivalent(
            state, mission.return_stellar_id, landed_index)) {
      if (mission.return_stellar_id == mission.travel_stellar_id ||
          mission.travel_stellar_id == -1) {
        runtime.initial_briefing_done = true;
      }
      if (!runtime.is_failed) {
        // Escort missions with no destroyed escorts complete on arrival;
        // the survivor check runs inside the objective evaluation.
        if (mission.spawn_behavior == 3 && mission.goal_counter_a < 1) {
          runtime.objective_complete = true;
        }
        if (runtime.initial_briefing_done && runtime.objective_complete) {
          Mission_ResolveMissionSuccess(state, mission_slot, debrief);
          resolved_a_success = true;
        }
      } else {
        Mission_ResolveMissionFailure(state, mission_slot, now_ms, debrief);
      }
    }
    Mission_HandleMissionOrSurrenderShipReaction(state, mission_slot, now_ms);
  }
  if (resolved_a_success) {
    // Side-effect call: refreshes availability and the resolved-locator cache.
    (void)Mission_EvaluateMissionLists(state);
  }
  state.in_travel_scene = false;
}

// Ghidra 0x00466c40 Mission_AdvanceGameDate.
void Mission_AdvanceGameDate(GameDate &date) {
  std::int16_t month_days = 31;
  switch (date.month) {
  case 4:
  case 6:
  case 9:
  case 11:
    month_days = 30;
    break;
  case 2:
    // Leap quirk kept: (year + (year < 0 ? 3 : 0)) & 3 == fixup in the
    // original, i.e. simply year divisible by 4.
    month_days = (date.year & 3) == 0 ? 29 : 28;
    break;
  default:
    break;
  }
  date.day = static_cast<std::int16_t>(date.day + 1);
  if (date.day > month_days) {
    date.day = 1;
    date.month = static_cast<std::int16_t>(date.month + 1);
    if (date.month > 12) {
      date.month = 1;
      date.year = static_cast<std::int16_t>(date.year + 1);
    }
  }
}

// Ghidra 0x0043f080 Mission_ComputeDateAfterSteps. The original
// leaves the out buffer untouched for steps < 1; callers only invoke it with
// a positive count (see Mission_ActivateAtSlot).
GameDate Mission_ComputeDateAfterSteps(const GameState &state,
                                       std::int16_t steps) {
  GameDate out = state.date;
  for (std::int16_t i = 0; i < steps; ++i) {
    Mission_AdvanceGameDate(out);
  }
  return out;
}

// Ghidra 0x00465550 Stellar_ComputeHyperspaceTravelDays.
int NovaStellar_ComputeHyperspaceTravelDays(const GameState &state,
                                            const Ship &ship) {
  const ShipClass *ship_class = state.scenario.Ship(
      static_cast<std::int16_t>(ship.ship_class_id + kResourceIdBase));
  if (ship_class == nullptr) {
    return 1;
  }
  int days = ship_class->mass_tons <= 99 ? 1 : 2;
  if (ship_class->mass_tons > 199) {
    ++days;
  }
  // The jump-time outfit arm applies only to the player (ShipState +0x86
  // ship_instance_id == 0): every owned outfit whose one of the four
  // ModTypes is 0x16 adds owned-count x ModVal days.
  if (ship.ship_instance_id == 0) {
    for (std::size_t outfit = 0;
         outfit < state.inventory.outfit_owned_count.size();
         ++outfit) {
      const std::int16_t owned = state.inventory.outfit_owned_count[outfit];
      if (owned <= 0) {
        continue;
      }
      const Outfit *def = state.scenario.Outfit(
          static_cast<std::int16_t>(outfit + kResourceIdBase));
      if (def == nullptr) {
        continue;
      }
      if (def->mod_type == 0x16) {
        days += owned * def->mod_val;
      }
      for (std::size_t i = 0; i < def->alt_mod_types.size(); ++i) {
        if (def->alt_mod_types[i] == 0x16) {
          days += owned * def->alt_mod_vals[i];
        }
      }
    }
  }
  return std::max(days, 1);
}

namespace {

// Ghidra 0x0046c800 Frame_IsRectVisibleInViewport (the crön-event arm; the
// same helper also culls rects elsewhere). Called with a g_cron_event_states
// block pointer it reads FirstYear/FirstMonth/FirstDay (+2/+4/+6) and
// LastYear/LastMonth/LastDay (+0x10/+0x12/+0x14) as an activation date
// window against the current game date:
// - FirstYear > 0 && year < FirstYear -> too early. FirstMonth == 0 gates on
//   FirstDay alone (day-of-month, any month); FirstMonth > 0 && FirstDay == 0
//   gates on the month alone; both > 0 gate on month*0x20 + day. A negative
//   FirstMonth or FirstDay field means "ignore this field" (Bible: 0 or -1).
// - Last* mirror the check with the comparisons inverted.
// Quirks preserved verbatim, including the month*0x20 composite comparison.
[[nodiscard]] bool CronEventDateWindowAllows(const CronEventDef &def,
                                             const GameDate &date) {
  if (def.first_year > 0 && date.year < def.first_year) {
    return false;
  }
  if (def.first_month == 0) {
    if (def.first_day > 0 && date.day < def.first_day) {
      return false;
    }
  } else if (def.first_month < 1 || def.first_day != 0) {
    if (def.first_month > 0 && def.first_day > 0 &&
        date.month * 0x20 + date.day < def.first_month * 0x20 + def.first_day) {
      return false;
    }
  } else if (def.first_month < date.month) {
    return false;
  }
  if (def.last_year > 0 && def.last_year < date.year) {
    return false;
  }
  if (def.last_month == 0) {
    if (def.last_day > 0) {
      if (def.last_day < date.day) {
        return false;
      }
      return true;
    }
  } else if (def.last_month < 1 || def.last_day != 0) {
    if (def.last_month > 0 && def.last_day > 0 &&
        def.last_month * 0x20 + def.last_day < date.month * 0x20 + date.day) {
      return false;
    }
  } else if (def.last_month < date.month) {
    return false;
  }
  return true;
}

// Ghidra 0x00439750 Mission_ActivateCronEvent. Fires the event's OnStart
// set-string through the reaction-script executor. With flags 0x0001 the
// original re-runs it while the Require mask and EnableOn expression hold,
// bailing out after 0x2711 iterations (a guard against infinite loops).
void Mission_ActivateCronEvent(GameState &state, std::int16_t cron_index) {
  const CronEventDef &def =
      state.scenario.cron_events[static_cast<std::size_t>(cron_index)];
  if ((def.flags & 0x0001U) == 0U) {
    Mission_ExecuteReactionScript(state, def.on_start);
    return;
  }
  std::int16_t iterations = 0;
  while (iterations < 0x2711) {
    if (!NovaOutfit_EvaluateRequireMask(
            state, def.require_lo, def.require_hi) ||
        !Mission_CheckReactionConditionSatisfied(state, def.enable_on)) {
      break;
    }
    Mission_ExecuteReactionScript(state, def.on_start);
    ++iterations;
  }
}

// Ghidra 0x004398b0 Mission_TerminateCronEvent. Fires OnEnd; flags 0x0002
// selects the same iterative arm as activation.
void Mission_TerminateCronEvent(GameState &state, std::int16_t cron_index) {
  const CronEventDef &def =
      state.scenario.cron_events[static_cast<std::size_t>(cron_index)];
  if ((def.flags & 0x0002U) == 0U) {
    Mission_ExecuteReactionScript(state, def.on_end);
    return;
  }
  std::int16_t iterations = 0;
  while (iterations < 0x2711) {
    if (!NovaOutfit_EvaluateRequireMask(
            state, def.require_lo, def.require_hi) ||
        !Mission_CheckReactionConditionSatisfied(state, def.enable_on)) {
      break;
    }
    Mission_ExecuteReactionScript(state, def.on_end);
    ++iterations;
  }
}

} // namespace

// Ghidra 0x00439500 Mission_TickDailyCronEvents. Once-per-game-day driver
// over the 0x200 crön slots (called from Mission_TickDailyWorldUpdate right
// after the calendar advance):
// - idle events roll rand(101) against the trigger odds; on a hit, the
//   date-window, Require-mask and EnableOn gates arm the event: active,
//   duration_counter = duration, then either the pre-holdoff wait or an
//   immediate OnStart (with an immediate OnEnd when duration == 0, latching
//   duration_counter to -1 so only the post-holdoff wait remains).
// - active events count down: holdoff first (a pre-holdoff expiry fires
//   OnStart; a post-holdoff expiry deactivates), then duration (OnEnd;
//   deactivation unless a post-holdoff wait keeps the slot busy).
// Events with no TimeLimit (duration == -1, the absent-slot sentinel) are
// never touched.
void Mission_TickDailyCronEvents(GameState &state) {
  const std::size_t count = std::min(state.scenario.cron_events.size(),
                                     state.cron_event_states.size());
  for (std::size_t index = 0; index < count; ++index) {
    const CronEventDef &def = state.scenario.cron_events[index];
    auto &runtime = state.cron_event_states[index];
    if (def.duration < 0 || !def.present) {
      continue;
    }
    if (!runtime.is_active) {
      if (std::uniform_int_distribution<int>{0, 100}(state.rng) >
          def.trigger_odds) {
        continue;
      }
      if (!CronEventDateWindowAllows(def, state.date) ||
          !NovaOutfit_EvaluateRequireMask(
              state, def.require_lo, def.require_hi) ||
          !Mission_CheckReactionConditionSatisfied(state, def.enable_on)) {
        continue;
      }
      runtime.is_active = true;
      runtime.duration_counter = def.duration;
      if (def.pre_holdoff < 1) {
        runtime.holdoff_counter = 0;
        Mission_ActivateCronEvent(state, static_cast<std::int16_t>(index));
        if (def.duration == 0) {
          Mission_TerminateCronEvent(state, static_cast<std::int16_t>(index));
          runtime.duration_counter = -1;
          if (def.post_holdoff > 0) {
            runtime.holdoff_counter = def.post_holdoff;
          }
        }
      } else {
        runtime.holdoff_counter = def.pre_holdoff;
      }
    } else if (runtime.holdoff_counter < 1) {
      --runtime.duration_counter;
      if (runtime.duration_counter < 1) {
        Mission_TerminateCronEvent(state, static_cast<std::int16_t>(index));
        if (def.post_holdoff < 1) {
          runtime.is_active = false;
        } else {
          runtime.holdoff_counter = def.post_holdoff;
        }
      }
    } else {
      --runtime.holdoff_counter;
      if (runtime.holdoff_counter < 1) {
        if (runtime.duration_counter < 0) {
          runtime.is_active = false;
        } else {
          Mission_ActivateCronEvent(state, static_cast<std::int16_t>(index));
          if (runtime.duration_counter == 0) {
            Mission_TerminateCronEvent(state, static_cast<std::int16_t>(index));
          }
        }
      }
    }
  }
}

// Ghidra 0x00423540 Outfit_CollectStellarIncome. Daily tribute pass: every
// available stellar carrying the +0x46 marker (its system visible + the 0x20
// availability bit, set by the display-state refresh) pays its Tribute value
// (payload +0x0a, default 1000 x TechLevel) and bumps its day counter
// (StellarDef +0x2a) unless the currently docked stellar carries the same
// 0x20 marker. TODO(decomp): the domination flow that grants a stellar the
// +0x46 marker is not modelled, so the pass stays idle in practice.
void Stellar_CollectDailyTributeIncome(GameState &state) {
  const std::size_t count =
      std::min(state.scenario.stellars.size(), static_cast<std::size_t>(0x800));
  for (std::size_t i = 0; i < count; ++i) {
    Stellar &stellar = state.scenario.stellars[i];
    if (!stellar.is_available || !stellar.hazard_marker) {
      continue;
    }
    const Stellar *docked = state.scenario.Stellar(
        static_cast<std::int16_t>(state.travel.selected_stellar_id));
    if (docked == nullptr || (docked->availability_flags & 0x20U) == 0U) {
      ++stellar.held_days;
    }
    state.player.credits += stellar.tribute;
    // g_playerInventoryAndLoadoutDirty = 1: the port's stat cache is the
    // consumer of that latch.
    state.stat_cache_valid = false;
  }
}

// Ghidra 0x00424f90 System_UpdateDisasterStates (per-game-day sweep of the
// 0x100 öops slots; runs from the daily world-update driver).
void System_UpdateDisasterStates(GameState &state) {
  for (auto &def : state.scenario.disaster_defs) {
    if (!def.present) {
      // Undefined slot: reset to the idle sentinels the loader leaves.
      def.active_stellar = -1;
      def.days_remaining = -1;
      def.started_once = false;
      continue;
    }
    if (def.days_remaining >= 1) {
      // Active: burn one day off the remaining duration.
      --def.days_remaining;
      continue;
    }
    if (def.started_once) {
      // Dead arm in the shipped code (started_once is never set), kept for
      // parity: a once-started disaster with a negative duration repeats
      // for two days.
      if (def.duration_days < 0) {
        def.days_remaining = 2;
      }
      continue;
    }
    // Idle and never started: roll the per-day chance, then gate on the
    // ActivateOn expression.
    const std::int16_t roll = RandomBelow(state, 100);
    if (roll + 1 > def.start_chance_percent) {
      continue;
    }
    if (!Mission_CheckReactionConditionSatisfied(state,
                                                 def.activation_expression)) {
      continue;
    }
    if (def.target_stellar < static_cast<std::int16_t>(kResourceIdBase)) {
      if (def.target_stellar != -1) {
        // Other sub-0x80 codes are inert (the original's empty inner arm).
        continue;
      }
      // "Any": pick a random available stellar that is not a travel-only
      // (flags 0x20) lane. The original rejects samples over the full 0x800
      // g_stellar_defs table; the clean-room table is shorter, so build the
      // eligible set explicitly (equivalent while the unmodelled tail slots
      // stay unavailable).
      std::vector<std::size_t> candidates;
      for (std::size_t i = 0; i < state.scenario.stellars.size(); ++i) {
        const auto &stellar = state.scenario.stellars[i];
        if (stellar.is_available && (stellar.flags & 0x20U) == 0U) {
          candidates.push_back(i);
        }
      }
      if (candidates.empty()) {
        continue;
      }
      std::uniform_int_distribution<std::size_t> pick(0, candidates.size() - 1);
      def.active_stellar =
          static_cast<std::int16_t>(candidates[pick(state.rng)]);
      def.days_remaining = def.duration_days;
      continue;
    }
    // Bound stellar: rebase the 0x80-based resource id to the 0-based index
    // the exchange compares against.
    def.active_stellar =
        static_cast<std::int16_t>(def.target_stellar - kResourceIdBase);
    def.days_remaining = def.duration_days;
  }
}

// Ghidra 0x00466cb0 ShipClass_RerollShipClassAvailabilityChances (daily
// world-update driver; see mission.hpp for the remaining skipped slices).
void Mission_TickDailyWorldUpdate(GameState &state) {
  Mission_AdvanceGameDate(state.date);
  Mission_TickDailyCronEvents(state);
  // Active-mission deadline countdown (MisnActive +0x45, 16 slots). The
  // -32000 no-deadline sentinel stays negative and is never touched.
  for (auto &mission : state.active_missions) {
    if (mission.time_limit_days_remaining > 0) {
      mission.time_limit_days_remaining =
          static_cast<std::int16_t>(mission.time_limit_days_remaining - 1);
    }
  }
  Stellar_CollectDailyTributeIncome(state);
  System_UpdateDisasterStates(state);
  const std::size_t system_count =
      std::min(state.scenario.systems.size(), GameState::kMaxSystems);
  for (std::size_t i = 0; i < system_count; ++i) {
    if (state.scenario.systems[i].is_visible &&
        state.reinforcement_retrigger_delay[i] > 0) {
      --state.reinforcement_retrigger_delay[i];
    }
  }
  // Per-stellar daily schedule + garrison resupply (0x800 x 0x498 loop):
  // available stellars only. TODO(decomp) skipped inside this loop: the two
  // daily-zeroed scratch fields (StellarDef +0x2e/+0x494) have no modelled
  // consumer.
  const std::size_t stellar_count =
      std::min(state.scenario.stellars.size(), static_cast<std::size_t>(0x800));
  for (std::size_t i = 0; i < stellar_count; ++i) {
    Stellar &stellar = state.scenario.stellars[i];
    if (!stellar.is_available) {
      continue;
    }
    // Garrison resupply (gated on the +0x46 marker like the income pass): a
    // garrison size above 1000 wraps modulo 1000, and the count creeps back
    // up one ship per 0x1c2-roll hit while below quota.
    if (stellar.hazard_marker) {
      int max = stellar.max_ship_count;
      if (max > 1000) {
        max %= 1000;
      }
      if (stellar.present_ship_count < max &&
          std::uniform_int_distribution<int>{0, 0x1c1}(state.rng) == 0) {
        ++stellar.present_ship_count;
      }
    }
    // Schedule countdown (StellarDef +0x47c, shared with the engagement-
    // access counter). Runs only while the stellar is sprite-active: a
    // negative seed pins the countdown at 1 (never fires); an expiring
    // countdown latches 0xffff, clears the sprite handle and fires the
    // schedule set-string once.
    if (NovaTargeting_IsStellarActive(stellar)) {
      if (stellar.schedule_days < 0) {
        stellar.engage_access = 1;
      } else if (--stellar.engage_access < 1) {
        stellar.engage_access = -1;
        stellar.strength = stellar.strength_capacity;
        Mission_ExecuteReactionScript(state, stellar.schedule_script);
      }
    } else {
      stellar.engage_access = -1;
    }
  }
  // Ship/outfit availability rerolls (the driver's tail): every ship class
  // gets fresh 1..100 licensed threshold/limit rolls, every outfit a fresh
  // 1..100 stock roll. TODO(decomp) skipped: the per-system dude_prob
  // +0x1c suppression countdown is handled above. TODO(decomp) skipped: the
  // active-rank daily salary (ränk Salary) is not modelled.
  const std::size_t ship_count =
      std::min(state.scenario.ships.size(), static_cast<std::size_t>(0x300));
  for (std::size_t i = 0; i < ship_count; ++i) {
    state.ship_class_limit_rolls[i] = static_cast<std::int16_t>(
        std::uniform_int_distribution<int>{0, 99}(state.rng) + 1);
    state.ship_class_threshold_rolls[i] = static_cast<std::int16_t>(
        std::uniform_int_distribution<int>{0, 99}(state.rng) + 1);
  }
  const std::size_t outfit_count =
      std::min(state.scenario.outfits.size(), static_cast<std::size_t>(0x200));
  for (std::size_t i = 0; i < outfit_count; ++i) {
    state.outfit_stock_rolls[i] = static_cast<std::int16_t>(
        std::uniform_int_distribution<int>{0, 99}(state.rng) + 1);
  }
}

namespace {

// Shared body of NovaText_FormatDateString (0x00468450) and
// Stellar_FormatElapsedTravelTime (0x00468600): "MONTH DAYst, YEAR" with the
// STR# 0x89 month table and day suffixes (st/nd/rd by last digit, th
// otherwise, 11-13 forced back to th).
[[nodiscard]] std::string FormatDateString(const GameDate &date,
                                           std::uint16_t month_entry) {
  std::string out;
  if (const auto month = NovaHud_LoadStringEntry(0x89, month_entry)) {
    out += *month;
  }
  out += ' ';
  out += std::to_string(date.day);
  std::uint16_t suffix = 0x1c; // "th"
  const int digit = date.day % 10;
  if (digit == 1) {
    suffix = 0x19;
  } else if (digit == 2) {
    suffix = 0x1a;
  } else if (digit == 3) {
    suffix = 0x1b;
  }
  if (date.day > 10 && date.day < 14) {
    suffix = 0x1c;
  }
  if (const auto text = NovaHud_LoadStringEntry(0x89, suffix)) {
    out += *text;
  }
  out += ", ";
  out += std::to_string(date.year);
  return out;
}

} // namespace

std::string NovaText_FormatDateString(const GameDate &date,
                                      bool abbreviated_month) {
  // The UI sites (BBS date 0x00441620, mission-info window, starmap status
  // bar) use the abbreviated month names (STR# 0x89 entries 13-24);
  // Stellar_FormatElapsedTravelTime (arrival message / <DL> token) uses the
  // full names (entries 1-12).
  return FormatDateString(date,
                          static_cast<std::uint16_t>(abbreviated_month
                                                         ? date.month + 12
                                                         : date.month));
}

// Ghidra 0x00426d10 Mission_ShowMissionShipAnnouncement.
void Mission_ShowMissionShipAnnouncement(GameState &state,
                                         std::int16_t hail_quote_id) {
  // NovaAudio_QueueCenteredSound(g_transition_sound_handle_table[4], 1,
  // g_centered_audio_gain): the flight loop consumes pending_ui_sounds and
  // plays transition-table cue 4 directly.
  state.pending_ui_sounds.push_back({/*transition_index=*/4, /*count=*/1});

  // Hail text: first string of STR# (hail_quote_id + 4999) when present,
  // else entry hail_quote_id of STR# 0x1bbd (7101, the pers HailQuote pool).
  // TODO(decomp): the original copies the raw STR# resource bytes into the
  // scratch and pstring-converts in place; the port decodes entry 1 of the
  // pool instead (same text for well-formed pools).
  std::optional<std::string> text;
  if (hail_quote_id >= 0) {
    text = NovaHud_LoadStringEntry(
        static_cast<std::uint16_t>(hail_quote_id + 4999), /*entry=*/1);
    if (!text) {
      text = NovaHud_LoadStringEntry(0x1bbd,
                                     static_cast<std::uint16_t>(hail_quote_id));
    }
  }
  if (!text) {
    NovaLog::Warn("mission hail: no text for pers hail id {}", hail_quote_id);
    return;
  }

  Mission_ExpandStringPlaceholders(state, *text);
  // Stellar_BuildTravelDestinationDescription(0, -1) (0x004444f0): the
  // wildcard pass with the mission context cleared -- mission destination
  // tokens expand to their [Error] sentinels, <OSN> to the speaking ship's
  // personality name (read from the DAT_0077430e speaker latch).
  *text = Mission_ExpandMissionWildcards(state,
                                         *text,
                                         /*offering_list=*/false,
                                         /*mission_id=*/-1);
  NovaHud_ShowOverlayMessage(state,
                             std::move(*text),
                             /*duration_frames=*/std::uint64_t{0x1a4});
}

// Ghidra 0x00426dd0 Mission_TrySpawnMissionShipAmbush.
void Mission_TrySpawnMissionShipAmbush(GameState &state) {
  // Gate: the ambush personality (pers slot 0x3fe) must be defined by the
  // scenario (present + loaded latch; PersDef +0x620/+0x623).
  const PersDef &ambusher =
      state.scenario.pers_defs[static_cast<std::size_t>(0x3fe)];
  if (!ambusher.present || !ambusher.loaded_latch) {
    return;
  }

  // Candidate count: dominated/hazard stellars that are currently
  // available with the availability_flags 0x20 bit clear (StellarDef
  // +0x44/+0x46/+0x34 read through the g_stellar_defs scan).
  std::int16_t candidates = 0;
  for (const Stellar &stellar : state.scenario.stellars) {
    if (stellar.is_available && stellar.hazard_marker &&
        (stellar.availability_flags & 0x20U) == 0U) {
      ++candidates;
    }
  }

  bool fire = false;
  if (candidates == 1) {
    fire = RandomBelow(state, 10) == 0;
  } else if (candidates > 1) {
    fire = RandomBelow(state, 5) == 0;
  }
  if (!fire) {
    return;
  }

  // TODO(decomp(0x00426dd0)) skipped: g_hud_overlay_msg_color = 0xffff is a
  // dead store in the original (NovaHud_ShowOverlayMessage overwrites the
  // countdown before anything reads it).
  const int slot =
      NovaPers_SpawnShipFromPersDef(state,
                                    state.player.current_system_id,
                                    /*exclude_derelict_govts=*/true,
                                    /*forced_pers_slot=*/0x3fe);
  if (slot < 0) {
    return;
  }
  Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
  // Ship_SetShipHostileToPlayer: its own pers Flags 0x10 arm may hail first
  // (speaker latched inside).
  NovaAi_SetShipHostileToPlayer(state, ship);
  state.mission_speaker_ship_slot = ship.ship_instance_id;
  Mission_ShowMissionShipAnnouncement(state, ambusher.hail_quote_id);
  state.mission_speaker_ship_slot = -1;
}

// Ghidra 0x00448660 Mission_ClearActiveReactionMission.
void Mission_ClearActiveReactionMission(GameState &state) {
  state.mission_interaction_context = -1;
}

// Ghidra 0x0046f140 Ship_HasAnyCargoLootOrActiveMission.
bool Ship_HasAnyCargoLootOrActiveMission(const GameState &state) {
  for (const std::int16_t bin : state.inventory.cargo_bins) {
    if (bin > 0) {
      return true;
    }
  }
  for (const std::int16_t junk : state.inventory.junk_counts) {
    if (junk > 0) {
      return true;
    }
  }
  for (const auto &flags : state.active_mission_runtime_flags) {
    if (flags.is_active) {
      return true;
    }
  }
  return false;
}

// Ghidra 0x00441b40 Mission_CheckMissionShipInteractionEligibility.
bool Mission_CheckMissionShipInteractionEligibility(const GameState &state,
                                                    std::int16_t mission_id,
                                                    bool interaction_context) {
  if (mission_id < 0 ||
      static_cast<std::size_t>(mission_id) >= state.scenario.missions.size()) {
    return false;
  }
  return CheckOfferingEligibility(state,
                                  static_cast<std::size_t>(mission_id),
                                  /*page_group=*/0,
                                  interaction_context);
}

// Ghidra 0x00433050 mission-hail ladder (runs inline in Ship_HandleShip).
void Mission_TickShipHailLadder(GameState &state,
                                Ship &ship,
                                std::int64_t now_60hz) {
  if (ship.pers_def_slot < 0 || static_cast<std::size_t>(ship.pers_def_slot) >=
                                    state.scenario.pers_defs.size()) {
    return;
  }
  const PersDef &pers =
      state.scenario.pers_defs[static_cast<std::size_t>(ship.pers_def_slot)];
  if (pers.hail_quote_id == -1 || !pers.present) {
    return;
  }
  // The player must not be station-held, the hailing ship must be visible
  // to the player under the cloak rules, alive, and not under escort control.
  if (state.player.ai_station_hold_timer > 0.0F ||
      !NovaAiShip_CanEngageTargetUnderCloakRules(state, state.player, ship) ||
      NovaAiShip_IsDestroyed(ship) || ship.ai_control_mode == 4 ||
      ship.ai_control_mode == 0x0D) {
    return;
  }

  const auto flags = static_cast<std::uint16_t>(pers.flags_primary);
  bool allow = true;
  // Flags 0x20: the ship hails only in its paired disable state -- disabled
  // ships hail only with 0x20 set, healthy ships only with it clear.
  const bool disabled = NovaAiShip_IsDisabled(state, ship);
  if (disabled != ((flags & 0x20U) != 0U)) {
    allow = false;
  }
  // Flags 0x10: one distress hail while not disabled, bypassing the throttle
  // below (the local_14 override in the original).
  bool distress_override = false;
  if ((flags & 0x10U) != 0U && !disabled) {
    if (ship.mission_hail_latch != 0) {
      allow = false;
    } else if (NovaTargeting_IsShipEligibleForDistressCall(state, ship)) {
      distress_override = true;
    } else {
      allow = false;
    }
  }
  // Flags 0x04: hail only after the personality has been loaded/marked
  // present by the mission-list evaluator (+0x621 latch).
  if ((flags & 0x04U) != 0U && !pers.loaded_latch) {
    allow = false;
  }
  // Flags 0x08: hail only when the ship's government would aid the player.
  if ((flags & 0x08U) != 0U &&
      !NovaGovernment_IsShipEligibleForGovernmentAid(state, ship)) {
    allow = false;
  }
  // Flags 0x400 + LinkMission: the linked mission must currently offer from
  // a ship (the original runs the check with g_travel_scene_ctx = 1).
  if ((flags & 0x400U) != 0U && pers.link_mission_id != -1 &&
      !Mission_CheckMissionShipInteractionEligibility(state,
                                                      pers.link_mission_id,
                                                      /*interaction_context=*/
                                                      true)) {
    allow = false;
  }
  // Flags 0x800: silent while the ship holds AI state 2 (clear primary).
  if ((flags & 0x800U) != 0U && ship.ai_state_code == 2) {
    allow = false;
  }
  // Flags 0x1000/0x2000/0x4000: silent versus player classes whose inherent
  // AI matches the masked band (1 / 2 / >2 respectively).
  const ShipClass *player_class = state.scenario.Ship(
      static_cast<std::int16_t>(state.player.ship_class_id + kResourceIdBase));
  const std::int16_t player_ai =
      player_class != nullptr ? player_class->default_ai_behavior : 0;
  if (((flags & 0x1000U) != 0U && player_ai == 1) ||
      ((flags & 0x2000U) != 0U && player_ai == 2) ||
      ((flags & 0x4000U) != 0U && player_ai > 2)) {
    allow = false;
  }
  // Flags 0x80: one hail per personality ship.
  if ((flags & 0x80U) != 0U && ship.mission_hail_latch != 0) {
    allow = false;
  }
  if (!allow) {
    return;
  }
  // Throttle: 1-in-0x8c roll, no overlay currently showing, and the +0xAC
  // re-hail window (+0xa8c = 2692 60 Hz ticks, ~45 s) expired. The distress
  // override bypasses all three.
  if (!distress_override &&
      !(RandomBelow(state, 0x8c) == 0 && !state.hud_overlay.active &&
        now_60hz > ship.last_mission_hail_tick_60hz + 0xa8c)) {
    return;
  }
  state.mission_speaker_ship_slot = ship.ship_instance_id;
  Mission_ShowMissionShipAnnouncement(state, pers.hail_quote_id);
  state.mission_speaker_ship_slot = -1;
  ship.mission_hail_latch = 1;
  ship.last_mission_hail_tick_60hz = static_cast<std::int32_t>(now_60hz);
}

} // namespace game
