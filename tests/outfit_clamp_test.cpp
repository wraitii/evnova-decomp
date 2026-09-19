// Ownership-limit tests for Outfit_ClampOwnedCountToLimits
// (Ghidra 0x004656a0 Outfit_ClampOutfitOwnedCountToCurrentLimits): the
// ordered arms are the ammo-back cap (primary ModType 3), the ModType-27 max
// multiplier, and the gun/turret slot caps, with OutfitOwnership::limited
// carrying the original bool return.

#include "game/compatibility.hpp"
#include "game/game_state.hpp"
#include "game/outfit.hpp"
#include "game/scenario_data.hpp"

#include <catch2/catch_test_macros.hpp>

namespace game {
namespace {

constexpr std::int16_t kAmmo = static_cast<std::int16_t>(OutfitEffect::kAmmo);
constexpr std::int16_t kIncreaseMax =
    static_cast<std::int16_t>(OutfitEffect::kIncreaseMax);
constexpr std::int16_t kModifyMaxGuns =
    static_cast<std::int16_t>(OutfitEffect::kModifyMaxGuns);
constexpr std::int16_t kModifyMaxTurrets =
    static_cast<std::int16_t>(OutfitEffect::kModifyMaxTurrets);

TEST_CASE("unlimited outfit reports the raw owned count", "[outfit][clamp]") {
  GameState state;
  state.scenario.outfits.resize(1);
  state.inventory.outfit_owned_count.fill(0);
  state.scenario.outfits[0].max_count = 10;
  state.inventory.outfit_owned_count[0] = 3;

  const OutfitOwnership r = Outfit_ClampOwnedCountToLimits(state, 0);
  CHECK(r.effective_owned == 3);
  CHECK(r.max_allowed == 10);
  CHECK_FALSE(r.limited);
}

TEST_CASE("out-of-range id clamps to zero and reports limited",
          "[outfit][clamp]") {
  GameState state;

  const OutfitOwnership below = Outfit_ClampOwnedCountToLimits(state, -1);
  CHECK(below.effective_owned == 0);
  CHECK(below.max_allowed == 0);
  CHECK(below.limited);

  const OutfitOwnership above = Outfit_ClampOwnedCountToLimits(state, 0x200);
  CHECK(above.effective_owned == 0);
  CHECK(above.max_allowed == 0);
  CHECK(above.limited);
}

TEST_CASE("ModType-27 multipliers lift the max and then cap ownership",
          "[outfit][clamp]") {
  GameState state;
  state.scenario.outfits.resize(2);
  state.inventory.outfit_owned_count.fill(0);
  state.scenario.outfits[0].max_count = 5;
  state.scenario.outfits[1].mod_type = kIncreaseMax;
  state.scenario.outfits[1].mod_val = 0x80;  // target resource id of index 0
  state.inventory.outfit_owned_count[1] = 2; // two multipliers

  state.inventory.outfit_owned_count[0] = 4;
  OutfitOwnership r = Outfit_ClampOwnedCountToLimits(state, 0);
  CHECK(r.max_allowed == 10); // 2 * 5
  CHECK(r.effective_owned == 4);
  CHECK_FALSE(r.limited);

  state.inventory.outfit_owned_count[0] = 11;
  r = Outfit_ClampOwnedCountToLimits(state, 0);
  CHECK(r.max_allowed == 10);
  CHECK(r.effective_owned == 10);
  CHECK(r.limited);
}

TEST_CASE("ammo-backed outfit caps at MaxAmmo times the live bank count",
          "[outfit][clamp]") {
  GameState state;
  state.scenario.outfits.resize(1);
  state.scenario.weapons.resize(1);
  state.inventory.outfit_owned_count.fill(0);
  state.scenario.outfits[0].max_count = 100;
  state.scenario.outfits[0].mod_type = kAmmo;
  state.scenario.outfits[0].mod_val = 0; // weapon bank 0
  state.scenario.weapons[0].max_ammo = 5;
  state.weapon_count_by_class[0] = 3; // three mounted launchers

  // Under the 15-round cap: no clamp, and MaxAmmo still lowers max_allowed.
  state.inventory.outfit_owned_count[0] = 10;
  OutfitOwnership r = Outfit_ClampOwnedCountToLimits(state, 0);
  CHECK(r.max_allowed == 15);
  CHECK(r.effective_owned == 10);
  CHECK_FALSE(r.limited);

  // At/over the cap: clamped to the cap and reported limited.
  state.inventory.outfit_owned_count[0] = 20;
  r = Outfit_ClampOwnedCountToLimits(state, 0);
  CHECK(r.max_allowed == 15);
  CHECK(r.effective_owned == 15);
  CHECK(r.limited);
}

TEST_CASE("gun and turret slot caps bound ownership", "[outfit][clamp]") {
  GameState state;
  state.scenario.outfits.resize(2);
  state.scenario.ships.resize(1);
  state.inventory.outfit_owned_count.fill(0);
  state.scenario.ships[0].max_gun = 1;
  state.scenario.ships[0].max_turret = 1;
  state.player.ship_class_id = 0;

  state.scenario.outfits[0].max_count = 5;
  state.scenario.outfits[0].flags = 0x0001; // gun
  state.scenario.outfits[1].max_count = 5;
  state.scenario.outfits[1].flags = 0x0002; // turret

  // One gun owned fills the single gun slot: reported limited at the cap.
  state.inventory.outfit_owned_count[0] = 1;
  OutfitOwnership r = Outfit_ClampOwnedCountToLimits(state, 0);
  CHECK(r.max_allowed == 1);
  CHECK(r.effective_owned == 1);
  CHECK(r.limited);

  // ModType-45 on another owned outfit lifts the gun cap back to 2, so the
  // lone gun is no longer at the cap.
  state.scenario.outfits[1].mod_type = kModifyMaxGuns;
  state.scenario.outfits[1].mod_val = 1;
  state.inventory.outfit_owned_count[1] = 1;
  r = Outfit_ClampOwnedCountToLimits(state, 0);
  CHECK(r.max_allowed == 2); // 1 + 1
  CHECK(r.effective_owned == 1);
  CHECK_FALSE(r.limited);

  // Turret target: one owned unit fills the single turret slot.
  state.scenario.outfits[0].mod_type = 0;
  state.scenario.outfits[0].flags = 0x0000;
  state.scenario.outfits[1].mod_type = 0;
  state.scenario.outfits[1].flags = 0x0002;
  state.inventory.outfit_owned_count[0] = 0;
  state.inventory.outfit_owned_count[1] = 1;
  r = Outfit_ClampOwnedCountToLimits(state, 1);
  CHECK(r.max_allowed == 1);
  CHECK(r.effective_owned == 1);
  CHECK(r.limited);

  // A second owned turret still caps the effective count at the one slot.
  state.inventory.outfit_owned_count[1] = 2;
  r = Outfit_ClampOwnedCountToLimits(state, 1);
  CHECK(r.max_allowed == 1);
  CHECK(r.effective_owned == 1);
  CHECK(r.limited);

  // ModType-46 lifts the turret cap, so both units fit again.
  state.scenario.outfits[0].mod_type = kModifyMaxTurrets;
  state.scenario.outfits[0].mod_val = 1;
  state.inventory.outfit_owned_count[0] = 1;
  state.inventory.outfit_owned_count[1] = 2;
  r = Outfit_ClampOwnedCountToLimits(state, 1);
  CHECK(r.max_allowed == 2); // 1 + 1
  CHECK(r.effective_owned == 2);
  CHECK(r.limited); // still limited by the lifted cap, matching the original
}

// BUGFIX(original): the executable added the first ModType-45/46 value once per
// owning outfit definition, so multiple copies of one mount modifier did not
// stack. kApplyOriginalBugFixes scales the bonus by the owned count instead.
TEST_CASE("max-gun/turret bonuses scale with owned copies", "[outfit][clamp]") {
  GameState state;
  state.scenario.outfits.resize(2);
  state.scenario.ships.resize(1);
  state.inventory.outfit_owned_count.fill(0);
  state.scenario.ships[0].max_gun = 1;
  state.scenario.ships[0].max_turret = 1;
  state.player.ship_class_id = 0;

  // [0] a mount modifier granting +2 guns and +2 turrets per copy.
  state.scenario.outfits[0].max_count = 100;
  state.scenario.outfits[0].mod_type = kModifyMaxGuns;
  state.scenario.outfits[0].mod_val = 2;
  state.scenario.outfits[0].alt_mod_types[0] = kModifyMaxTurrets;
  state.scenario.outfits[0].alt_mod_vals[0] = 2;
  // [1] the gun/turret being clamped (one owned copy fills the slot).
  state.scenario.outfits[1].max_count = 100;
  state.scenario.outfits[1].flags = 0x0001; // gun
  state.inventory.outfit_owned_count[1] = 1;

  const std::int16_t expected = kApplyOriginalBugFixes
                                    ? static_cast<std::int16_t>(1 + 2 * 3)
                                    : static_cast<std::int16_t>(1 + 2);

  state.inventory.outfit_owned_count[0] = 3; // three modifier copies
  OutfitOwnership r = Outfit_ClampOwnedCountToLimits(state, 1);
  CHECK(r.max_allowed == expected);

  state.scenario.outfits[1].flags = 0x0002; // turret
  r = Outfit_ClampOwnedCountToLimits(state, 1);
  CHECK(r.max_allowed == expected);
}

} // namespace
} // namespace game
