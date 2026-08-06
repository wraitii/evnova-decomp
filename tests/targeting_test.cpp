#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/game_state.hpp"
#include "game/scenario_data.hpp"
#include "game/targeting.hpp"

namespace {

using game::GameState;
using game::Outfit;
using game::ShipClass;
using game::Stellar;
using game::System;

// A deterministic scenario with one visible star system and a set of stellars,
// injected directly (no archive), so the stellar-targeting predicates are
// unit-testable in isolation.
struct Fixture {
  GameState state;
  System *sys = nullptr;
  Stellar *orbital = nullptr;
  Stellar *landing = nullptr;

  Fixture() {
    state.player.current_system_id = 0;
    state.player.ship_class_id = 0;

    // A ship class whose base shield/armor back the landing refill.
    ShipClass sc;
    sc.base_shield = 30;
    sc.base_armor = 30;
    sc.base_fuel = 300;
    state.scenario.ships.push_back(sc); // index 0 == resource 0x80

    System s;
    s.name = "Test System";
    s.links = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    s.nav_defs = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    s.is_visible = true;
    // Three nav stellars: an orbital (0x80), a landable body (0x81) and a
    // restricted travel lane (0x82).
    s.nav_defs[0] = 0x80;
    s.nav_defs[1] = 0x81;
    s.nav_defs[2] = 0x82;
    state.scenario.systems.push_back(s); // index 0 == resource 0x80
    sys = &state.scenario.systems[0];

    Stellar o;
    o.name = "Port Kane";
    // Engaged travel target: control bit (0x1) + engaged (0x80), with a live
    // sprite, so the target-active gate sees active == engaged -> usable.
    o.flags = 0x81;
    o.availability_flags = 0;
    o.pos_x = 100;
    o.pos_y = 0;
    o.sprite_population = 1; // a live ambient sprite
    o.sprite_handle_active = true;
    o.system_id = 0;
    state.scenario.stellars.push_back(o); // index 0 == resource 0x80

    Stellar l;
    l.name = "Orbital Colony";
    // Engaged landable target: control bit + engaged + landable (0x2).
    l.flags = 0x83;
    l.availability_flags = 0;
    l.pos_x = 200;
    l.pos_y = 0;
    l.sprite_population = 1;
    l.sprite_handle_active = true;
    l.system_id = 0;
    state.scenario.stellars.push_back(l); // index 1 == resource 0x81

    // A reserved travel-lane star (availability_flags & 0x3000): target-active
    // but not usable for travel/landing.
    Stellar r;
    r.name = "Jump Gate";
    r.flags = 0x81;
    r.availability_flags = 0x1000;
    r.pos_x = 300;
    r.pos_y = 0;
    r.sprite_population = 1;
    r.sprite_handle_active = true;
    r.system_id = 0;
    state.scenario.stellars.push_back(r); // index 2 == resource 0x82

    // Capture the pointers only after all pushes so later growth cannot
    // reallocate the backing vector out from under them.
    orbital = &state.scenario.stellars[0];
    landing = &state.scenario.stellars[1];
  }
};

} // namespace

// Stellar_IsStellarActive (0x0046E3C0): population > 0 and (handle active or
// engaged).

TEST_CASE("a stellar is active when it has spawned a sprite that is live",
          "[targeting]") {
  Fixture f;
  CHECK(game::NovaTargeting_IsStellarActive(*f.orbital));

  // Population drops to zero: inactive.
  f.orbital->sprite_population = 0;
  CHECK_FALSE(game::NovaTargeting_IsStellarActive(*f.orbital));

  // Population present but the handle is released and not engaged: inactive.
  f.orbital->sprite_population = 1;
  f.orbital->sprite_handle_active = false;
  f.orbital->engage_access = 0;
  CHECK_FALSE(game::NovaTargeting_IsStellarActive(*f.orbital));
}

TEST_CASE("an engaged stellar stays active without a live sprite handle",
          "[targeting]") {
  Fixture f;
  f.orbital->sprite_handle_active = false;
  f.orbital->engage_access = 1;
  CHECK(game::NovaTargeting_IsStellarActive(*f.orbital));
  f.orbital->engage_access = 0;
  CHECK_FALSE(game::NovaTargeting_IsStellarActive(*f.orbital));
}

// Stellar_StellarTargetsSpriteSetActive (0x0046E3F0): control bit set AND
// sprite-active == engaged flag.

TEST_CASE("sprite set is target-active when activity agrees with the engaged "
          "flag",
          "[targeting]") {
  Fixture f;
  // Orbital: control bit set, active AND engaged -> agree -> target-active.
  CHECK(game::NovaTargeting_StellarTargetsSpriteSetActive(*f.orbital));

  // Flip engagement off while the sprite stays active (disagree): not
  // target-active.
  f.orbital->flags = 0x1; // engaged bit cleared
  CHECK_FALSE(game::NovaTargeting_StellarTargetsSpriteSetActive(*f.orbital));

  // Dormant stellar (no sprite, not engaged): agrees (both off) so is
  // target-active, but the control bit must still be set.
  f.orbital->flags = 0x1;
  f.orbital->sprite_population = 0;
  f.orbital->sprite_handle_active = false;
  CHECK(game::NovaTargeting_StellarTargetsSpriteSetActive(*f.orbital));

  // Without the control bit nothing is target-active.
  f.orbital->flags = 0x0;
  CHECK_FALSE(game::NovaTargeting_StellarTargetsSpriteSetActive(*f.orbital));
}

// Stellar_IsStellarUsableForTravel (0x0046E440): target-active + not 0x3000.

TEST_CASE("stellar is usable for travel when active and not in the reserved "
          "lane",
          "[targeting]") {
  Fixture f;
  CHECK(game::NovaTargeting_IsStellarUsableForTravel(*f.orbital));

  // The landable colony is target-active and not restricted: usable.
  CHECK(game::NovaTargeting_IsStellarUsableForTravel(*f.landing));
  // The restricted travel lane (0x82, availability_flags & 0x3000) is not
  // usable even when otherwise target-active.
  const auto &gate = f.state.scenario.stellars[2];
  CHECK_FALSE(game::NovaTargeting_IsStellarUsableForTravel(gate));
}

// Stellar_ComputeTravelRangeSq (0x00465610): base 1000 + ModType 23 outfits.

TEST_CASE("travel range defaults to 1000 and squares", "[targeting]") {
  Fixture f;
  CHECK(game::NovaTargeting_ComputeTravelRangeSq(f.state) ==
        Catch::Approx(1000.0F * 1000.0F));
}

TEST_CASE("ModType 23 outfit widens the travel range by owned count",
          "[targeting]") {
  Fixture f;
  // Attach a hypothetical ModType 23 outfit with mod_val 50 to the player.
  Outfit o;
  o.name = "Distance Mod";
  o.mod_type = 0x17; // ModType 23
  o.mod_val = 50;
  f.state.scenario.outfits.push_back(o);       // index 0 == resource 0x80
  f.state.inventory.outfit_owned_count[0] = 2; // owns 2

  // radius = 1000 + 50*2 = 1100; squared = 1,210,000.
  CHECK(game::NovaTargeting_ComputeTravelRangeSq(f.state) ==
        Catch::Approx(1100.0F * 1100.0F));

  // An alternate-mod-slot ModType 23 also counts.
  f.state.scenario.outfits[0].mod_type = 0; // no longer primary 23
  f.state.scenario.outfits[0].alt_mod_types[1] = 0x17;
  f.state.scenario.outfits[0].alt_mod_vals[1] = 25;
  // radius = 1000 + 25*2 = 1050.
  CHECK(game::NovaTargeting_ComputeTravelRangeSq(f.state) ==
        Catch::Approx(1050.0F * 1050.0F));
}

// Stellar_IsStellarAdjacentToSystem (0x0040CD80).

TEST_CASE("stellar is adjacent when present in the system's nav list",
          "[targeting]") {
  Fixture f;
  CHECK(game::NovaTargeting_IsStellarAdjacentToSystem(*f.sys, 0x81));
  CHECK_FALSE(game::NovaTargeting_IsStellarAdjacentToSystem(*f.sys, 0x99));
}

TEST_CASE("system with no nav entries reports every stellar adjacent",
          "[targeting]") {
  Fixture f;
  f.sys->nav_defs = {
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
  // Preserved original quirk: empty nav list => degenerate "true".
  CHECK(game::NovaTargeting_IsStellarAdjacentToSystem(*f.sys, 0x80));
  CHECK(game::NovaTargeting_IsStellarAdjacentToSystem(*f.sys, 0x999));
}

// System_FindSystemContainingStellar (0x0046E790).

TEST_CASE("finds the system containing a stellar, preferring visible",
          "[targeting]") {
  Fixture f;
  CHECK(game::NovaTargeting_FindSystemContainingStellar(f.state.scenario,
                                                        0x80) == 0);
  CHECK(game::NovaTargeting_FindSystemContainingStellar(f.state.scenario,
                                                        0x123) == -1);

  // A hidden second system owns a stellar; the visible-first pass should ignore
  // it, but the fallback pass finds it.
  System hidden;
  hidden.is_visible = false;
  hidden.nav_defs = {
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
  hidden.nav_defs[0] = 0xAA;
  f.state.scenario.systems.push_back(hidden);
  CHECK(game::NovaTargeting_FindSystemContainingStellar(f.state.scenario,
                                                        0xAA) == 1);
}

// NovaTargeting_UpdateStellarAvailability (scope 3 of 0x00432470).

TEST_CASE("availability refresh homes stellars to the current system and marks "
          "playable ones available",
          "[targeting]") {
  Fixture f;
  // Orbital starts with system_id 0; the colony's system is unset (-1) but it
  // is in the current nav list, so it gets re-homed to system 0 and marked
  // available once the system is visible.
  f.landing->system_id = -1;

  game::NovaTargeting_UpdateStellarAvailability(f.state);
  CHECK(f.orbital->is_available);
  CHECK(f.landing->is_available);
  CHECK(f.landing->system_id == 0);

  // A stellar owned by a non-visible system is not available.
  Stellar faraway;
  faraway.system_id = 5; // index 5 (no such visible system in this fixture)
  faraway.name = "Far";
  f.state.scenario.stellars.push_back(faraway);
  game::NovaTargeting_UpdateStellarAvailability(f.state);
  CHECK_FALSE(f.state.scenario.stellars.back().is_available);
}

TEST_CASE("hazard flag is set when the availability flags carry the 0x20 bit",
          "[targeting]") {
  Fixture f;
  f.orbital->availability_flags = 0x20;
  f.orbital->system_id = 0;
  game::NovaTargeting_UpdateStellarAvailability(f.state);
  CHECK(f.orbital->is_available);
  CHECK(f.orbital->hazard_marker);
}

// Landing selection (clean-room, from the primitives).

TEST_CASE("finds the nearest landable stellar within travel range",
          "[targeting]") {
  Fixture f;
  f.state.player.pos_x = 200.0F; // on the colony
  f.state.player.pos_y = 0.0F;
  // Within the default 1000px radius, the nearest *landable* (non-reserved)
  // stellar is the colony (0x81). The orbital (0x80, not landable) is nearer
  // but does not carry the landable bit.
  CHECK(game::NovaTargeting_FindNearestLandableStellar(f.state) == 0x81);
}

TEST_CASE("no landable stellar within range yields -1", "[targeting]") {
  Fixture f;
  f.state.player.pos_x = 4000.0F; // far outside the 1000px no-jump radius
  f.state.player.pos_y = 0.0F;
  CHECK(game::NovaTargeting_FindNearestLandableStellar(f.state) == -1);
}

TEST_CASE("landability requires usability and the landable bit",
          "[targeting]") {
  Fixture f;
  // A reserved travel-lane star is not landable even if the bit is set.
  Stellar &gate = f.state.scenario.stellars[2];
  gate.flags |= 0x2U;
  CHECK_FALSE(game::NovaTargeting_IsLandableStellar(gate));

  // The plain orbital lacks the landable bit.
  CHECK_FALSE(game::NovaTargeting_IsLandableStellar(*f.orbital));
  // The colony has it.
  CHECK(game::NovaTargeting_IsLandableStellar(*f.landing));
}

// Per-frame targeting + landing interaction.

TEST_CASE("per-frame targeting selects the nearest usable stellar",
          "[targeting]") {
  Fixture f;
  f.state.player.pos_x = 90.0F; // closest to the orbital (100,0)
  f.state.player.pos_y = 0.0F;
  // Orbital (0x80) is usable and nearest; the colony (0x81) is landable but
  // farther. Both within range.
  game::NovaTargeting_UpdatePlayerTarget(f.state);
  CHECK(f.state.travel.selected_stellar_id == 0x80);

  // Standing on the colony makes it the nearest usable target.
  f.state.player.pos_x = 200.0F;
  game::NovaTargeting_UpdatePlayerTarget(f.state);
  CHECK(f.state.travel.selected_stellar_id == 0x81);
}

TEST_CASE("per-frame targeting clears when nothing is usable in range",
          "[targeting]") {
  Fixture f;
  f.state.player.pos_x = 5000.0F; // far outside the 1000px radius
  f.state.player.pos_y = 0.0F;
  // No stellar is within travel range even though the system has usable ones.
  game::NovaTargeting_UpdatePlayerTarget(f.state);
  CHECK(f.state.travel.selected_stellar_id == -1);
}

TEST_CASE("landing is available only for a landable selected target in range",
          "[targeting]") {
  Fixture f;
  // Target the colony and park on it.
  f.state.player.pos_x = 200.0F;
  f.state.player.pos_y = 0.0F;
  game::NovaTargeting_UpdatePlayerTarget(f.state);
  CHECK(game::NovaTargeting_IsLandingAvailable(f.state));

  // Far away, still targeting the colony: no landing available.
  f.state.player.pos_x = 4000.0F;
  game::NovaTargeting_UpdatePlayerTarget(f.state);
  CHECK_FALSE(game::NovaTargeting_IsLandingAvailable(f.state));

  // Target a non-landable stellar (orbital) in range: not landable.
  f.state.player.pos_x = 100.0F;
  game::NovaTargeting_UpdatePlayerTarget(f.state); // targets orbital (0x80)
  CHECK_FALSE(game::NovaTargeting_IsLandingAvailable(f.state));
}

TEST_CASE("landing repositions, refills and deducts service cost",
          "[targeting]") {
  Fixture f;
  f.state.player.pos_x = 200.0F;
  f.state.player.pos_y = 0.0F;
  f.state.player.shield_points = 5.0F; // depleted
  f.state.player.armor_points = 3.0F;
  f.state.player.credits = 500;
  // The colony charges a service fee on landing.
  f.landing->service_cost = 150;

  game::NovaTargeting_UpdatePlayerTarget(f.state);
  CHECK(game::NovaLanding_TryLand(f.state));
  CHECK(f.state.travel.landed_this_frame);
  // Repositioned onto the colony and refilled to max.
  CHECK(f.state.player.pos_x == Catch::Approx(200.0F));
  CHECK(f.state.player.shield_points == 30.0F);
  CHECK(f.state.player.armor_points == 30.0F);
  CHECK(f.state.player.speed == Catch::Approx(0.0F));
  // Service cost paid.
  CHECK(f.state.player.credits == 500 - 150);
}

TEST_CASE("landing clamps credits at zero for an expensive fee",
          "[targeting]") {
  Fixture f;
  f.state.player.pos_x = 200.0F;
  f.state.player.pos_y = 0.0F;
  f.state.player.credits = 50;
  f.landing->service_cost = 1000;
  game::NovaTargeting_UpdatePlayerTarget(f.state);
  CHECK(game::NovaLanding_TryLand(f.state));
  CHECK(f.state.player.credits == 0);
}

TEST_CASE("landing is refused without a landable target in range",
          "[targeting]") {
  Fixture f;
  f.state.player.pos_x = 4000.0F; // no target in range
  f.state.player.pos_y = 0.0F;
  game::NovaTargeting_UpdatePlayerTarget(f.state);
  CHECK_FALSE(game::NovaLanding_TryLand(f.state));
  CHECK_FALSE(f.state.travel.landed_this_frame);
}
