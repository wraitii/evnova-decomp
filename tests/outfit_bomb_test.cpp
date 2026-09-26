// Carried-bomb class latch and detonation-timer seed written by
// Outfit_RecomputeOutfitDerivedState (0x0046d4b0). Bible ModType 47 "bomb"
// maps to class 1 (lethal / escape-pod variant), ModType 50 "nonlethal bomb"
// to class 2, and the timer is rerolled with Random(100) whenever a bomb is
// owned (the class-1 latch wins if both are present).

#include "game/game_state.hpp"
#include "game/outfit.hpp"
#include "game/scenario_data.hpp"

#include <catch2/catch_test_macros.hpp>

namespace game {
namespace {

constexpr std::int16_t kBomb = static_cast<std::int16_t>(OutfitEffect::kBomb);
constexpr std::int16_t kNonlethalBomb =
    static_cast<std::int16_t>(OutfitEffect::kNonlethalBomb);

TEST_CASE("recompute arms a ModType 47 bomb as class 1", "[outfit][bomb]") {
  GameState state;
  state.scenario.outfits.resize(1);
  state.scenario.outfits[0].mod_type = kBomb;
  state.inventory.outfit_owned_count.fill(0);
  state.inventory.outfit_owned_count[0] = 1;

  NovaOutfit_RecomputeOutfitDerivedState(state);
  CHECK(state.bomb_outfit_class == 1);
  CHECK(state.bomb_detonation_timer >= 0.0F);
  CHECK(state.bomb_detonation_timer < 100.0F);
}

TEST_CASE("recompute arms a ModType 50 bomb as class 2", "[outfit][bomb]") {
  GameState state;
  state.scenario.outfits.resize(1);
  state.scenario.outfits[0].mod_type = kNonlethalBomb;
  state.inventory.outfit_owned_count.fill(0);
  state.inventory.outfit_owned_count[0] = 1;

  NovaOutfit_RecomputeOutfitDerivedState(state);
  CHECK(state.bomb_outfit_class == 2);
}

TEST_CASE("ModType 47 wins over ModType 50 regardless of scan order",
          "[outfit][bomb]") {
  for (const bool bomb_first : {true, false}) {
    GameState state;
    state.scenario.outfits.resize(2);
    state.scenario.outfits[bomb_first ? 0 : 1].mod_type = kBomb;
    state.scenario.outfits[bomb_first ? 1 : 0].mod_type = kNonlethalBomb;
    state.inventory.outfit_owned_count.fill(0);
    state.inventory.outfit_owned_count[0] = 1;
    state.inventory.outfit_owned_count[1] = 1;

    NovaOutfit_RecomputeOutfitDerivedState(state);
    CHECK(state.bomb_outfit_class == 1);
  }
}

TEST_CASE("recompute clears the bomb class when no bomb is owned",
          "[outfit][bomb]") {
  GameState state;
  state.scenario.outfits.resize(1);
  state.scenario.outfits[0].mod_type = kBomb;
  state.inventory.outfit_owned_count.fill(0);
  state.inventory.outfit_owned_count[0] = 1;
  NovaOutfit_RecomputeOutfitDerivedState(state);
  REQUIRE(state.bomb_outfit_class == 1);

  state.inventory.outfit_owned_count[0] = 0;
  NovaOutfit_RecomputeOutfitDerivedState(state);
  CHECK(state.bomb_outfit_class == 0);
}

} // namespace
} // namespace game
