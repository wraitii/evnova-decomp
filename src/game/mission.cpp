#include "mission.hpp"

#include "game_state.hpp"
#include "government.hpp"
#include "hud_overlay.hpp"
#include "log.hpp"
#include "mission_script.hpp"
#include "outfit.hpp"
#include "ship_ai.hpp"
#include "ship_spawn.hpp"
#include "targeting.hpp"
#include "travel.hpp"

#include <algorithm>
#include <cmath>
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
    return id >= 0 &&
           id < static_cast<std::int16_t>(
                    state.inventory.outfit_owned_count.size()) &&
           state.inventory.outfit_owned_count[static_cast<std::size_t>(id)] > 0;
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
      // Mission_ActivateMissionAtSlot checks both cargo and hull mass before
      // opening its original error dialog. Outfit purchase mass is already
      // normalized for hull-proportional outfits by ScenarioData.
      const std::int32_t total_mass = Outfit_ComputePlayerTotalMass(state);
      if (total_mass < definition.cargo_qty_tons ||
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
// yet, so availability/system membership remain the verified gates.
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
      if (static_cast<std::int16_t>(i) != current && system.is_visible &&
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
          state.scenario.systems[static_cast<std::size_t>(linked)].is_visible) {
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
  const auto matches = [&](const System &system) {
    if (!system.is_visible || system.government_id < 0) {
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
        matches(state.scenario.systems[i])) {
      candidates.push_back(static_cast<std::int16_t>(i));
    }
  }
  return choose_random(candidates, fallback);
}

} // namespace

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
  auto &active = state.active_missions[free_slot];
  // Bible PickupMode 0: the mission cargo is aboard from mission start. The
  // LoadCargText desc dialog (payload +0x38) is UI-owned and runs in
  // NovaMission_RunAcceptanceDialogs (docked_dialog.cpp) after activation.
  if (active.pickup_mode == 0) {
    active.carrying_resources = true;
  }
  // The initial destination briefing is skipped when the mission has no
  // TravelStel, or when the player accepts it while docked there.
  runtime.initial_briefing_done = active.travel_stellar_id == -1 ||
                                  active.travel_stellar_id == landed_stellar_id;
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
// TODO(decomp): the original despawns the assigned ships when
// g_travel_scene_ctx != 0 (the landing/travel scene owns the world); the
// clean-room runtime has no such context latch yet.
void Mission_ClearMisnSlotAssignments(GameState &state,
                                      std::int16_t mission_slot,
                                      bool emit_completion_payload,
                                      std::uint32_t now_ms) {
  for (Ship &ship : state.ships_) {
    if (!ship.is_active || ship.mission_fleet_slot != mission_slot) {
      continue;
    }
    ship.mission_fleet_slot = -1;
    if (ship.ai_target_ship_slot != -1) {
      const auto *ship_class = state.scenario.Ship(
          static_cast<std::int16_t>(ship.ship_class_id + kResourceIdBase));
      if (ship_class != nullptr) {
        ship.ai_behavior_code = ship_class->default_ai_behavior;
      }
      ship.ai_target_ship_slot = -1;
      NovaAi_EnterState2ClearPrimaryTarget(ship, now_ms);
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
                                   std::int16_t mission_slot) {
  const auto slot = static_cast<std::size_t>(mission_slot);
  ActiveMission &mission = state.active_missions[slot];
  // MisnActive +0x3d selects the success debrief selection dialog
  // (Ui_LoadSelectionDialogResource + Stellar_BuildTravelDestination-
  // Description + Ui_RunTravelSelectionDialog). UI-owned; not reconstructed.
  if (mission.brief_description_ids[4] != -1) {
    NovaLog::Todo("mission success debrief dialog (misn {} id {}) not "
                  "reconstructed yet",
                  mission.mission_template_id,
                  mission.brief_description_ids[4]);
  }
  state.active_mission_runtime_flags[slot].is_active = false;
  Mission_RunMisnScriptPayload(
      state, TextOf(mission.on_success_text), mission_slot);
  // On-resolve repeat count re-runs the daily availability reroll
  // (ShipClass_RerollShipClassAvailabilityChances 0x00466cb0) once per count.
  // That driver is not ported yet.
  if (mission.on_resolve_repeat_count > 0) {
    NovaLog::Todo("mission success repeat-count {} requires the daily "
                  "availability reroll (0x00466cb0), not yet ported",
                  mission.on_resolve_repeat_count);
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
          rep = static_cast<std::int16_t>(std::lrint(
              static_cast<float>(rep) - static_cast<float>(delta) * 0.5F));
        } else if (NovaGovernment_AreGovtsAllied(
                       state.scenario, system_govt, govt)) {
          rep = static_cast<std::int16_t>(std::lrint(
              static_cast<float>(rep) + static_cast<float>(delta) * 0.5F));
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
                                   std::uint32_t now_ms) {
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
  // MisnActive +0x3f selects the failure debrief dialog. UI-owned; not
  // reconstructed.
  if (mission.brief_description_ids[5] != -1) {
    NovaLog::Todo("mission failure debrief dialog (misn {} id {}) not "
                  "reconstructed yet",
                  mission.mission_template_id,
                  mission.brief_description_ids[5]);
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
    if (Outfit_ComputePlayerTotalMass(state) < count) {
      // The original shows the STR# 0x7d2 0x165 "not enough cargo mass"
      // selection dialog. UI-owned; not reconstructed (TODO(decomp)).
      NovaLog::Todo("mission interaction denied: total mass below {} tons",
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
  // On-resolve repeat count re-runs the daily availability reroll
  // (ShipClass_RerollShipClassAvailabilityChances 0x00466cb0, not yet
  // ported; see Mission_ResolveMissionSuccess).
  if (mission.on_resolve_repeat_count > 0) {
    NovaLog::Todo("mission resolve repeat-count {} requires the daily "
                  "availability reroll (0x00466cb0), not yet ported",
                  mission.on_resolve_repeat_count);
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
  // Recheck timer: DAT_00776af4 = NovaTime_GetTicksMs() + NovaRandom_Range(30)
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
  for (const char ch : text) {
    switch (machine) {
    case 1:
      if (ch == 'g' || ch == 'G') {
        // DAT_00734c1c ('m' latch): 1 = male -> first arm; '!' swaps.
        const bool male = state.control.male != negate;
        machine = male ? 3 : 5;
      } else if (ch == 'p' || ch == 'P') {
        count = 0;
        machine = 2;
      } else if (ch == 'b' || ch == 'B') {
        count = 0;
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
      // Condition bodies. {pN}: licensed game passes unconditionally; the
      // shareware day-counter arm is not modeled. {bN}: the original reads a
      // byte table at DAT_005914cc + N whose meaning is unresolved.
      // TODO(decomp) skipped: {p} shareware arm and the {b} byte table; both
      // evaluate as true here (fleet/announcement texts only). `count` carries
      // the parsed N for when those arms are reconstructed.
      (void)count;
      bool pass = true;
      if (negate) {
        pass = !pass;
      }
      if (pass) {
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
  // TODO(decomp) skipped: the original's trailing <PSRK...>/<SSRK...> scan
  // caches referenced ship-class ids for later name resolution; the port has
  // no consumer for that cache yet.
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
// and keep it when the id is out of range or the display name is empty.
[[nodiscard]] std::string MissionStellarName(const GameState &state,
                                             std::int16_t stellar_id) {
  if (stellar_id < 0 ||
      stellar_id >= static_cast<std::int16_t>(state.scenario.stellars.size())) {
    return "[Error]";
  }
  const auto &stellar =
      state.scenario.stellars[static_cast<std::size_t>(stellar_id)];
  return stellar.name.empty() ? "[Error]" : stellar.name;
}

// System name for <DSY>/<RSY>. The original resolves the stellar's owning
// system through System_ResolveVisibleSystemForTravel, then the discovery
// slot, then System_FindSystemContainingStellar; the port's membership map is
// already the visible-system resolution, so the visible-root fallback and a
// raw-id fallback cover it.
[[nodiscard]] std::string MissionSystemName(const GameState &state,
                                            std::int16_t system_id) {
  std::int16_t resolved = Misn_ResolveVisibleSystemForTravel(state, system_id);
  if (resolved == -1) {
    resolved = system_id;
  }
  if (resolved < 0 ||
      resolved >= static_cast<std::int16_t>(state.scenario.systems.size())) {
    return "[Error]";
  }
  const auto &system =
      state.scenario.systems[static_cast<std::size_t>(resolved)];
  return system.name.empty() ? "[Error]" : system.name;
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
// the credit amount, -50001.. is an acceptance cost (abs - 50000), and
// -40001..-40035 is a percentage (0.01 per unit, DOUBLE_00575508) of the
// player's current credits. The remaining negative encodings show 0.
[[nodiscard]] std::string MissionPayText(const GameState &state,
                                         std::int32_t pay_val) {
  std::int64_t shown = 0;
  if (pay_val > 0) {
    shown = pay_val;
  } else if (pay_val < -50000) {
    shown = -static_cast<std::int64_t>(pay_val) - 50000;
  } else if (pay_val < -40000) {
    const double scaled = static_cast<double>(state.player.credits) *
                          (static_cast<double>(-pay_val) - 40000.0) * 0.01;
    shown = std::llround(scaled);
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

// <PRK>/<SRK>/<RRK>: the highest-weighted active rank names scan the rank
// (system-cue) table, which is not reconstructed yet; the original falls back
// to STR# 0x7d2 entry 0x155 ("captain") when no rank applies.
[[nodiscard]] std::string MissionRankFallback() {
  return NovaHud_LoadStringEntry(0x7d2, 0x155).value_or("captain");
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
    // TODO(decomp): the special-ship name is picked from the STR# pool at
    // acceptance (g_active_misn +0x40 mission_fleet_name); the port stores
    // only the pool id, so <SN> keeps the [Error] sentinel until then.
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
  ReplaceMissionToken(result,
                      "<SN>",
                      active != nullptr && !special_ship_name.empty()
                          ? special_ship_name
                          : "[Error]");
  // TODO(decomp(0x004444f0)) skipped: <DL> needs the game calendar (the port
  // does not model the start date / deadline clock yet); the original formats
  // Stellar_FormatElapsedTravelTime over the deadline fields and keeps
  // "[Error]" when the deadline equals the current date.
  ReplaceMissionToken(result, "<DL>", "[Error]");
  ReplaceMissionToken(result, "<PN>", player_name);
  ReplaceMissionToken(result, "<PNN>", nickname);
  ReplaceMissionToken(result,
                      "<PSN>",
                      state.player.ship_name.empty() ? std::string("[Error]")
                                                     : state.player.ship_name);
  ReplaceMissionToken(result, "<PST>", ship_type);
  // <OSN> is only resolvable in the ship-offering interaction context
  // (g_ship_states[g_ship_offering_slot].pers name); the port has no offering
  // ship wired yet, so it keeps the sentinel.
  ReplaceMissionToken(result, "<OSN>", "[Error]");
  // TODO(decomp): rank names/weights live on the rank (system-cue) defs, not
  // reconstructed; the fallback arm emits "captain".
  const std::string rank_fallback = MissionRankFallback();
  ReplaceMissionToken(result, "<PRK>", rank_fallback);
  ReplaceMissionToken(result, "<SRK>", rank_fallback);
  ReplaceMissionToken(result, "<RRK>", rank_fallback);
  ReplaceMissionToken(result, "<PAY>", MissionPayText(state, pay_val));
  // FUN_004d45a0: the registration name, or the shared "EV Nova Community"
  // string when no name is registered. The port has no registration system.
  ReplaceMissionToken(result, "<REG>", "EV Nova Community");
  // TODO(decomp(0x004444f0)) skipped: the <PRK%i>/<SRK%i> per-government rank
  // variants and the unregistered letter-scramble block (DAT_007354a4) need
  // the rank system / shareware model respectively.
  return result;
}

// Ghidra 0x00443c60 Mission_HandleMissionOrSurrenderShipReaction. Per-tick
// objective evaluation for one active mission slot: drives the
// objective-complete/failed runtime latches from the mission goal's counters
// (Bible ShipGoal: 0 destroy, 1 disable, 2 board, 3 escort, 4 observe,
// 5 rescue, 6 chase-off; -1 no goal), fails overdue missions, and runs the
// completion payload + auto-abort resolution when the objective first
// completes. Only the in-flight caller is wired; the landing-window caller
// (0x00443780) runs with g_travel_scene_ctx set, which suppresses the
// deadline arm (TODO(decomp) when that gate is ported).
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
  // Deadline arm (flight only; the landing context suppresses it in the
  // original): an expired TimeLimit quick-fails the mission. The
  // STR# 0x7d2 0x11d "mission failed" overlay + centered sound are skipped
  // for invisible missions (flags-accept 0x0400) in the original; the sound
  // itself is TODO(decomp) (transition-sound table not modelled).
  if (!runtime.is_failed && mission.time_limit_days_remaining < 1 &&
      mission.time_limit_days_remaining > -32000) {
    runtime.is_failed = true;
    if ((runtime.flags_primary_at_accept & 0x0400U) == 0U) {
      // STR# 0x7d2 entry 0x11d (1-based) = "Time limit exceeded - mission
      // failed." (Mission_HandleMissionOrSurrenderShipReaction 0x00443c60).
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
    GameState &state, std::int16_t landed_stellar_id, std::uint32_t now_ms) {
  bool resolved_a_success = false;
  for (std::size_t slot = 0; slot < GameState::kMaxActiveMissions; ++slot) {
    if (!state.active_mission_runtime_flags[slot].is_active) {
      continue;
    }
    const auto mission_slot = static_cast<std::int16_t>(slot);
    Mission_HandleMissionOrSurrenderShipReaction(state, mission_slot, now_ms);
    Mission_ProcessInteractionReactionSlotResources(
        state, mission_slot, landed_stellar_id);
    ActiveMission &mission = state.active_missions[slot];
    MissionRuntimeFlags &runtime = state.active_mission_runtime_flags[slot];
    if (NovaStellar_AreStellarsEquivalent(
            state, mission.return_stellar_id, landed_stellar_id)) {
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
          Mission_ResolveMissionSuccess(state, mission_slot);
          resolved_a_success = true;
        }
      } else {
        Mission_ResolveMissionFailure(state, mission_slot, now_ms);
      }
    }
    Mission_HandleMissionOrSurrenderShipReaction(state, mission_slot, now_ms);
  }
  if (resolved_a_success) {
    // Side-effect call: refreshes availability and the resolved-locator cache.
    (void)Mission_EvaluateMissionLists(state);
  }
}

} // namespace game
