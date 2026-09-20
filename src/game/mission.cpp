#include "mission.hpp"

#include "compatibility.hpp"

#include "../brgr_archive.hpp"
#include "boarding_plunder.hpp"
#include "game_state.hpp"
#include "government.hpp"
#include "hud_overlay.hpp"
#include "log.hpp"
#include "mission_internal.hpp"
#include "mission_script.hpp"
#include "nova_random.hpp"
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

using mission_detail::kResourceIdBase;
using mission_detail::ResolveContainingSystem;

namespace {

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
    return NovaSystem_HasExploredToken(state, id);
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
          Ship_ComputeShipTotalCargoCapacity(state);
      if (total_capacity < definition.cargo_qty_tons ||
          Player_ComputeRemainingCargoSpace(state) <
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

[[nodiscard]] std::int16_t
FindSystemContainingStellar(const GameState &state, std::int16_t stellar_id) {
  if (stellar_id < 0) {
    return -1;
  }
  const auto resource_id =
      static_cast<std::int16_t>(stellar_id + kResourceIdBase);
  for (std::size_t system_id = 0; system_id < state.scenario.systems.size();
       ++system_id) {
    const auto &system = state.scenario.systems[system_id];
    if (std::find(system.nav_defs.begin(),
                  system.nav_defs.end(),
                  resource_id) != system.nav_defs.end()) {
      return static_cast<std::int16_t>(system_id);
    }
  }
  return -1;
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
// Mission_EvaluateMissionLists; the p\xefrs ActiveOn cache (PersDef +0x622)
// is refreshed below.
void NovaResources_EvaluateAvailability(GameState &state) {
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
        Mission_CheckReactionConditionSatisfied(state, system.visibility_expr);
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
  // is_available/dominated from the new membership map; the original
  // relies on the last per-tick pass having run with the same map.
  NovaTargeting_UpdateStellarAvailability(state);

  // ShipClass availability cache. The -9999 TechLevel sentinel marks an
  // absent ship class and must suppress the expression result.
  for (auto &ship : state.scenario.ships) {
    ship.is_available_runtime =
        ship.tech_level != kShipClassNonexistentTechLevel &&
        Mission_CheckReactionConditionSatisfied(state, ship.availability_expr);
  }

  // p\x91rs ActiveOn cache (PersDef +0x622). Ghidra 0x00448090 walks all
  // 0x400 personality slots (stride 0x794): a clear +0x623 loaded latch
  // forces +0x622 = 0, otherwise +0x622 caches the +0x644 ActiveOn evaluation.
  // Pers_SpawnShipFromPersDef (0x004235c0) requires +0x622, so without this a
  // personality gated by a control bit never becomes eligible after the bit
  // flips.
  for (auto &pers : state.scenario.pers_defs) {
    pers.is_available_runtime =
        pers.loaded_latch && Mission_CheckReactionConditionSatisfied(
                                 state, pers.availability_expression);
  }

  // Fleet availability is disabled for an absent fleet (lead ship -1).
  for (auto &fleet : state.scenario.fleets) {
    fleet.is_available_runtime =
        fleet.lead_ship_class_id >= 0 &&
        Mission_CheckReactionConditionSatisfied(state, fleet.availability_expr);
  }

  // Mission availability uses LinkSyst == -32000 as its absent/suppressed
  // sentinel; the expression itself is evaluated by the shared NCB helper.
  for (auto &mission : state.scenario.missions) {
    mission.is_available_runtime = mission.link_system_filter != -32000 &&
                                   Mission_CheckReactionConditionSatisfied(
                                       state, mission.availability_expr);
  }

  // n\x91bu region triggers use their rectangle dimensions as the presence
  // test before evaluating ActiveOn.
  for (auto &nebula : state.scenario.nebulae) {
    nebula.active_on = nebula.width >= 1 && nebula.height >= 1 &&
                       Mission_CheckReactionConditionSatisfied(
                           state, nebula.active_on_expression);
  }

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
    for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
      Ship &ship = state.ShipAt(slot);
      if (ship.is_active) {
        ship.current_system_id = resolved;
      }
    }
    for (ActiveShot &shot : state.active_shots) {
      if (shot.life_ticks_remaining > 0.0F) {
        shot.system_id = resolved;
      }
    }
    for (FreeflightObjectState &object : state.freeflight_objects) {
      if (object.lifetime_ticks >= 0.0F) {
        object.system_id = resolved;
      }
    }
  }
}

namespace {

// Mission_SelectMissionStellarByLocator (0x0043d510) filters all stellars
// against a locator family. The clean-room model has no separate travel graph,
// but uses the same availability, visibility, usable-for-travel, and flag
// gates as the original. `reference` is the original param_2 offering stellar
// passed by Mission_ResolveMissionStellarTargets (0x0043d240).
[[nodiscard]] std::int16_t MissionReferenceStellar(const GameState &state) {
  if (state.in_travel_scene) {
    const auto current = state.player.current_system_id;
    if (current >= 0 &&
        static_cast<std::size_t>(current) < state.scenario.systems.size()) {
      // 0x0043d240 initializes this to zero and replaces it with the first
      // nav stellar while g_travel_scene_ctx is set.
      for (const std::int16_t nav :
           state.scenario.systems[static_cast<std::size_t>(current)].nav_defs) {
        if (nav >= kResourceIdBase && nav < kResourceIdBase + 0x800) {
          return static_cast<std::int16_t>(nav - kResourceIdBase);
        }
      }
    }
    return 0;
  }
  // The original reads ShipState.ai_secondary_target_slot in flight. The
  // port's travel commands retain that selection in TravelState instead;
  // honor the ship field when a caller has populated it for a reconstructed
  // path, then use the canonical clean-room travel selection.
  if (state.player.ai_secondary_target_slot >= 0 &&
      state.player.ai_secondary_target_slot < 0x800) {
    return state.player.ai_secondary_target_slot;
  }
  if (state.travel.selected_stellar_id >= kResourceIdBase &&
      state.travel.selected_stellar_id < kResourceIdBase + 0x800) {
    return static_cast<std::int16_t>(state.travel.selected_stellar_id -
                                     kResourceIdBase);
  }
  return -1;
}

[[nodiscard]] std::vector<std::int16_t>
CollectStellarLocatorCandidates(const GameState &state,
                                std::int16_t locator,
                                std::int16_t excluded,
                                std::int16_t reference) {
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
        !NovaTargeting_IsStellarUsableForTravel(stellar)) {
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
    if (!Mission_IsStellarValidRandomDestination(
            state, stellar_id, reference)) {
      return false;
    }
    const auto govt = stellar.government_id;
    if (locator == -2) {
      // Ghidra 0x0043d510: ordinary travel stellar, neither the 0x20
      // restricted/hypergate lane nor the 0x10 land-only lane.
      return (stellar.flags & (0x10U | 0x20U)) == 0U;
    }
    if (locator == -3) {
      // Ghidra 0x0043d510: the 0x20 lane, but still not 0x10.
      return (stellar.flags & 0x20U) != 0U && (stellar.flags & 0x10U) == 0U;
    }
    // All remaining random families use the ordinary (non-0x20) travel lane.
    // The exact-government 9999..14999 arm permits 0x20 only when the target
    // government carries Flags1c 0x800; retain that narrow original quirk.
    if ((stellar.flags & 0x20U) != 0U) {
      const auto target_govt = locator >= 10000 && locator < 15000
                                   ? static_cast<std::int16_t>(locator - 10000)
                                   : static_cast<std::int16_t>(-1);
      if (target_govt < 0 ||
          target_govt >=
              static_cast<std::int16_t>(state.scenario.governments.size()) ||
          (state.scenario.governments[static_cast<std::size_t>(target_govt)]
               .flags_primary &
           0x0800U) == 0U) {
        return false;
      }
    }
    if (locator >= 10000 && locator < 15000) {
      return govt == static_cast<std::int16_t>(locator - 10000);
    }
    if (locator >= 15000 && locator < 20000) {
      // TODO(decomp(0x0043d510)) skipped: the decompile's allied-family
      // existence/pick loops contain inconsistent system-table indexing.
      // Keep the supported government-alliance predicate until disassembly
      // establishes whether that expression is a real binary quirk.
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
                                                 std::int16_t fallback,
                                                 std::int16_t reference) {
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
      CollectStellarLocatorCandidates(state, locator, excluded, reference);
  if (candidates.empty()) {
    return fallback;
  }
  // TODO(decomp(0x0043d510)) skipped: the original proves that at least one
  // of the fixed 0x800 stellar slots is eligible, then repeatedly draws a
  // slot in [0, 0x800) until it passes. Choosing once from the eligible vector
  // is distribution-equivalent but deliberately consumes a different number
  // of RNG values, so later random events can diverge for the same seed.
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
  // TODO(decomp(0x0043bbb0)) skipped: g_offer_random_bypass_flag is seeded
  // from an unmodelled debug-option word (FUN_004cd0b0 word 0xf, all-zero in
  // shipped data) and forces the AvailRandom gate to pass (0x00441f2f). The
  // default-clear path is reproduced here.
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
    if (Player_ComputeRemainingCargoSpace(state) < def.cargo_qty_tons) {
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
      return !CollectStellarLocatorCandidates(
                  state, locator, -1, MissionReferenceStellar(state))
                  .empty();
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

  // Ghidra 0x00441b40 reads FLOAT_00575510 (100.0): missions with Flags
  // 0x0008 require at least one jump's worth of player fuel before they can
  // be offered. The original performs this strict less-than check after the
  // same-system denial and before the ten cached eligibility gates.
  if ((def.flags_primary & 0x0008U) != 0U &&
      state.player.fuel_points < kJumpFuelCost) {
    return false;
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
    return static_cast<std::int16_t>(locator - kResourceIdBase);
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
  const auto selected =
      NovaDude_SelectShipTypeIndex(*dude, state.scenario, false, state.rng);
  return selected >= 0 ? static_cast<std::int16_t>(selected)
                       : static_cast<std::int16_t>(NovaDude_SelectShipTypeIndex(
                             *dude, state.scenario, true, state.rng));
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

// Ghidra 0x0043d240 Mission_ResolveMissionStellarTargets.
void Mission_ResolveMissionStellarTargets(GameState &state,
                                          std::int16_t mission_id) {
  if (mission_id < 0 ||
      mission_id >= static_cast<std::int16_t>(state.scenario.missions.size()) ||
      static_cast<std::size_t>(mission_id) >=
          state.mission_target_resolutions.size()) {
    return;
  }
  const auto &definition =
      state.scenario.missions[static_cast<std::size_t>(mission_id)];
  if (!definition.present) {
    return;
  }
  auto &target =
      state.mission_target_resolutions[static_cast<std::size_t>(mission_id)];
  const auto old_deadline_year = target.deadline_year;
  const auto old_deadline_month = target.deadline_month;
  const auto old_deadline_day = target.deadline_day;
  target = {};
  if (definition.time_limit_days < 1) {
    // Mission_ComputeDateAfterSteps returns without touching its output for a
    // non-positive step count, so a repeated LinkMission refresh preserves
    // the prior deadline bytes.
    target.deadline_year = old_deadline_year;
    target.deadline_month = old_deadline_month;
    target.deadline_day = old_deadline_day;
  }
  // MisnDef +0x0c/+0x0e are the TravelStel/ReturnStel locators (mïsn
  // payload +0x0c/+0x0e); the "on_fail/on_success condition" naming was a
  // misnomer.
  const std::int16_t reference = MissionReferenceStellar(state);
  target.travel_stellar_id = ResolveMissionStellar(
      state, definition.travel_stellar_locator, -1, -1, reference);
  target.travel_system_id =
      ResolveContainingSystem(state, target.travel_stellar_id);
  if (target.travel_system_id != -1) {
    target.travel_system_id =
        Misn_ResolveVisibleSystemForTravel(state, target.travel_system_id);
  }
  if (target.travel_system_id == -1) {
    target.travel_system_id =
        FindSystemContainingStellar(state, target.travel_stellar_id);
  }
  target.return_stellar_id =
      definition.return_stellar_locator == -1
          ? target.travel_stellar_id
          : ResolveMissionStellar(state,
                                  definition.return_stellar_locator,
                                  target.travel_stellar_id,
                                  target.travel_stellar_id,
                                  reference);
  target.return_system_id =
      ResolveContainingSystem(state, target.return_stellar_id);
  if (target.return_system_id == -1) {
    target.return_system_id =
        FindSystemContainingStellar(state, target.return_stellar_id);
  }
  target.cargo_type_id =
      ResolveSpecialShipSystem(state, definition.cargo_type_resource);
  target.cargo_qty_tons =
      ResolveSpecialShipCount(state, definition.cargo_qty_tons);
  target.priority_payload = definition.resource_delta_or_cost;
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

// Ghidra 0x0043c3e0 Misn_ResolveMissionStellarLocators.
void Mission_ResolveMissionStellarLocators(GameState &state) {
  for (std::size_t index = 0; index < state.scenario.missions.size(); ++index) {
    if (state.scenario.missions[index].present) {
      Mission_ResolveMissionStellarTargets(state,
                                           static_cast<std::int16_t>(index));
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
  active.ship_goal = definition->ship_goal;
  active.ship_behavior = definition->ship_behavior;
  active.ship_start = definition->ship_start;
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
  // Ghidra 0x0043f8c0: ShipStart/special spawn mode 1 begins with no ships
  // owed to the respawn stepper; the ordinary modes seed the target count.
  active.goal_count_remaining =
      definition->ship_start == 1 ? 0 : definition->target_ship_count;
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
  if (definition->ship_start < 1) {
    active.spawn_rearm_timer = -1;
  } else if (definition->ship_behavior == 1 && definition->ship_goal == 3) {
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
  copy_text_buffer(active.on_refuse_text, 0x25a);
  copy_text_buffer(active.on_success_text, 0x359);
  copy_text_buffer(active.on_failure_text, 0x458);
  copy_text_buffer(active.on_abort_text, 0x557);
  copy_text_buffer(active.on_ship_done_text, 0x660);
  std::copy(definition->raw_payload.begin(),
            definition->raw_payload.end(),
            active.raw_payload.begin());
  return true;
}

bool Mission_ActivateAtSlot(GameState &state,
                            std::int16_t mission_id,
                            std::int16_t landed_stellar_id,
                            const MissionAcceptanceSink &acceptance) {
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
  // payload +0x15b) runs at the end of the original activate function, and
  // then the same function shows the acceptance dialogs. Tutorial missions use
  // the payload to set chain bits and reveal the destination system (X
  // opcode); a nested S activation recurses through here with the same sink,
  // so its dialogs precede this mission's exactly as in the original.
  Mission_RunMisnScriptPayload(
      state,
      TextOf(state.active_missions[free_slot].on_accept_text),
      static_cast<std::int16_t>(free_slot),
      MissionScriptContext{"OnAccept"},
      acceptance);
  if (acceptance) {
    acceptance(mission_id);
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

// Per-mission auxiliary-fleet rearm tail shared by both 0x00448910 callers.
// Kept as one body so the random-walk RNG order matches the original's
// interleaved head/tail loop in Mission_RefreshActiveMissionSpawnState.
static void RearmMissionTimers(GameState &state, ActiveMission &mission) {
  if ((mission.flags_primary & 0x10) != 0) {
    mission.mission_ship_count_active = mission.mission_ship_count_max;
  }
  mission.rearm_roll_clock =
      static_cast<std::int16_t>(RandomBelow(state, 0x46) + 0x46);
  mission.mission_fleet_metric_c = 0;
}

// Ghidra 0x00448910 Mission_RefreshActiveMissionSpawnState.
void Mission_RefreshActiveMissionSpawnState(GameState &state) {
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
         mission.ship_start == 1)) {
      if (mission.ship_behavior == 1 && mission.ship_goal == 3) {
        mission.spawn_rearm_timer = 30;
      } else {
        mission.spawn_rearm_timer =
            static_cast<std::int16_t>(RandomBelow(state, 100) + 100);
      }
      mission.goal_count_remaining = 0;
    }
    RearmMissionTimers(state, mission);
  }
}

// Ghidra 0x00455e10 inlined tail at 0x004560a0 (a duplicate of the tail of
// 0x00448910). The launch tail re-arms every active mission's auxiliary-fleet
// clock WITHOUT the ShipStart-1 delayed-arrival head above.
void Mission_RearmActiveMissionTimers(GameState &state) {
  for (std::size_t slot = 0; slot < state.active_missions.size(); ++slot) {
    if (!state.active_mission_runtime_flags[slot].is_active) {
      continue;
    }
    RearmMissionTimers(state, state.active_missions[slot]);
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
// When emit_completion_payload is set it then runs the mission's on-abort
// payload (Bible OnAbort, MisnActive +0x5e8). While the travel scene owns the
// world (state.in_travel_scene, the original's g_travel_scene_ctx) the
// released ships are despawned instead: the landing pass runs while the
// destination window owns the world, so the released fleet must not linger
// into the flight scene.
void Mission_ClearMisnSlotAssignments(GameState &state,
                                      std::int16_t mission_slot,
                                      bool emit_completion_payload,
                                      std::uint32_t now_ms,
                                      const MissionAcceptanceSink &acceptance) {
  // now_ms is kept for the caller chain but the AI state-2 stamp reads the
  // shared 60 Hz counter (NovaAi_EnterState2ClearPrimaryTarget).
  (void)now_ms;
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
      NovaAi_EnterState2ClearPrimaryTarget(state, ship);
    }
    if (state.in_travel_scene) {
      ship.is_active = false;
    }
  }
  if (emit_completion_payload) {
    Mission_RunMisnScriptPayload(
        state,
        TextOf(state.active_missions[static_cast<std::size_t>(mission_slot)]
                   .on_abort_text),
        mission_slot,
        MissionScriptContext{"OnAbort"},
        acceptance);
  }
  state.active_missions[static_cast<std::size_t>(mission_slot)]
      .carrying_resources = false;
  state.active_mission_runtime_flags[static_cast<std::size_t>(mission_slot)]
      .is_active = false;
  // The original also invalidates g_last_system_for_ambient_rolls (0xffff);
  // the clean-room ambient-roll cache latch is not modelled (TODO(decomp)).
}

// Applies the Bible CompGovt/CompReward "competing government" reputation
// delta shared by the success resolution and, under kApplyOriginalBugFixes,
// the auto-abort resolution. Full `delta` is added to every system owned by
// `govt`. When `include_relations` is set (Mission_ResolveMissionSuccess
// 0x00440410), systems whose government is hostile/xenophobic to `govt` take
// `-delta/2` and allied systems `+delta/2` (x87 truncation toward zero, not
// round-to-nearest); when clear (the NovaUi_RunMissionComputerWindow
// 0x00446150 Flags 0x0040 manual-abort reversal), only exact-government
// systems change. Independent systems (govt -1) and a `govt` outside 0..0xff
// are no-ops.
void ApplyCompetingGovernmentReputation(GameState &state,
                                        std::int16_t govt,
                                        std::int16_t delta,
                                        bool include_relations) {
  if (govt < 0 || govt >= 0x100) {
    return;
  }
  const auto count = static_cast<std::int16_t>(
      std::min<std::size_t>(state.system_reputation.size(), 0x800));
  for (std::int16_t i = 0; i < count; ++i) {
    const std::int16_t system_govt =
        state.scenario.systems[static_cast<std::size_t>(i)].government_id;
    auto &rep = state.system_reputation[static_cast<std::size_t>(i)];
    if (system_govt == govt) {
      rep = static_cast<std::int16_t>(rep + delta);
    } else if (include_relations && system_govt != -1) {
      if (NovaGovernment_AreGovtsHostileOrXenophobic(
              state.scenario, system_govt, govt)) {
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
  Mission_RunMisnScriptPayload(state,
                               TextOf(mission.on_success_text),
                               mission_slot,
                               MissionScriptContext{"OnSuccess"});
  // On-resolve repeat count (Bible DatePostInc) re-runs the daily world
  // update (0x00466cb0) once per count, advancing the calendar.
  for (std::int16_t i = 0; i < mission.on_resolve_repeat_count; ++i) {
    Mission_TickDailyWorldUpdate(state);
  }
  ApplyCompetingGovernmentReputation(state,
                                     mission.comp_govt_id,
                                     mission.comp_reward_delta,
                                     /*include_relations=*/true);
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
  Mission_RunMisnScriptPayload(state,
                               TextOf(mission.on_failure_text),
                               mission_slot,
                               MissionScriptContext{"OnFailure"});
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
    if (Ship_ComputeShipTotalCargoCapacity(state) < count) {
      // The original shows the STR# 0x7d2 0x165 "not enough cargo space"
      // selection dialog. UI-owned; not reconstructed (TODO(decomp)).
      NovaLog::Todo("mission interaction denied: cargo capacity below {} tons",
                    count);
      return false;
    }
    if (Player_ComputeRemainingCargoSpace(state) < count) {
      // STR# 0x7d2 0x166 "not enough free cargo space" dialog.
      NovaLog::Todo("mission interaction denied: free cargo space below {} "
                    "tons",
                    count);
      return false;
    }
  }
  // g_playerInventoryAndLoadoutDirty + Outfit_RecomputeOutfitDerivedState.
  state.InvalidateDerivedStatCaches();
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
  Mission_RunMisnScriptPayload(state,
                               TextOf(mission.on_failure_text),
                               mission_slot,
                               MissionScriptContext{"OnFailure (quick)"});
  state.active_mission_runtime_flags[slot].is_failed = true;
  if (mission.can_abort) {
    Mission_ClearMisnSlotAssignments(state, mission_slot, false, now_ms);
  }
  // Ambient-roll latch invalidation is not modelled (TODO(decomp)).
}

// Ghidra 0x00447d90 Mission_ResolveMisnSlot. Completes an auto-abort/goal
// mission: on-abort payload (Bible OnAbort, +0x5e8), optional daily rerolls,
// the auto-abort fuel penalty, auto-abort pay, and slot teardown.
void Mission_ResolveMisnSlot(GameState &state,
                             std::int16_t mission_slot,
                             std::uint32_t now_ms) {
  const auto slot = static_cast<std::size_t>(mission_slot);
  ActiveMission &mission = state.active_missions[slot];
  Mission_RunMisnScriptPayload(state,
                               TextOf(mission.on_abort_text),
                               mission_slot,
                               MissionScriptContext{"OnAbort (auto)"});
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
  // BUGFIX(original): the shipped auto-abort resolver never runs the
  // CompGovt/CompReward walk that Mission_ResolveMissionSuccess (0x00440410)
  // applies. Restrict the fix to missions that opt into an auto-abort
  // consequence by either documented flag: Flags2 0x0002 (pay on auto-abort)
  // or Flags 0x0040 (reversal on abort). In the stock data that covers the
  // Refuel Trader missions (+2 Civvies) and Eamon (-200 Wild Geese), plus the
  // 16 Avoid missions, whose 0x0040 reversal (-5x, exact-government systems
  // only, mirroring NovaUi_RunMissionComputerWindow 0x00446150) the original
  // could only reach through a manual abort the family blocks with CanAbort 0.
  // With the policy off, reproduce the original omission. See
  // docs/known_original_bugs.md.
  const bool reversal = (mission.flags_primary & 0x0040U) != 0U;
  const bool pay_on_auto_abort = (mission.flags_secondary & 0x0002U) != 0U;
  if (kApplyOriginalBugFixes && (reversal || pay_on_auto_abort)) {
    ApplyCompetingGovernmentReputation(
        state,
        mission.comp_govt_id,
        reversal ? static_cast<std::int16_t>(mission.comp_reward_delta * -5)
                 : mission.comp_reward_delta,
        /*include_relations=*/!reversal);
  }
  Mission_ClearMisnSlotAssignments(state, mission_slot, false, now_ms);
}

// Ghidra 0x00458802 (inside Stellar_HandleStellarEntryAndExit): draws the
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

// Ghidra 0x0043bbb0 NovaResources_LoadMisnResourceDefs, runtime half. The
// definition table itself is rebuilt by ScenarioData::LoadFromArchives (which
// mirrors the same 1000-entry, id-0x80-strided decode); this reproduces the
// cross-cutting state the original clears around it.
//
// TODO(decomp(0x0043bbb0)) skipped: the original gates / seeds this reset from
// FUN_004cd0b0, which reads a 32-word debug-option block (resource type
// 0x91627567 id 0x80, decoded FourCC "ebug", all-zero in the shipped Nova.rez;
// likely a leftover Mac developer debug/QA directive of unknown origin). Word 1
// skips the whole load, word 9 skips this reset (the Ghidra "restoring from
// save" arm), and word 0xf becomes g_offer_random_bypass_flag. None are ever
// set by the shipped binary, so the port always takes the load+reset path.
void Mission_ResetRuntimeStateOnMissionDefsLoad(GameState &state) {
  // g_last_system_for_ambient_rolls = -1: the clean-room ambient-roll cache
  // latch is not modelled (see Mission_ClearMisnSlotAssignments).
  state.in_travel_scene = false;
  state.mission_speaker_ship_slot = -1;
  // g_travel_destination_window = 0 and g_starmap_selected_system_id = -1 have
  // no clean-room counterpart: the travel window is an SDL modal and the
  // starmap keeps its selection local to NovaStarmap_* rather than in a global.
  state.script_mission_context_slot = -1;
  // Original guard: skip the active-slot/control-bit clear only when the
  // all-zero debug word 9 is set; the port therefore always clears.
  for (auto &flags : state.active_mission_runtime_flags) {
    flags.is_active = false;
    flags.is_failed = false;
  }
  state.control.bits.reset();
  state.control.persisted_bit_bytes.fill(0);
  // DAT_00773eed[1000] = 0; DAT_00774ae2 = -1.
  state.mission_interaction_shown.fill(0);
  state.mission_interaction_context = -1;
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
  if (result != MissionOfferResult::kAccepted) {
    // The original removes an ordinary decline from the current return list;
    // its -1 activation-failure arm also latches the definition as shown.
    // Since the port rebuilds that list on demand, the shown latch represents
    // both cases until the interaction context resets it.
    state.mission_interaction_shown[static_cast<std::size_t>(candidate)] = 1;
  }
  // Recheck timer: DAT_00776af4 = NovaTime_GetTickCount60Hz() +
  // NovaRandom_Range(30) + 30, in 1/60 s ticks. Refresh the port's shared
  // 60 Hz counter from the caller's gameplay clock before stamping it.
  state.tick_60hz = static_cast<std::uint32_t>(
      static_cast<std::uint64_t>(now_ms) * 60 / 1000);
  state.mission_interaction_recheck_tick_60hz =
      static_cast<std::int32_t>(state.tick_60hz) +
      static_cast<std::int32_t>(RandomBelow(state, 0x1e)) + 0x1e;
  return true;
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
  if (mission.ship_goal == -1 && mission.target_ship_count > 0 &&
      mission.goal_count_remaining < 1 &&
      (mission.flags_primary & 0x0001U) != 0U) {
    runtime.objective_complete = false;
  }
  const bool was_objective_complete = runtime.objective_complete;
  const std::int16_t goal = mission.ship_goal;
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
          mission.goal_count_remaining > 0 || mission.ship_start != 1) {
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
    if (mission.ship_goal != -1) {
      Mission_RunMisnScriptPayload(state,
                                   TextOf(mission.on_ship_done_text),
                                   mission_slot,
                                   MissionScriptContext{"OnShipDone"});
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
    if (mission.pickup_mode == 1 && !mission.carrying_resources) {
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
      state.InvalidateDerivedStatCaches();
    }
  }
  if (NovaStellar_AreStellarsEquivalent(
          state, mission.return_stellar_id, landed_stellar_id) &&
      mission.drop_off_mode == 1 && mission.carrying_resources &&
      (runtime.objective_complete || mission.ship_goal == -1 ||
       (mission.ship_goal == 3 && mission.goal_counter_a < 1))) {
    mission.carrying_resources = false;
    if (mission.brief_description_ids[3] != -1) {
      NovaLog::Todo("mission cargo-dropped desc (misn {} id {}) not "
                    "reconstructed yet",
                    mission.mission_template_id,
                    mission.brief_description_ids[3]);
    }
    state.InvalidateDerivedStatCaches();
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
        if (mission.ship_goal == 3 && mission.goal_counter_a < 1) {
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

// Ghidra 0x00426d10 Mission_ShowMissionShipAnnouncement.
void Mission_ShowMissionShipAnnouncement(GameState &state,
                                         std::int16_t hail_quote_id) {
  // NovaAudio_QueueCenteredSound(g_transition_sound_handle_table[4], 1,
  // g_centered_audio_gain): the flight loop consumes pending_ui_sounds and
  // plays transition-table cue 4 directly.
  state.pending_ui_sounds.push_back(
      {/*transition_index=*/4, /*priority_width=*/1});

  // Hail text: `STR ` (hail_quote_id + 4999) when present, else entry
  // hail_quote_id of STR# 0x1bbd (7101, the pers HailQuote pool).
  std::optional<std::string> text;
  if (hail_quote_id >= 0) {
    text = NovaResources_LoadStringResource(
        static_cast<std::uint16_t>(hail_quote_id + 4999));
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
  if (!ambusher.alive || !ambusher.loaded_latch) {
    return;
  }

  // Candidate count: dominated/hazard stellars that are currently
  // available with the availability_flags 0x20 bit clear (StellarDef
  // +0x44/+0x46/+0x34 read through the g_stellar_defs scan).
  std::int16_t candidates = 0;
  for (const Stellar &stellar : state.scenario.stellars) {
    if (stellar.is_available && stellar.dominated &&
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

// Ghidra 0x00454910 Ship_HandlePlayerTargetActionCommand, post-accept arm.
// The original performs the personality and fleet lookup inline after
// NovaUi_RunMissionShipInteractionWindow returns 1. Keep the state mutation
// here so the SDL modal remains a thin presentation wrapper.
bool Mission_HandleAcceptedShipInteraction(GameState &state,
                                           std::int16_t target_ship_slot,
                                           std::uint32_t now_ms) {
  // now_ms is kept for the caller chain; the state-2 stamp reads the shared
  // 60 Hz counter.
  (void)now_ms;
  if (target_ship_slot <= 0 ||
      !state.SlotInRange(static_cast<std::size_t>(target_ship_slot))) {
    return false;
  }
  Ship &target = state.ShipAt(static_cast<std::size_t>(target_ship_slot));
  if (!target.is_active || target.pers_def_slot < 0 ||
      static_cast<std::size_t>(target.pers_def_slot) >=
          state.scenario.pers_defs.size()) {
    return false;
  }

  const std::int16_t pers_slot = target.pers_def_slot;
  PersDef &pers = state.scenario.pers_defs[static_cast<std::size_t>(pers_slot)];
  if ((pers.flags_primary & 0x0100U) != 0U) {
    // PersDef +0x620 is the present latch, not the ActiveOn cache at +0x622.
    pers.alive = false;
  }
  if ((pers.flags_primary & 0x0800U) != 0U) {
    NovaAi_EnterState2ClearPrimaryTarget(state, target);
  }

  std::int16_t mission_slot = -1;
  if (pers.link_mission_id != -1) {
    for (std::size_t slot = 0; slot < state.active_missions.size(); ++slot) {
      const auto &runtime = state.active_mission_runtime_flags[slot];
      if (runtime.is_active &&
          state.active_missions[slot].mission_template_id ==
              pers.link_mission_id) {
        mission_slot = static_cast<std::int16_t>(slot);
        break;
      }
    }
  }
  if (mission_slot < 0 || (pers.flags_primary & 0x0040U) == 0U) {
    return false;
  }
  ActiveMission &mission =
      state.active_missions[static_cast<std::size_t>(mission_slot)];
  if (mission.target_ship_count != 1) {
    return false;
  }

  // Mission_SpawnMissionShipFromDudeDef (0x0041cf40) owns the special
  // ship-type lock for Flags 0x0800 fleets. The old class is passed as the
  // forced class exactly as in the original callsite.
  const int replacement_slot =
      NovaMission_SpawnMissionShipFromDudeDef(state,
                                              mission.dude_def_index,
                                              target.ship_class_id,
                                              state.player.current_system_id,
                                              mission_slot);
  if (replacement_slot < 0) {
    NovaLog::Todo("mission ship {} accepted linked mission {} but replacement "
                  "spawn failed",
                  target_ship_slot,
                  pers.link_mission_id);
    return false;
  }

  Ship &replacement = state.ShipAt(static_cast<std::size_t>(replacement_slot));
  replacement.pos_x = target.pos_x;
  replacement.pos_y = target.pos_y;
  replacement.vel_x = target.vel_x;
  replacement.vel_y = target.vel_y;
  replacement.heading = target.heading;
  if (mission.ship_goal == 3) {
    NovaShip_EnterSquadReturnState(state, replacement);
  }
  target.is_active = false;
  state.player.primary_target_ship_slot =
      static_cast<std::int16_t>(replacement_slot);
  state.ship_reticle_pulse = 0.0F;
  return true;
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
  if (pers.hail_quote_id == -1 || !pers.alive) {
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
    } else if (NovaTargeting_IsThreatToPlayerSquad(state, ship)) {
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
  if ((flags & 0x08U) != 0U && !NovaShip_DoesShipLikePlayer(state, ship)) {
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
