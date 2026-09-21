#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "game/escort_formation.hpp"
#include "game/game_state.hpp"
#include "game/hud_overlay.hpp"
#include "game/outfit.hpp"
#include "game/preferences.hpp"
#include "game/scenario_data.hpp"
#include "game/ship_ai.hpp"
#include "game/spaceflight.hpp"
#include "game/spaceflight_view.hpp"
#include "game/targeting.hpp"
#include "game/travel.hpp"

namespace {

using game::GameState;
using game::NovaSystem_OnSystemEntered;
using game::NovaTravel_CompleteRestrictedTravel;
using game::NovaTravel_CycleDestinationSystem;
using game::NovaTravel_PlayerInJumpRange;
using game::NovaTravel_PlotStarmapDestination;
using game::NovaTravel_ResolveHypergateDestination;
using game::NovaTravel_SelectWormholeDestination;
using game::NovaTravel_Tick;
using game::RestrictedTravelKind;
using game::StellarAnimationState;

// Returns whether zero-based `id` is visited (the per-system fog record):
// discovery_state > 0 mirrored to the pilot's explored bitset. (SystemDef
// is_visible/has_explored_flag are load-time flags, not fog.)
bool IsVisited(const GameState &state, std::int16_t zero_based_id) {
  if (zero_based_id < 0 || static_cast<std::size_t>(zero_based_id) >=
                               state.scenario.systems.size()) {
    return false;
  }
  const std::size_t idx = static_cast<std::size_t>(zero_based_id);
  const auto &sys = state.scenario.systems[idx];
  return sys.discovery_state > 0;
}

// Returns whether `id` appears on the map view without being visited (the
// discovered_this_rebuild latch for one-hop neighbours of visited systems).
bool IsRevealedOnly(const GameState &state, std::int16_t zero_based_id) {
  if (zero_based_id < 0 || static_cast<std::size_t>(zero_based_id) >=
                               state.scenario.systems.size()) {
    return false;
  }
  return !IsVisited(state, zero_based_id) &&
         state.scenario.systems[static_cast<std::size_t>(zero_based_id)]
             .discovered_this_rebuild;
}

} // namespace

TEST_CASE("stellar cycling restores travel mode after system arrival") {
  GameState state;
  state.scenario.systems.resize(1);
  state.scenario.stellars.resize(1);
  state.scenario.systems[0].nav_defs.fill(-1);
  state.scenario.systems[0].nav_defs[0] = 0x80;
  state.scenario.stellars[0].system_id = 0;
  state.scenario.stellars[0].is_available = true;
  state.scenario.stellars[0].flags = 1;
  state.player.current_system_id = 0;
  state.player.travel_transfer_mode = -1; // system-arrival reset

  REQUIRE(game::NovaTargeting_CyclePlayerStellarTarget(state, true));
  CHECK(state.travel.selected_stellar_id == 0x80);
  CHECK(state.player.travel_transfer_mode == 2);
}

TEST_CASE(
    "hypergate animation opens, works, and closes around its transition") {
  GameState state;
  game::Stellar gate;
  gate.availability_flags = game::Stellar::kHypergate;
  gate.custom_picture_or_gate_transition_frame = 3;
  gate.animation_dwell_time = 1;
  StellarAnimationState animation;

  game::NovaStellar_AdvanceAnimationFrame(
      state, gate, 8, true, 1.0F, animation);
  CHECK(animation.current_frame == 1);
  game::NovaStellar_AdvanceAnimationFrame(
      state, gate, 8, true, 1.0F, animation);
  CHECK(animation.current_frame == 2);
  game::NovaStellar_AdvanceAnimationFrame(
      state, gate, 8, true, 1.0F, animation);
  CHECK(animation.current_frame == 3);
  game::NovaStellar_AdvanceAnimationFrame(
      state, gate, 8, true, 1.0F, animation);
  CHECK(animation.current_frame == 3);
  CHECK(animation.previous_frame == 4);

  game::NovaStellar_AdvanceAnimationFrame(
      state, gate, 8, false, 1.0F, animation);
  CHECK(animation.current_frame == 4);
  animation.current_frame = 7;
  game::NovaStellar_AdvanceAnimationFrame(
      state, gate, 8, false, 1.0F, animation);
  CHECK(animation.current_frame == 2);
  game::NovaStellar_AdvanceAnimationFrame(
      state, gate, 8, false, 1.0F, animation);
  CHECK(animation.current_frame == 1);
}

TEST_CASE("hypergate working animation honors return-to-transition flag") {
  GameState state;
  game::Stellar gate;
  gate.availability_flags =
      game::Stellar::kHypergate | game::Stellar::kAnimationReturnToFirstFrame;
  gate.custom_picture_or_gate_transition_frame = 3;
  StellarAnimationState animation{.current_frame = 3, .previous_frame = 4};

  game::NovaStellar_AdvanceAnimationFrame(
      state, gate, 8, true, 0.0F, animation);
  CHECK(animation.current_frame == 4);
  CHECK(animation.previous_frame == 5);
  game::NovaStellar_AdvanceAnimationFrame(
      state, gate, 8, true, 0.0F, animation);
  CHECK(animation.current_frame == 3);
}

TEST_CASE("state-0x15 ship fades from white over its final 16 hold ticks") {
  game::Ship ship;
  ship.ai_state_code = 0x15;
  ship.ai_maneuver_timer_ms = 60.0F;
  CHECK_FALSE(game::NovaShip_EmergencePresentation(ship).visible);
  ship.ai_maneuver_timer_ms = 16.0F;
  const auto start = game::NovaShip_EmergencePresentation(ship);
  CHECK(start.visible);
  CHECK(start.hull_alpha == 0.0F);
  CHECK(start.white_mix == 1.0F);
  ship.ai_maneuver_timer_ms = 8.0F;
  const auto middle = game::NovaShip_EmergencePresentation(ship);
  CHECK(middle.hull_alpha == 0.5F);
  CHECK(middle.white_mix == 1.0F);
  ship.ai_maneuver_timer_ms = 4.0F;
  const auto colorizing = game::NovaShip_EmergencePresentation(ship);
  CHECK(colorizing.hull_alpha == 0.75F);
  CHECK(colorizing.white_mix == 0.5F);
  ship.ai_state_code = 8;
  const auto arrived = game::NovaShip_EmergencePresentation(ship);
  CHECK(arrived.visible);
  CHECK(arrived.hull_alpha == 1.0F);
  CHECK(arrived.white_mix == 0.0F);
}

TEST_CASE("wormholes distinguish linked and random-unlinked destinations") {
  GameState state;
  state.scenario.systems.resize(3);
  for (auto &system : state.scenario.systems) {
    system.is_visible = true;
  }
  state.scenario.stellars.resize(3);
  auto &source = state.scenario.stellars[0];
  source.is_defined = true;
  source.is_available = true;
  source.system_id = 0;
  source.availability_flags = 0x2000;
  auto &linked = state.scenario.stellars[1];
  linked.is_defined = true;
  linked.is_available = true;
  linked.system_id = 1;
  linked.availability_flags = 0x2000;
  source.hyperlinks[0] = 0x81;
  auto &unlinked = state.scenario.stellars[2];
  unlinked.is_defined = true;
  unlinked.is_available = true;
  unlinked.system_id = 2;
  unlinked.availability_flags = 0x2000;
  state.player.current_system_id = 0;

  CHECK(NovaTravel_SelectWormholeDestination(state, 0x80) == 0x81);
  source.hyperlinks.fill(-1);
  linked.hyperlinks[0] = 0x80; // linked wormholes are excluded from fallback
  CHECK(NovaTravel_SelectWormholeDestination(state, 0x80) == 0x82);
}

TEST_CASE("hypergate selection accepts only linked visible systems") {
  GameState state;
  state.scenario.systems.resize(3);
  for (auto &system : state.scenario.systems) {
    system.is_visible = true;
  }
  state.scenario.stellars.resize(2);
  auto &source = state.scenario.stellars[0];
  source.is_defined = true;
  source.system_id = 0;
  source.availability_flags = 0x1000;
  source.hyperlinks[0] = 0x81;
  auto &destination = state.scenario.stellars[1];
  destination.is_defined = true;
  destination.system_id = 1;

  CHECK(NovaTravel_ResolveHypergateDestination(state, 0x80, 1) == 0x81);
  CHECK(NovaTravel_ResolveHypergateDestination(state, 0x80, 2) == -1);
  state.scenario.systems[1].is_visible = false;
  CHECK(NovaTravel_ResolveHypergateDestination(state, 0x80, 1) == -1);
}

TEST_CASE("stellar government ScanMask gates hypergate access") {
  GameState state;
  state.scenario.systems.resize(1);
  state.scenario.ships.resize(1);
  state.scenario.governments.resize(1);
  state.scenario.stellars.resize(1);
  state.player.current_system_id = 0;
  state.player.ship_class_id = 0;
  auto &gate = state.scenario.stellars[0];
  gate.is_defined = true;
  gate.system_id = 0;
  gate.government_id = 0;
  gate.min_status = -0x7fff;
  gate.availability_flags = game::Stellar::kHypergate;
  state.scenario.governments[0].require_lo = 0x20;

  CHECK_FALSE(game::NovaTravel_PlayerMeetsStellarAccess(state, 0x80));
  state.scenario.ships[0].contribute_lo = 0x20;
  CHECK(game::NovaTravel_PlayerMeetsStellarAccess(state, 0x80));
}

TEST_CASE("restricted stellar transfer uses the destination emergence angle") {
  GameState state;
  state.scenario.systems.resize(2);
  state.scenario.systems[1].is_visible = true;
  state.scenario.systems[1].name = "Destination";
  state.scenario.stellars.resize(1);
  auto &destination = state.scenario.stellars[0];
  destination.is_defined = true;
  destination.system_id = 1;
  destination.pos_x = 120;
  destination.pos_y = -40;
  destination.emergence_angle_deg = 90;
  state.player.current_system_id = 0;
  state.cached_stats.speed_raw = 1000.0F;

  REQUIRE(NovaTravel_CompleteRestrictedTravel(
      state, 0x80, RestrictedTravelKind::kHypergate));
  CHECK(state.player.current_system_id == 1);
  CHECK(state.player.pos_x == 120.0F);
  CHECK(state.player.pos_y == -40.0F);
  CHECK(state.player.speed == Catch::Approx(5.0F));
  CHECK(state.player.vel_x == Catch::Approx(5.0F));
  CHECK(state.player.vel_y == Catch::Approx(0.0F).margin(0.0001F));
  CHECK(state.travel.just_completed);
  CHECK(state.travel.starmap_route[0] == 1);
}

// Gives the player a healthy hull (a launched pilot always has full armor;
// a default-constructed ship's armor of 0 would read as disabled and the
// jump dispatch silently refuses -- Ship_IsShipDisabled 0x004687b0).
void MakePlayerHealthy(GameState &state) {
  const auto *cls = state.scenario.Ship(
      static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
  REQUIRE(cls != nullptr);
  state.player.armor_points = static_cast<float>(cls->base_armor);
}

// The discovery pass that runs on a completed jump must mark the reached
// system visited and latch its linked neighbours as revealed (the one-jump-
// ahead window the starmap shows) WITHOUT marking those neighbours explored —
// the original's arrival block floods depth 0 only
// (System_RebuildSystemVisibilityMap(cur, 0, 1)) and the neighbour window is
// the transient discovered_this_rebuild latch.
TEST_CASE(
    "jump discovery marks the destination visited and neighbours revealed") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  REQUIRE(!state.scenario.systems.empty());
  // Exhaustively: clear the fog record (discovery_state; the load-time
  // is_visible/has_explored_flag flags stay as decoded), then pick one system
  // with outward links and confirm only it is visited while each of those
  // links is merely revealed.
  for (auto &sys : state.scenario.systems) {
    sys.discovery_state = 0;
    sys.discovered_this_rebuild = false;
  }

  const std::int16_t start = 0;
  const auto *const sys =
      state.scenario.System(static_cast<std::int16_t>(start + 0x80));
  REQUIRE(sys != nullptr);

  NovaSystem_OnSystemEntered(state, start, 1);

  // The reached system is visited.
  CHECK(IsVisited(state, start));

  // Every outward link (stored as a system resource id >= 0x80) shows on the
  // map but is NOT explored itself.
  for (const std::int16_t link : sys->links) {
    if (link < 0x80) {
      continue;
    }
    const auto dest = static_cast<std::int16_t>(link - 0x80);
    CHECK(IsRevealedOnly(state, dest));
    CHECK_FALSE(IsVisited(state, dest));
  }

  // A system that was neither reached nor linked stays fully hidden.
  bool found_unrelated = false;
  for (std::size_t i = 0; i < state.scenario.systems.size(); ++i) {
    const std::int16_t id = static_cast<std::int16_t>(i);
    if (id == start) {
      continue;
    }
    bool linked = false;
    for (const std::int16_t link : sys->links) {
      if (link == static_cast<std::int16_t>(id + 0x80)) {
        linked = true;
        break;
      }
    }
    if (!linked) {
      CHECK_FALSE(IsVisited(state, id));
      CHECK_FALSE(IsRevealedOnly(state, id));
      found_unrelated = true;
    }
  }
  CHECK(found_unrelated);
}

// Marking an out-of-range / negative system is a safe no-op.
TEST_CASE("jump discovery guards out-of-range systems") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  const auto visited_count = [&state] {
    return static_cast<std::size_t>(std::count_if(
        state.scenario.systems.begin(),
        state.scenario.systems.end(),
        [](const game::System &sys) { return sys.discovery_state > 0; }));
  };
  const std::size_t before = visited_count();
  NovaSystem_OnSystemEntered(state, -5, 1);
  NovaSystem_OnSystemEntered(
      state, static_cast<std::int16_t>(state.scenario.systems.size() + 10), 1);
  CHECK(visited_count() == before);
}

// Ghidra 0x00448be0 'E' token: the operand is a system RESOURCE id (0x80 +
// zero-based index), so E128 tests system 0, not system 128.
TEST_CASE("has-explored NCB token rebases the system resource id") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.scenario.systems[0].discovery_state = 1;
  CHECK(NovaSystem_HasExploredToken(state, 0x80));
  CHECK_FALSE(NovaSystem_HasExploredToken(state, 0));
  CHECK_FALSE(NovaSystem_HasExploredToken(state, 0x81));
  CHECK_FALSE(NovaSystem_HasExploredToken(state, 0x7f));
  CHECK_FALSE(NovaSystem_HasExploredToken(state, 0x880));
}

// Plotting a starmap destination arms the travel slot for a directly-linked
// system so 'j' jumps there, and does not arm a slot when the destination
// cannot be reached in a single jump.
TEST_CASE("starmap plot arms the travel slot only for linked destinations") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  // Pick a starting system with at least one outward link to a real system.
  std::int16_t start = -1;
  std::int16_t linked_dest = -1;
  std::int16_t unrelated = -1;
  for (std::size_t i = 0; i < state.scenario.systems.size() && linked_dest < 0;
       ++i) {
    const auto *sys =
        state.scenario.System(static_cast<std::int16_t>(i + 0x80));
    if (!sys) {
      continue;
    }
    for (const std::int16_t link : sys->links) {
      if (link >= 0x80 &&
          static_cast<std::size_t>(link - 0x80) <
              state.scenario.systems.size() &&
          link - 0x80 != static_cast<std::int16_t>(i)) {
        start = static_cast<std::int16_t>(i);
        linked_dest = static_cast<std::int16_t>(link - 0x80);
        break;
      }
    }
  }
  REQUIRE(start >= 0);
  REQUIRE(linked_dest >= 0);
  state.player.current_system_id = start;

  // Find a system that is neither start nor linked_dest, for the no-link case.
  for (std::size_t i = 0; i < state.scenario.systems.size(); ++i) {
    const std::int16_t id = static_cast<std::int16_t>(i);
    if (id == start || id == linked_dest) {
      continue;
    }
    bool linked = false;
    const auto *start_sys =
        state.scenario.System(static_cast<std::int16_t>(start + 0x80));
    for (const std::int16_t link : start_sys->links) {
      if (link == static_cast<std::int16_t>(id + 0x80)) {
        linked = true;
        break;
      }
    }
    if (!linked) {
      unrelated = id;
      break;
    }
  }
  REQUIRE(unrelated >= 0);

  // Directly-linked destination: arms the travel slot. Mode-3 plotted jumps
  // clear the stellar selection (the reticle hides; the nav panel switches to
  // the Hyperspace display).
  REQUIRE(NovaTravel_PlotStarmapDestination(state, linked_dest));
  CHECK(state.travel.starmap_destination_system_id == linked_dest);
  CHECK(state.travel.travel_slot >= 0);
  CHECK_FALSE(state.travel.selected_stellar_is_manual);
  CHECK(state.travel.selected_stellar_id == -1);

  // Reset for the negative cases, then check a non-linked destination.
  state.travel.starmap_destination_system_id = -1;
  state.travel.travel_slot = -1;
  const bool not_armed = NovaTravel_PlotStarmapDestination(state, unrelated);
  CHECK(not_armed == false);
  CHECK(state.travel.starmap_destination_system_id == unrelated);
  CHECK(state.travel.travel_slot == -1);

  // Plotting the current system records no jump.
  state.travel.starmap_destination_system_id = -1;
  state.travel.travel_slot = -1;
  NovaTravel_PlotStarmapDestination(state, start);
  CHECK(state.travel.starmap_destination_system_id == start);
  CHECK(state.travel.travel_slot == -1);
}

// Regression: Kania (0) links to Tichel (1) at hyperlink slot 3, but has no
// travel-nav stellar there (nav_defs[3] is empty). A plotted jump to Tichel
// must still arm (destination resolved purely against links) and 'j' must land
// in Tichel, not fall back to the nearest travel point (which would go to a
// different system). This locked in the fix where PlotStarmapDestination no
// longer required a NavDef paired with the hyperlink slot.
TEST_CASE("plot to a hyperlink without a paired nav-def stellar still jumps") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);

  // Verify the scenario shape that exercises the fix: Kania links to Tichel at
  // a slot whose nav_defs entry is empty.
  state.player.current_system_id = 0; // Kania
  const game::System *kania = state.scenario.System(0x80);
  REQUIRE(kania != nullptr);
  int tichel_slot = -1;
  for (std::size_t i = 0; i < kania->links.size(); ++i) {
    if (kania->links[i] == 0x81 /* Tichel resource */) {
      tichel_slot = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(tichel_slot >= 0);
  CAPTURE(tichel_slot);
  REQUIRE(static_cast<std::size_t>(tichel_slot) < kania->nav_defs.size());
  // Historically this slot had an empty nav_def, exercising the no-paired-
  // NavDef branch; arm regardless.
  const bool armed = NovaTravel_PlotStarmapDestination(state, 1);
  REQUIRE(armed);
  CHECK(state.travel.starmap_destination_system_id == 1);
  CHECK(state.travel.travel_slot == tichel_slot);
  CHECK(state.travel.destination_system_id == 1);

  // Pressing 'j' to completion must land in Tichel (system id 1). The ship
  // starts beyond the no-jump radius so the engage passes; the brake is short
  // (velocity 0) and the fire lands once the hold passes 30 ticks with no
  // 'Warp up' voice active (~1 s of frames at 16.67 ms).
  state.player.fuel_points = 500;
  state.player.pos_x = 0.0F;
  state.player.pos_y = -3000.0F; // beyond the 1000 px no-jump radius
  for (int f = 0; f < 800 && !state.travel.just_completed; ++f) {
    NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  }
  CHECK(state.travel.just_completed);
  CHECK(state.player.current_system_id == 1);
}

// The destination-system cycle (the Backslash / command-0x60 channel) must
// advance through the current system's directly-linked systems, wrapping, and
// arm the travel slot + destination so 'j' jumps there. On a system with no
// travelable links it must return -1 and clear the armed destination.
TEST_CASE("destination-system cycle steps and wraps") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  // Pick a starting system with at least two distinct outward links.
  std::int16_t start = -1;
  std::vector<std::int16_t> dests;
  for (std::size_t i = 0; i < state.scenario.systems.size(); ++i) {
    const game::System *sys =
        state.scenario.System(static_cast<std::int16_t>(i + 0x80));
    if (!sys) {
      continue;
    }
    dests.clear();
    for (const std::int16_t link : sys->links) {
      if (link >= 0x80 &&
          static_cast<std::size_t>(link - 0x80) <
              state.scenario.systems.size() &&
          link - 0x80 != static_cast<std::int16_t>(i)) {
        dests.push_back(static_cast<std::int16_t>(link - 0x80));
      }
    }
    if (dests.size() >= 2) {
      start = static_cast<std::int16_t>(i);
      break;
    }
  }
  REQUIRE(start >= 0);
  REQUIRE(dests.size() >= 2);
  state.player.current_system_id = start;

  // First press selects the first destination (travel_slot was -1).
  const std::int16_t d0 =
      NovaTravel_CycleDestinationSystem(state, /*forward=*/true);
  REQUIRE(d0 == dests[0]);
  CHECK(state.travel.starmap_destination_system_id == d0);
  CHECK(state.travel.travel_slot >= 0);
  CHECK(state.travel.destination_system_id == d0);

  // Next press moves to the second destination (not back to the first).
  const std::int16_t d1 =
      NovaTravel_CycleDestinationSystem(state, /*forward=*/true);
  REQUIRE(d1 == dests[1]);
  CHECK(d1 != d0);

  // Backward wraps from the second to the first.
  const std::int16_t back =
      NovaTravel_CycleDestinationSystem(state, /*forward=*/false);
  REQUIRE(back == d0);

  // A system with no outward links clears the armed destination and returns -1.
  std::int16_t dead_end = -1;
  for (std::size_t i = 0; i < state.scenario.systems.size(); ++i) {
    const game::System *sys =
        state.scenario.System(static_cast<std::int16_t>(i + 0x80));
    if (!sys) {
      continue;
    }
    bool has_link = false;
    for (const std::int16_t link : sys->links) {
      if (link >= 0x80 &&
          static_cast<std::size_t>(link - 0x80) <
              state.scenario.systems.size() &&
          link - 0x80 != static_cast<std::int16_t>(i)) {
        has_link = true;
        break;
      }
    }
    if (!has_link) {
      dead_end = static_cast<std::int16_t>(i);
      break;
    }
  }
  REQUIRE(dead_end >= 0);
  state.player.current_system_id = dead_end;
  const std::int16_t no_dest =
      NovaTravel_CycleDestinationSystem(state, /*forward=*/true);
  REQUIRE(no_dest == -1);
  CHECK(state.travel.starmap_destination_system_id == -1);
  CHECK(state.travel.travel_slot == -1);
}

// The pre-fire turn-around: engaging 'j' while the ship is still moving must
// turn the hull (toward the reverse of the velocity, i.e. back toward the jump
// vector) and brake the velocity to the |round(vel)| < 2 stop before the hold
// fires -- the original's jump dispatch in Ship_HandlePlayerShipCore
// (0x0044c195 / 0x0044fff0). The jump still completes to the plotted
// destination.
TEST_CASE("jump engages with a moving ship: turns around and brakes") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);

  // Kania (0) -> Tichel (1); give the ship a healthy outward drift so the
  // turn-around actually has something to brake, parked beyond the 1000 px
  // no-jump radius so the engage passes.
  state.player.current_system_id = 0;
  state.player.pos_x = 3000.0F;
  state.player.pos_y = 3000.0F;
  state.player.heading = 0.0F; // up
  state.player.vel_x = 8.0F;   // flying rightward (heading 90 deg)
  state.player.vel_y = 0.0F;
  state.player.fuel_points = 500;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  const float initial_speed =
      std::hypot(state.player.vel_x, state.player.vel_y);
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  // Engage and let the brake run: after a few frames the heading must
  // have changed (turning around) and the speed must have decayed.
  NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  REQUIRE(state.travel.engaging);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kBrake);
  const float heading_after_engage = state.player.heading;
  for (int f = 0; f < 30; ++f) {
    NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  }
  CHECK(state.player.heading != heading_after_engage);
  CHECK(std::hypot(state.player.vel_x, state.player.vel_y) < initial_speed);

  // Let the whole sequence run; it must still land in Tichel. The brake needs
  // ~240 ticks to slow |vel| 8 -> the |round(vel)| < 2 stop at 0.99204/tick,
  // then the hold fires once it passes 30 ticks (~1 s) with no 'Warp up'
  // voice active in tick-only tests.
  for (int f = 0; f < 1200 && !state.travel.just_completed; ++f) {
    NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  }
  CHECK(state.travel.just_completed);
  CHECK(state.player.current_system_id == 1);
}

// The stationary hold starts the 'Warp up' cue the moment the brake hands
// off (the original pre-stages the sound at the stop), then the fire lands
// once the hold passes the 30-tick engage threshold with the cue finished.
TEST_CASE("jump hold starts Warp up then fires past the engage threshold") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.player.pos_x = 0.0F;
  state.player.pos_y = -3000.0F; // beyond the no-jump radius
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  // The stopped handoff enters the hold and latches the cue immediately.
  NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  REQUIRE(state.travel.engaging);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kBrake);
  NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kHold);
  CHECK(state.travel.warp_up_started);
  CHECK(state.warp_up_sound_pending);
  state.warp_up_sound_pending = false; // consumed by the audio loop

  // With no cue voice active the fire lands just past the 30-tick threshold
  // (0.5 ticks per 16.67 ms frame -> ~62 frames).
  for (int f = 0; f < 200 && !state.travel.just_completed; ++f) {
    NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  }
  CHECK(state.travel.just_completed);
  CHECK(state.player.current_system_id == 1);
}

// Regression: the jump hold must seed the PLAYER's ai_station_hold_timer
// (0x0044c548) and stamp ai_mode_start_time_ms (0x0044c54f).
// Ship_SyncJumpStateToSquad copies that timer to attached escorts and enters
// them into state 0x0B, and the player-led jump spin-up in Ship_HandleShip
// (0x00433050) reads both fields to ramp the escort's departure. Before the
// fix the port kept the hold clock in a travel-local, so escorts stopped with
// the player but never aligned or jumped.
TEST_CASE("jump hold clocks player station timer so escorts sync") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.player.pos_y = -3000.0F; // beyond the no-jump radius
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;

  // One attached escort (behavior 6, no stellar attachment); no others.
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    state.ShipAt(slot).is_active = false;
  }
  auto &escort = state.ShipAt(1);
  escort.is_active = true;
  escort.ship_instance_id = 1;
  escort.ship_class_id = 0;
  escort.current_system_id = 0;
  escort.ai_behavior_code = 6;
  escort.squad_leader_ship_slot = 0;
  escort.defense_fleet_home_stellar_id = -1;
  escort.mission_fleet_slot = -1;
  escort.ai_station_hold_timer = -1.0F;
  escort.armor_points = 1000.0F;
  escort.shield_points = 0.0F;

  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));
  NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  REQUIRE(state.travel.engaging);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kBrake);
  NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kHold);

  // Hold-begin seeds the timer to 2.0. The per-tick squad sync then mirrors it
  // into the escort and enters state 0x0B.
  CHECK(state.player.ai_station_hold_timer == Catch::Approx(2.0F));
  NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  CHECK(state.player.ai_station_hold_timer > 1.0F);
  CHECK(escort.ai_station_hold_timer > 1.0F);
  CHECK(escort.ai_state_code == 0x0b);

  // The player's desired heading must track the map jump bearing (the original
  // stores it at 0x0044eeb3). The escort mode-0xD spin-up reads it via
  // leader_delta / the leader-desired fallback, so a stale 0 made every escort
  // aim straight up.
  const auto *src = state.scenario.System(0x80);
  const auto *dst = state.scenario.System(0x81);
  REQUIRE(src != nullptr);
  REQUIRE(dst != nullptr);
  int expected_deg = static_cast<int>(
      std::lround(std::atan2(static_cast<float>(dst->pos_x - src->pos_x),
                             -static_cast<float>(dst->pos_y - src->pos_y)) *
                  (180.0F / 3.14159265358979323846F)));
  expected_deg = ((expected_deg % 360) + 360) % 360;
  CAPTURE(expected_deg);
  CHECK(state.player.ai_desired_heading_deg == expected_deg);

  // Let the player finish turning onto the bearing, then run the escort's
  // mode-0xD controls: it must aim at the player's heading, not 0/up.
  for (int f = 0; f < 200; ++f) {
    NovaTravel_Tick(state,
                    /*travel_input=*/false,
                    16.67F,
                    /*warp_up_sound_active=*/true);
  }
  escort.ai_control_mode = 0x0d;
  game::NovaAi_ApplyControls(state, escort, 0.5F);
  CHECK(std::abs(static_cast<int>(escort.ai_desired_heading_deg) -
                 expected_deg) <= 1);

  // The clock keeps running through the hold (the escort spin-up reads it) and
  // is cleared once the fire lands.
  for (int f = 0; f < 400 && !state.travel.just_completed; ++f) {
    NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  }
  CHECK(state.travel.just_completed);
  CHECK(state.player.ai_station_hold_timer < 0.0F);
}

// The original stop gate uses the x87 FIST correction idiom to truncate each
// velocity component toward zero. A component below 2 therefore starts the
// cue even when rounding-to-nearest would produce 2.
TEST_CASE("jump stop gate truncates velocity before starting Warp up") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.player.pos_y = -3000.0F;
  state.player.vel_x = 1.75F;
  state.player.vel_y = -1.75F;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kBrake);
  NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);

  CHECK(state.travel.jump_phase == game::TravelState::JumpPhase::kHold);
  CHECK(state.warp_up_sound_pending);
}

// Ship_CheckSpecialLoadoutCapability (0x0046d080): a class with Flags2 0x0020
// skips the per-axis stop gate (0x0044c4db) and keeps its momentum through the
// hold (0x0044c72e) -- "jump without slowing down".
TEST_CASE("fast-jump class skips the brake and keeps momentum") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.player.pos_x = 3000.0F;
  state.player.pos_y = 3000.0F;
  state.player.heading = 0.0F;
  state.player.vel_x = 8.0F;
  state.player.vel_y = 0.0F;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  state.scenario.ships[static_cast<std::size_t>(state.player.ship_class_id)]
      .flags_secondary |= 0x0020U;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kBrake);
  NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  // The stop gate is bypassed: the hold begins with the ship still moving and
  // the slow-phase damp never runs.
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kHold);
  const float speed = std::hypot(state.player.vel_x, state.player.vel_y);
  CHECK(speed == Catch::Approx(8.0F));
  for (int f = 0; f < 10; ++f) {
    NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
    CHECK(std::hypot(state.player.vel_x, state.player.vel_y) ==
          Catch::Approx(speed));
  }
  // The sequence still completes to the plotted destination.
  for (int f = 0; f < 400 && !state.travel.just_completed; ++f) {
    NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  }
  CHECK(state.travel.just_completed);
  CHECK(state.player.current_system_id == 1);
}

TEST_CASE("owned fast-jump outfit grants the capability") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.player.pos_x = 3000.0F;
  state.player.pos_y = 3000.0F;
  state.player.vel_x = 8.0F;
  state.player.vel_y = 0.0F;
  // The scenario loads a full 0x200 outfit table, so reuse the last slot
  // instead of appending (id must stay inside the 0x200 owned-count array).
  REQUIRE(!state.scenario.outfits.empty());
  const std::size_t id = state.scenario.outfits.size() - 1;
  REQUIRE(id < state.inventory.outfit_owned_count.size());
  game::Outfit &outfit = state.scenario.outfits[id];
  outfit.mod_type = static_cast<std::int16_t>(game::OutfitEffect::kFastJump);
  outfit.mod_val = 0;
  outfit.alt_mod_types = {};
  outfit.alt_mod_vals = {};
  state.inventory.outfit_owned_count[id] = 1;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kBrake);
  NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  CHECK(state.travel.jump_phase == game::TravelState::JumpPhase::kHold);
}

// A merely-defined fast-jump outfit is not owned (count 0), so the ordinary
// brake still runs.
TEST_CASE("defined but unowned fast-jump outfit does not grant") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.player.pos_x = 3000.0F;
  state.player.pos_y = 3000.0F;
  state.player.vel_x = 8.0F;
  state.player.vel_y = 0.0F;
  REQUIRE(!state.scenario.outfits.empty());
  const std::size_t id = state.scenario.outfits.size() - 1;
  REQUIRE(id < state.inventory.outfit_owned_count.size());
  game::Outfit &outfit = state.scenario.outfits[id];
  outfit.mod_type = static_cast<std::int16_t>(game::OutfitEffect::kFastJump);
  state.inventory.outfit_owned_count[id] = 0;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kBrake);
  NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  CHECK(state.travel.jump_phase == game::TravelState::JumpPhase::kBrake);
}

// The fast-jump ModType in an alternate effect slot with ModVal 0 still grants
// the capability: the four ModTypes are equivalent and no positive value or
// activation is needed.
TEST_CASE("fast-jump in an alternate ModType with ModVal 0 grants") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.player.pos_x = 3000.0F;
  state.player.pos_y = 3000.0F;
  state.player.vel_x = 8.0F;
  state.player.vel_y = 0.0F;
  REQUIRE(!state.scenario.outfits.empty());
  const std::size_t id = state.scenario.outfits.size() - 1;
  REQUIRE(id < state.inventory.outfit_owned_count.size());
  game::Outfit &outfit = state.scenario.outfits[id];
  outfit.mod_type = 0;
  outfit.mod_val = 0;
  outfit.alt_mod_types[1] =
      static_cast<std::int16_t>(game::OutfitEffect::kFastJump);
  outfit.alt_mod_vals[1] = 0;
  state.inventory.outfit_owned_count[id] = 1;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kBrake);
  NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  CHECK(state.travel.jump_phase == game::TravelState::JumpPhase::kHold);
}

// The fast-jump capability does not bypass the no-jump radius around the
// system centre (0x0044c220 + Stellar_ComputeTravelRangeSq): the engage is
// refused with STR# 0x7d2 0x2a before the stop gate is reached.
TEST_CASE("fast-jump is still denied inside the no-jump range") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.player.pos_x = 0.0F;
  state.player.pos_y = 0.0F;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  state.scenario.ships[static_cast<std::size_t>(state.player.ship_class_id)]
      .flags_secondary |= 0x0020U;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  CHECK_FALSE(state.travel.engaging);
  CHECK(state.travel.jump_phase == game::TravelState::JumpPhase::kIdle);
}

// Inertialess hulls use the 0x0044f0e3 speed-decay arm of the jump brake
// (Outfit_ShipIsInertialess 0x0046df70), not the 0x0044f127 turnaround: the
// maintained scalar speed decays by the effective thrust step while the
// heading and velocity direction stay fixed, then the stop gate hands off to
// the hold.
TEST_CASE(
    "inertialess hull decays scalar speed without turning in the jump brake") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.player.pos_x = 3000.0F;
  state.player.pos_y = 3000.0F;
  constexpr float kHeading = 0.75F;
  constexpr float kInitialSpeed = 8.0F;
  state.player.heading = kHeading;
  state.player.speed = kInitialSpeed;
  state.player.vel_x = std::sin(kHeading) * kInitialSpeed;
  state.player.vel_y = -std::cos(kHeading) * kInitialSpeed;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  state.scenario.ships[static_cast<std::size_t>(state.player.ship_class_id)]
      .flags_secondary |= 0x0040U;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kBrake);
  const float thrust = state.cached_stats.thrust_raw / 10000.0F * 2.0F;
  REQUIRE(thrust > 0.0F);
  const float ticks = 16.67F / (1000.0F / 30.0F);
  const float expected_speed = std::max(0.0F, kInitialSpeed - thrust * ticks);
  NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kBrake);
  CHECK(state.player.heading == Catch::Approx(kHeading));
  CHECK(state.player.speed == Catch::Approx(expected_speed));
  CHECK(std::hypot(state.player.vel_x, state.player.vel_y) ==
        Catch::Approx(expected_speed).margin(1e-4));

  // Coast straight to the stop gate without the turnaround's reverse heading.
  int guard = 0;
  while (state.travel.jump_phase == game::TravelState::JumpPhase::kBrake &&
         guard++ < 400) {
    CHECK(state.player.heading == Catch::Approx(kHeading));
    NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  }
  CHECK(state.travel.jump_phase == game::TravelState::JumpPhase::kHold);
}

// The owned inertial dampener (ModType 38, kInertialDampener) selects the same
// arm as the class Flags2 0x40 flag.
TEST_CASE("owned inertial dampener selects the jump-brake speed-decay arm") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.player.pos_x = 3000.0F;
  state.player.pos_y = 3000.0F;
  constexpr float kHeading = 0.75F;
  state.player.heading = kHeading;
  state.player.speed = 8.0F;
  state.player.vel_x = std::sin(kHeading) * 8.0F;
  state.player.vel_y = -std::cos(kHeading) * 8.0F;
  REQUIRE(!state.scenario.outfits.empty());
  const std::size_t id = state.scenario.outfits.size() - 1;
  REQUIRE(id < state.inventory.outfit_owned_count.size());
  game::Outfit &outfit = state.scenario.outfits[id];
  outfit.mod_type =
      static_cast<std::int16_t>(game::OutfitEffect::kInertialDampener);
  outfit.mod_val = 0;
  outfit.alt_mod_types = {};
  outfit.alt_mod_vals = {};
  state.inventory.outfit_owned_count[id] = 1;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kBrake);
  const float speed_before = state.player.speed;
  NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kBrake);
  CHECK(state.player.speed < speed_before);
  CHECK(state.player.heading == Catch::Approx(kHeading));
}

// Off-heading velocity with the scalar at the zero floor: the arm must not
// make the speed negative and must not snap the velocity onto heading*speed in
// one frame.
TEST_CASE("inertialess jump brake floors scalar speed and steers off-heading "
          "velocity") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.player.pos_x = 3000.0F;
  state.player.pos_y = 3000.0F;
  constexpr float kHeading = 0.0F;
  state.player.heading = kHeading; // heading points up
  state.player.speed = 0.0F;
  state.player.vel_x = 8.0F; // off-heading: velocity is sideways
  state.player.vel_y = 0.0F;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  state.scenario.ships[static_cast<std::size_t>(state.player.ship_class_id)]
      .flags_secondary |= 0x0040U;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kBrake);
  const float thrust = state.cached_stats.thrust_raw / 10000.0F * 2.0F;
  REQUIRE(thrust > 0.0F);
  const float ticks = 16.67F / (1000.0F / 30.0F);
  // Ship_SteerVelocityTowardShipHeading step = eff_thrust * 4.0 * ticks
  // (_DAT_005754ac = 4.0). At speed 0 the commanded velocity is (0,0), so each
  // axis may move at most one step and vel_y (already 0) stays put.
  const float step = thrust * 4.0F * ticks;
  NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kBrake);
  CHECK(state.player.speed == 0.0F); // floored, never negative
  CHECK(state.player.heading == Catch::Approx(kHeading));
  CHECK(state.player.vel_x == Catch::Approx(8.0F - step));
  CHECK(state.player.vel_y == Catch::Approx(0.0F));
}

// The fast-jump capability is tested first at 0x0044c4db, before the
// inertialess split, so it wins: the hold begins with the ship still moving.
TEST_CASE("fast-jump wins over inertialess in the jump brake") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.player.pos_x = 3000.0F;
  state.player.pos_y = 3000.0F;
  state.player.heading = 0.0F;
  state.player.speed = 8.0F;
  state.player.vel_x = 8.0F;
  state.player.vel_y = 0.0F;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  state.scenario.ships[static_cast<std::size_t>(state.player.ship_class_id)]
      .flags_secondary |= 0x0040U | 0x0020U;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kBrake);
  NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  CHECK(state.travel.jump_phase == game::TravelState::JumpPhase::kHold);
  CHECK(std::hypot(state.player.vel_x, state.player.vel_y) ==
        Catch::Approx(8.0F));
}

// Engaged hold: the shared manual-flight inertialess tail (Ghidra 0x0044cffe ->
// 0x0044d05b) still runs, so a fast-jump inertialess hull steers velocity
// toward heading*speed while the hold turns onto the jump bearing. Heading and
// jump bearing coincide here, isolating the steering from the auto-turn.
TEST_CASE("fast-jump inertialess hold steers off-heading velocity") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.player.pos_x = 3000.0F;
  state.player.pos_y = 3000.0F;
  constexpr float kHeading = 0.0F;
  constexpr float kSpeed = 8.0F;
  state.player.heading = kHeading;
  state.player.speed = kSpeed;
  state.player.vel_x = kSpeed; // off-heading: velocity is sideways
  state.player.vel_y = 0.0F;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  state.scenario.ships[static_cast<std::size_t>(state.player.ship_class_id)]
      .flags_secondary |= 0x0040U | 0x0020U;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kBrake);
  NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kHold);
  // Pin the jump bearing to the current heading: the hold must not turn, so
  // only the shared steering tail moves the velocity.
  state.travel.jump_heading_rad = kHeading;

  const float thrust = state.cached_stats.thrust_raw / 10000.0F * 2.0F;
  REQUIRE(thrust > 0.0F);
  const float ticks = 16.67F / (1000.0F / 30.0F);
  // Ship_SteerVelocityTowardShipHeading step = eff_thrust * 4.0 * ticks
  // (_DAT_005754ac = 4.0). Command = heading*speed = (0, -8).
  const float step = thrust * 4.0F * ticks;
  NovaTravel_Tick(state, /*travel_input=*/false, 16.67F); // first kHold body
  CHECK(state.player.vel_x == Catch::Approx(kSpeed - step));
  CHECK(state.player.vel_y == Catch::Approx(-step));
  // Inertialess hulls keep the maintained scalar authoritative; it must not be
  // overwritten with hypot(velocity).
  CHECK(state.player.speed == Catch::Approx(kSpeed));
}

// During the hold the auto-turn runs before the shared inertialess steering
// (Ghidra 0x0044cffe -> 0x0044d05b), so the velocity chases the freshly turned
// heading.
TEST_CASE("fast-jump inertialess hold turns while steering velocity") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.player.pos_x = 3000.0F;
  state.player.pos_y = 3000.0F;
  constexpr float kHeading = 0.0F;
  constexpr float kJumpBearing = 0.5F;
  constexpr float kSpeed = 8.0F;
  state.player.heading = kHeading;
  state.player.speed = kSpeed;
  state.player.vel_x = 0.0F;
  state.player.vel_y = -kSpeed; // aligned to the old heading, off the bearing
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  state.scenario.ships[static_cast<std::size_t>(state.player.ship_class_id)]
      .flags_secondary |= 0x0040U | 0x0020U;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kHold);
  state.travel.jump_heading_rad = kJumpBearing;

  NovaTravel_Tick(state, /*travel_input=*/false, 16.67F); // first kHold body
  CHECK(state.player.heading > kHeading);
  CHECK(state.player.heading <= kJumpBearing);
  // The auto-turned heading has sin > 0, so the commanded heading*speed pulls
  // velocity toward positive x; a steering pass that ran before the turn (or
  // did not run) would leave vel_x at zero and vel_y at -speed.
  CHECK(state.player.vel_x > 0.0F);
}

// Ordinary (non-fast, non-inertialess) holds keep the slow-phase damp.
TEST_CASE("ordinary hold applies the slow-phase velocity damp") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.player.pos_x = 3000.0F;
  state.player.pos_y = 3000.0F;
  state.player.heading = 0.0F;
  state.player.vel_x = 0.5F;
  state.player.vel_y = -0.5F;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kHold);
  state.travel.jump_heading_rad = 0.0F;

  // g_hyperspace_slow_phase_velocity_damp (0x005755f8, double 0.98006866).
  const float ticks = 16.67F / (1000.0F / 30.0F);
  const float damp = std::pow(0.98006866F, ticks);
  NovaTravel_Tick(state, /*travel_input=*/false, 16.67F); // first kHold body
  CHECK(state.player.vel_x == Catch::Approx(0.5F * damp));
  CHECK(state.player.vel_y == Catch::Approx(-0.5F * damp));
}

// A non-fast inertialess hull in the hold runs the slow damp on the velocity
// but keeps its maintained scalar speed (+0x48): the original's 0x0044f414
// damp writes vel_x/vel_y only, and the shared tail then steers toward
// heading*speed. Overwriting the scalar with hypot() would decay it.
TEST_CASE("inertialess hold keeps its maintained scalar through the damp") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.player.pos_x = 3000.0F;
  state.player.pos_y = 3000.0F;
  constexpr float kHeading = 0.0F;
  constexpr float kSpeed = 1.0F;
  state.player.heading = kHeading;
  state.player.speed = kSpeed;
  state.player.vel_x = 0.0F;
  state.player.vel_y = -kSpeed;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  state.scenario.ships[static_cast<std::size_t>(state.player.ship_class_id)]
      .flags_secondary |= 0x0040U;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kHold);
  state.travel.jump_heading_rad = kHeading;

  NovaTravel_Tick(state, /*travel_input=*/false, 16.67F); // first kHold body
  CHECK(state.player.speed == Catch::Approx(kSpeed));
  // The steering tail pulls the damped velocity back toward heading*speed.
  CHECK(state.player.vel_y == Catch::Approx(-kSpeed).margin(1e-4F));
}

TEST_CASE("jump payroll uses fleet travel days after escort restoration",
          "[travel][escort]") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);
  state.player.ship_class_id = 0;
  state.scenario.ships[0].mass_tons = 50;
  auto &escort_class = state.scenario.ships[1];
  escort_class.mass_tons = 250;
  escort_class.cost = 10000;
  escort_class.default_ai_behavior = 1;
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    state.ShipAt(slot).is_active = false;
  }
  auto &escort = state.ShipAt(1);
  escort.is_active = true;
  escort.ship_instance_id = 1;
  escort.ship_class_id = 1;
  escort.current_system_id = 0;
  escort.ai_behavior_code = 6;
  escort.squad_leader_ship_slot = 0;
  escort.mission_fleet_slot = -1;
  escort.escort_origin_mark = 1;
  escort.armor_points = static_cast<float>(escort_class.base_armor);
  escort.shield_points = static_cast<float>(escort_class.base_shield);
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.player.pos_y = -3000;
  state.player.credits = 1000;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;

  int expected_credits = 700;
  bool released = false;
  SECTION("three days charged once") {}
  SECTION("shortfall in the second period releases the escort") {
    state.player.credits = 150;
    expected_credits = 50;
    released = true;
  }
  SECTION("captured escort adds travel days without upkeep") {
    escort.escort_origin_mark = 0;
    expected_credits = 1000;
  }

  const auto before_jump = state.player.credits;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));
  for (int frame = 0; frame < 1200 && !state.travel.just_completed; ++frame) {
    NovaTravel_Tick(state, frame == 0, 16.67F);
  }
  REQUIRE(state.travel.just_completed);
  CHECK(state.travel.pending_payroll_periods == 3);
  CHECK(state.player.credits == before_jump);
  CHECK(escort.current_system_id == 0);
  game::NovaSystem_RestorePlayerEscorts(state, false, 0);
  REQUIRE(escort.current_system_id == 1);
  int messages = 0;
  const auto show_text = [&](const std::string &text) {
    CHECK_FALSE(text.empty());
    ++messages;
  };
  game::NovaTravel_ProcessArrivalPayroll(state, show_text);
  CHECK(state.player.credits == expected_credits);
  CHECK(escort.is_active == !released);
  CHECK(messages == (released ? 1 : 0));
  CHECK(state.travel.pending_payroll_periods == 0);
  game::NovaTravel_ProcessArrivalPayroll(state, show_text);
  CHECK(state.player.credits == expected_credits);
  CHECK(messages == (released ? 1 : 0));
}

TEST_CASE("jump heading uses the linked systems' map vector") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.player.pos_x = 123456.0F;
  state.player.pos_y = -654321.0F;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  const auto *source = state.scenario.System(0x80);
  const auto *destination = state.scenario.System(0x81);
  REQUIRE(source != nullptr);
  REQUIRE(destination != nullptr);
  const float expected =
      std::atan2(static_cast<float>(destination->pos_x - source->pos_x),
                 -static_cast<float>(destination->pos_y - source->pos_y));
  CHECK(std::abs(std::remainder(state.travel.jump_heading_rad - expected,
                                6.28318530717958646F)) < 0.001F);
}

// The fire lands while the ship is still in the ORIGIN system and hurls it
// 1350 px from the in-system origin (0,0) along the reverse of the jump
// bearing, moving at max speed along its heading; there is no separate
// in-tunnel coast phase after the fire.
TEST_CASE("jump fire hurls the ship 1350 px past the in-system origin") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.player.pos_x = 0.0F;
  state.player.pos_y = -3000.0F; // beyond the no-jump radius
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  const float max_speed = state.cached_stats.speed_raw / 100.0F;
  REQUIRE(max_speed > 0.0F);

  for (int f = 0; f < 400 && !state.travel.just_completed; ++f) {
    NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  }
  REQUIRE(state.travel.just_completed);
  REQUIRE(state.player.current_system_id == 1);
  CHECK_FALSE(state.travel.engaging);
  CHECK(state.travel.jump_phase == game::TravelState::JumpPhase::kIdle);

  // Arrived at max speed along the heading, exactly 1350 px from the
  // in-system origin (0,0), not from the destination's galaxy-map position
  // (g_hyperspace_engage_velocity_hurl; original zeroes the ship then adds
  // the polar hurl).
  const float arrival_speed =
      std::hypot(state.player.vel_x, state.player.vel_y);
  CHECK(arrival_speed == Catch::Approx(max_speed).epsilon(0.01F));
  const float hurl = std::hypot(state.player.pos_x, state.player.pos_y);
  CHECK(hurl == Catch::Approx(1350.0F).epsilon(0.01F));
}

// The fire moment must arm the full-screen flash (the original's centered
// effect 0x32, the 'boom' white frame) and the Mac _FadeWhiteOut. The jump
// hold also drives a progressive build-up (Mac _FadeWhiteIn), so the
// intensity rises before the boom; NovaTravel_Tick leaves the boom at full and
// arms the fade-out, which the spaceflight loop decays over 1.5 s.
TEST_CASE("jump fire arms the screen flash and fade-out") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  bool flash_armed = false;
  // Engage with the ship beyond the no-jump radius; the brake + hold then
  // lead to the fire.
  state.player.pos_x = 0.0F;
  state.player.pos_y = -3000.0F;
  for (int f = 0; f < 420 && !state.travel.just_completed; ++f) {
    NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
    if (state.screen_flash_intensity > 0.0F) {
      flash_armed = true;
    }
  }
  CHECK(state.travel.just_completed);
  CHECK(flash_armed);
  CHECK(state.screen_flash_intensity == 1.0F);
  CHECK(state.screen_flash_mode == GameState::ScreenFlashMode::kFadeOut);
}

// Mac progressive white fade-in during the jump hold: the tunnel scalar
// FLOAT_007354a0 = (progress - 55) * 5, clamped [0,100], is a one-shot trigger
// for the 1.5 s _FadeWhiteIn (Ship_HandlePlayerShipCore 0x0044aa70 top block,
// Mac _HandlePlayer). It is not continuously sampled opacity. The Windows
// build computes progress * 0.3 - 15 at 0x00450601 and calls the stubbed
// NoSys_NoOp_00467e60, so its build-up is dead; the port follows the Mac
// display behaviour unconditionally.
TEST_CASE(
    "hyperspace hold starts one-shot screen fade-in at the Mac threshold") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.player.pos_x = 0.0F;
  state.player.pos_y = -3000.0F;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  // Hold the 'Warp up' cue open so the fire cannot land while the fade runs.
  bool saw_hold = false;
  bool saw_fade_trigger = false;
  for (int f = 0; f < 900 && !state.travel.just_completed; ++f) {
    NovaTravel_Tick(state,
                    /*travel_input=*/true,
                    16.67F,
                    /*warp_up_sound_active=*/true);
    if (state.travel.jump_phase != game::TravelState::JumpPhase::kHold) {
      continue;
    }
    saw_hold = true;
    if (state.screen_flash_mode == GameState::ScreenFlashMode::kFadeIn) {
      saw_fade_trigger = true;
      break;
    }
  }
  CHECK(saw_hold);
  CHECK(saw_fade_trigger);
  CHECK(state.screen_flash_fade_in_started);
  CHECK(state.screen_flash_intensity == 0.0F);

  // The display fade owns the opacity. A simulation tick after the trigger
  // must not re-arm or overwrite it.
  NovaTravel_AdvanceScreenFlash(state, 750.0F);
  CHECK(state.screen_flash_intensity == Catch::Approx(0.5F));
  NovaTravel_Tick(state, true, 16.67F, true);
  CHECK(state.screen_flash_intensity == Catch::Approx(0.5F));
  CHECK(state.screen_flash_mode == GameState::ScreenFlashMode::kFadeIn);

  // Arrival interrupts the incomplete fade-in but still starts from the Mac
  // full-white endpoint before the 1.5 s reveal.
  NovaTravel_Tick(state, true, 16.67F, false);
  CHECK(state.travel.just_completed);
  CHECK(state.screen_flash_intensity == 1.0F);
  CHECK(state.screen_flash_mode == GameState::ScreenFlashMode::kFadeOut);
}

// Mac display-fade arming: the hypergate transfer runs _FadeWhiteOut (0x63c40,
// a 1.5 s CoreGraphics display fade), while the wormhole paints white but
// resets the starfield instead, so it keeps the one-frame flash.
TEST_CASE("hypergate arms the fade-out but wormhole only flashes") {
  const auto make_state = [] {
    GameState state;
    state.scenario.systems.resize(2);
    state.scenario.systems[1].is_visible = true;
    state.scenario.stellars.resize(1);
    auto &destination = state.scenario.stellars[0];
    destination.is_defined = true;
    destination.system_id = 1;
    destination.pos_x = 10.0F;
    destination.pos_y = 20.0F;
    state.player.current_system_id = 0;
    state.cached_stats.speed_raw = 1000.0F;
    return state;
  };

  GameState hypergate = make_state();
  REQUIRE(NovaTravel_CompleteRestrictedTravel(
      hypergate, 0x80, RestrictedTravelKind::kHypergate));
  CHECK(hypergate.screen_flash_intensity == 1.0F);
  CHECK(hypergate.screen_flash_mode == GameState::ScreenFlashMode::kFadeOut);

  GameState wormhole = make_state();
  REQUIRE(NovaTravel_CompleteRestrictedTravel(
      wormhole, 0x80, RestrictedTravelKind::kWormhole));
  CHECK(wormhole.screen_flash_intensity == 1.0F);
  CHECK(wormhole.screen_flash_mode == GameState::ScreenFlashMode::kInstant);
}

// The render-side flash driver uses the Mac display fade duration for both
// directions. The pre-trigger kBuildup state remains unchanged until the
// progress threshold is crossed; kInstant keeps the ~60 ms fallback.
TEST_CASE("screen flash display fades use Mac durations") {
  GameState state;
  state.screen_flash_intensity = 0.5F;
  state.screen_flash_mode = GameState::ScreenFlashMode::kBuildup;
  NovaTravel_AdvanceScreenFlash(state, 16.67F);
  CHECK(state.screen_flash_intensity == 0.5F);
  CHECK(state.screen_flash_mode == GameState::ScreenFlashMode::kBuildup);

  state.screen_flash_intensity = 0.0F;
  state.screen_flash_mode = GameState::ScreenFlashMode::kFadeIn;
  NovaTravel_AdvanceScreenFlash(state, 750.0F);
  CHECK(state.screen_flash_intensity == Catch::Approx(0.5F));
  NovaTravel_AdvanceScreenFlash(state, 750.0F);
  CHECK(state.screen_flash_intensity == Catch::Approx(1.0F));
  CHECK(state.screen_flash_mode == GameState::ScreenFlashMode::kFadeIn);

  state.screen_flash_intensity = 1.0F;
  state.screen_flash_mode = GameState::ScreenFlashMode::kFadeOut;
  NovaTravel_AdvanceScreenFlash(state, 750.0F);
  CHECK(state.screen_flash_intensity == Catch::Approx(0.5F));
  CHECK(state.screen_flash_mode == GameState::ScreenFlashMode::kFadeOut);

  state.screen_flash_intensity = 1.0F;
  state.screen_flash_mode = GameState::ScreenFlashMode::kInstant;
  NovaTravel_AdvanceScreenFlash(state, 60.0F);
  CHECK(state.screen_flash_intensity == 0.0F);
  CHECK(state.screen_flash_mode == GameState::ScreenFlashMode::kNone);
}

// A disabled (fire-restricted) ship cannot ENGAGE a jump: the original's
// dispatch bails on Ship_IsShipDisabled (0x0044c1a3) before any denial
// feedback -- a silent refusal, not an overlay.
TEST_CASE("disabled ship cannot engage a jump") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.current_system_id = 0;
  state.player.pos_x = 0.0F;
  state.player.pos_y = -3000.0F; // beyond the no-jump radius
  state.player.fuel_points = 500;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  // Default-constructed hull: armor 0 -> disabled. A healthy hull would
  // engage here (see the fire tests).
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));
  NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  CHECK_FALSE(state.travel.engaging);
}

// Becoming disabled between the engage and the fire collapses the jump
// (Ship_HandlePlayerShipCore 0x0044b037): the sequence aborts with NO system
// change, the flash + 'Warp out' boom arm, the 'Warp up' cue cancels, and --
// once the tunnel ramp had begun (progress past onset) -- the velocity is
// rebuilt at min(progress, max speed) along the heading.
TEST_CASE("disabled mid-jump collapses the field in the same system") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.player.pos_x = 0.0F;
  state.player.pos_y = -3000.0F;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  REQUIRE(state.travel.engaging);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kBrake);

  // Run the brake + hold with the 'Warp up' cue still "playing" so the fire
  // cannot land before the tunnel onset (~2.12 s into the hold).
  for (int f = 0; f < 600; ++f) {
    NovaTravel_Tick(state,
                    /*travel_input=*/false,
                    16.67F,
                    /*warp_up_sound_active=*/true);
    if (state.travel.jump_phase == game::TravelState::JumpPhase::kHold &&
        f > 200) {
      break;
    }
  }
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kHold);
  // Past the tunnel onset (progress > 0: 127 ticks after the hold stamp).
  NovaTravel_Tick(state, false, 16.67F, true);

  const std::int16_t system_before = state.player.current_system_id;
  const float heading_before = state.player.heading;

  // Now cripple the hull: the next tick must collapse the field.
  state.player.armor_points = 0.0F;
  NovaTravel_Tick(state, false, 16.67F, true);

  CHECK_FALSE(state.travel.engaging);
  CHECK(state.travel.jump_phase == game::TravelState::JumpPhase::kIdle);
  CHECK(state.player.current_system_id == system_before);
  CHECK(state.screen_flash_intensity == 1.0F);
  // The collapse keeps the legacy one-frame flash (the Mac abort's fade is an
  // open question; see travel.cpp).
  CHECK(state.screen_flash_mode == GameState::ScreenFlashMode::kInstant);
  CHECK(state.warp_out_sound_pending);
  CHECK(state.warp_up_cancel_pending);
  // Exit velocity: min(progress, max speed) along the heading.
  const float speed = std::hypot(state.player.vel_x, state.player.vel_y);
  CHECK(speed > 0.0F);
  CHECK(speed <= state.cached_stats.speed_raw / 100.0F + 0.01F);
  CHECK(state.player.vel_x ==
        Catch::Approx(std::sin(heading_before) * speed).epsilon(0.01F));
  CHECK(state.player.vel_y ==
        Catch::Approx(-std::cos(heading_before) * speed).epsilon(0.01F));
}

// Disabled DURING the brake (before the tunnel onset): the jump still aborts,
// but the velocity clause does not apply -- the ship keeps its damped
// turnaround velocity.
TEST_CASE("disabled before tunnel onset aborts without the exit velocity") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.player.pos_x = 0.0F;
  state.player.pos_y = -3000.0F;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  REQUIRE(state.travel.engaging);

  // A couple of brake frames only -- nowhere near the ~2.12 s onset.
  NovaTravel_Tick(state, false, 16.67F);
  const float vel_x_before = state.player.vel_x;
  const float vel_y_before = state.player.vel_y;

  state.player.armor_points = 0.0F;
  NovaTravel_Tick(state, false, 16.67F);

  CHECK_FALSE(state.travel.engaging);
  CHECK(state.player.current_system_id == 0);
  CHECK_FALSE(state.warp_out_sound_pending == false);
  CHECK(state.warp_up_cancel_pending);
  CHECK(state.player.vel_x == vel_x_before);
  CHECK(state.player.vel_y == vel_y_before);
}

// The "entered jump range" cue (Ship_HandlePlayerShipCore flight tail,
// ~0x00450a2c): with a plotted mode-3 jump armed, the rising edge of the
// in-range latch queues transition-table sound [4]; the latch updates
// whenever the jump is armed (no re-edge while it stays in range, no edge on
// leaving, suppressed while engaged or without fuel for a jump).
TEST_CASE("entering jump range with a plotted jump cues the UI sound") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  MakePlayerHealthy(state);
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;

  // Plot while far outside the no-jump radius: the first armed tick crosses
  // the latch edge and queues the cue.
  state.player.pos_x = 3000.0F;
  state.player.pos_y = 3000.0F;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));
  NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  REQUIRE(state.pending_ui_sounds.size() == 1);
  CHECK(state.pending_ui_sounds[0].transition_index == 4);
  CHECK(state.pending_ui_sounds[0].priority_width == 1);
  CHECK(state.travel.jump_range_cue_latch);
  state.pending_ui_sounds.clear();

  // Still in range: no new edge, no new cue.
  NovaTravel_Tick(state, false, 16.67F);
  CHECK(state.pending_ui_sounds.empty());

  // Back inside the radius: the latch clears silently (leaving range is not
  // an edge).
  state.player.pos_x = 0.0F;
  state.player.pos_y = 0.0F;
  NovaTravel_Tick(state, false, 16.67F);
  CHECK(state.pending_ui_sounds.empty());
  CHECK_FALSE(state.travel.jump_range_cue_latch);

  // Leaving again re-arms the edge and the cue fires once more.
  state.player.pos_x = 3000.0F;
  state.player.pos_y = 3000.0F;
  NovaTravel_Tick(state, false, 16.67F);
  REQUIRE(state.pending_ui_sounds.size() == 1);
  CHECK(state.pending_ui_sounds[0].transition_index == 4);
  state.pending_ui_sounds.clear();

  // No fuel for a jump: the latch still arms but the cue is suppressed
  // (the original gates the sound on fuel >= kJumpFuelCost).
  state.player.pos_x = 0.0F;
  state.player.pos_y = 0.0F;
  NovaTravel_Tick(state, false, 16.67F);
  state.player.fuel_points = 50.0F;
  state.player.pos_x = 3000.0F;
  state.player.pos_y = 3000.0F;
  NovaTravel_Tick(state, false, 16.67F);
  CHECK(state.travel.jump_range_cue_latch);
  CHECK(state.pending_ui_sounds.empty());
}

// The shared no-jump-radius probe (flight-tail cue + travel-panel colour,
// Ship_HandlePlayerShipCore ~0x00450a2c / NovaUi_DrawTravelStatusPanel
// 0x0045e400): true when no NON-restricted nav of the current system is
// within Stellar_ComputeTravelRangeSq of the SYSTEM CENTER; a system with
// only restricted (0x3000) navs is always "in range".
TEST_CASE(
    "jump-range probe keys on the system center and non-restricted navs") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.current_system_id = 0;

  // Parked on the system center with usable navs: inside the radius.
  state.player.pos_x = 0.0F;
  state.player.pos_y = 0.0F;
  CHECK_FALSE(NovaTravel_PlayerInJumpRange(state));

  // Far out: beyond the radius.
  state.player.pos_x = 3000.0F;
  state.player.pos_y = 3000.0F;
  CHECK(NovaTravel_PlayerInJumpRange(state));
}

// Stellar_GetJumpSequenceDuration60Hz (0x0046efb0) returns the engine cue
// (snd 128, shipped 364) normally and the noengine cue (snd 129, shipped 252)
// while g_x2_mode_active is set; both keep the 350 missing-resource fallback.
TEST_CASE("jump sequence duration selects the x2 noengine cue and fallback",
          "[travel]") {
  GameState state;
  state.jump_duration_engine_60hz = 364;
  state.jump_duration_noengine_60hz = 252;
  CHECK(game::NovaTravel_JumpSequenceDuration60Hz(state) ==
        Catch::Approx(364.0F));
  state.x2_mode_active = true;
  CHECK(game::NovaTravel_JumpSequenceDuration60Hz(state) ==
        Catch::Approx(252.0F));
  state.jump_duration_noengine_60hz = 350; // missing/broken cue fallback
  CHECK(game::NovaTravel_JumpSequenceDuration60Hz(state) ==
        Catch::Approx(350.0F));
}

// The scanner's hyperspace-committed guard (Ghidra 0x00401800) now uses the
// decoded ShipClassDef.jump_duration_multiplier instead of a pinned 1.0.
// Onset: tunnel_elapsed_60hz > 35 * (364*0.01) / multiplier^2.
TEST_CASE("jump onset guard scales with the class jump multiplier",
          "[travel][contraband]") {
  GameState state;
  state.scenario.ships.resize(1);
  state.player.ship_class_id = 0;
  state.jump_duration_engine_60hz = 364; // shipped snd 128 cue

  state.scenario.ships[0].jump_duration_multiplier = 1.3F;
  state.travel.tunnel_elapsed_60hz = 100.0F; // onset = 127.4/1.69 = 75.4
  CHECK(game::NovaTravel_PlayerPastJumpOnset(state));

  state.scenario.ships[0].jump_duration_multiplier = 0.91F;
  // onset = 127.4/0.8281 = 153.8, so 100 ticks is still before onset.
  CHECK_FALSE(game::NovaTravel_PlayerPastJumpOnset(state));

  state.player.ship_class_id = -1; // missing class -> fallback 1.0
  CHECK_FALSE(game::NovaTravel_PlayerPastJumpOnset(state));
}

// Ghidra 0x0046c250 System_GetEffectiveMurkPercent: raw SystemDef.murk (clamped
// >= 0) plus every owned ModType 0x1c (MurkMod) outfit's owned_count * ModVal
// across all four mod slots, clamped to [0, 100].
TEST_CASE("effective murk sums owned MurkMod outfits and clamps",
          "[travel][murk]") {
  GameState state;
  state.scenario.systems.resize(2);
  state.player.current_system_id = 0;
  state.scenario.systems[0].murk = 20;
  state.scenario.systems[1].murk = 90;

  // One outfit with ModType 0x1c / ModVal +10 in its first alternate slot.
  state.scenario.outfits.resize(1);
  state.scenario.outfits[0].alt_mod_types[0] = 0x1c;
  state.scenario.outfits[0].alt_mod_vals[0] = 10;

  CHECK(game::NovaSystem_GetEffectiveMurkPercent(state) == 20); // none owned
  state.inventory.outfit_owned_count[0] = 1;
  CHECK(game::NovaSystem_GetEffectiveMurkPercent(state) == 30); // 20 + 10
  state.inventory.outfit_owned_count[0] = 30;
  CHECK(game::NovaSystem_GetEffectiveMurkPercent(state) == 100); // clamp

  // A negative raw murk clamps to 0 before the modifier is added.
  state.scenario.systems[0].murk = -5;
  state.inventory.outfit_owned_count[0] = 1;
  CHECK(game::NovaSystem_GetEffectiveMurkPercent(state) == 10);

  // A negative ModVal reduces murk and the total clamps at 0.
  state.scenario.outfits[0].alt_mod_vals[0] = -50;
  CHECK(game::NovaSystem_GetEffectiveMurkPercent(state) == 0);

  // The value follows the current system.
  state.player.current_system_id = 1;
  CHECK(game::NovaSystem_GetEffectiveMurkPercent(state) == 40); // 90 - 50
}

TEST_CASE("flight tutorial range hints advance the hint state",
          "[travel][hints]") {
  GameState state;
  game::NovaPreferences prefs;
  prefs.bindings.ResetToDefaults();
  state.spaceflight_frame_counter = 0;

  // State 0, outside the 2,000,000 px^2 no-jump radius: the "not yet far
  // enough away" hint (STR# 0x7d2 0x1e) latches the state to 1.
  state.travel.travel_hint_state = 0;
  state.player.pos_x = 2000.0F; // squared distance 4,000,000
  state.player.pos_y = 0.0F;
  game::PlayerTick_FlightTutorialHints(state, prefs);
  CHECK(state.travel.travel_hint_state == 1);
  REQUIRE(state.hud_overlay.active);
  CHECK(state.hud_overlay.message.find("beyond safe hyperspace range") !=
        std::string::npos);

  // Latched at 1, beyond the 10,000,000 px^2 threshold: the "nothing to find
  // out here" hint (0x1f) latches the state to 2.
  state.spaceflight_frame_counter = 60;
  state.player.pos_x = 4000.0F; // squared distance 16,000,000
  game::NovaHud_ClearOverlayMessage(state);
  game::PlayerTick_FlightTutorialHints(state, prefs);
  CHECK(state.travel.travel_hint_state == 2);
  REQUIRE(state.hud_overlay.active);
  CHECK(state.hud_overlay.message.find("nothing to find out here") !=
        std::string::npos);

  // Once latched at 2 the tick is inert.
  state.spaceflight_frame_counter = 120;
  game::NovaHud_ClearOverlayMessage(state);
  game::PlayerTick_FlightTutorialHints(state, prefs);
  CHECK_FALSE(state.hud_overlay.active);
}
