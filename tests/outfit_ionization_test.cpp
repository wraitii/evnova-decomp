// Ionization decay rate tests for Ship_ComputeIonizationDecayRate
// (Ghidra 0x0046c080). Covers the NPC class-base bypass, the player ModType 39
// (ion dissipator) accumulation across all four slots with owned-count
// filtering, the dedicated lazy player cache (valid while >= 0, so a negative
// or NaN result recomputes every call), and invalidation.

#include "game/game_state.hpp"
#include "game/outfit.hpp"
#include "game/scenario_data.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <limits>
#include <vector>

namespace game {
namespace {

constexpr std::int16_t kIon =
    static_cast<std::int16_t>(OutfitEffect::kIonDissipator);

// One loaded ship class at resource 0x80 (zero-based id 0) bound to the player.
void SetPlayerClassDecay(GameState &state, float rate) {
  state.scenario.ships.resize(1);
  state.scenario.ships[0].ionization_decay_rate = rate;
  state.player.ship_class_id = 0;
  state.player.ship_instance_id = 0;
  state.inventory.outfit_owned_count.fill(0);
  state.InvalidateDerivedStatCaches();
}

void AddOutfit(GameState &state,
               std::size_t id,
               std::int16_t type,
               std::int16_t val) {
  if (state.scenario.outfits.size() <= id) {
    state.scenario.outfits.resize(id + 1);
  }
  state.scenario.outfits[id].mod_type = type;
  state.scenario.outfits[id].mod_val = val;
}

} // namespace

TEST_CASE("ionization decay: NPC class base ignores owned ion dissipators",
          "[outfit][ionization]") {
  GameState state;
  SetPlayerClassDecay(state, 3.0F);
  AddOutfit(state, 0, kIon, 100);
  state.inventory.outfit_owned_count[0] = 4;

  Ship npc = state.player;
  npc.ship_instance_id = 7;
  CHECK(NovaOutfit_ComputeIonizationDecayRate(state, npc) == 3.0F);
  // The NPC path must not populate the player cache.
  CHECK(state.cached_ionization_decay_rate == -1.0F);
}

TEST_CASE("ionization decay: all four ModType-39 slots accumulate",
          "[outfit][ionization]") {
  GameState state;
  SetPlayerClassDecay(state, 1.0F);
  state.scenario.outfits.resize(1);
  state.scenario.outfits[0].mod_type = kIon;
  state.scenario.outfits[0].mod_val = 100;
  state.scenario.outfits[0].alt_mod_types = {kIon, kIon, kIon};
  state.scenario.outfits[0].alt_mod_vals = {50, 25, 10};
  state.inventory.outfit_owned_count[0] = 2;

  // 1.0 + (100*0.01)*2 + (50*0.01)*2 + (25*0.01)*2 + (10*0.01)*2 = 4.7
  CHECK(NovaOutfit_ComputeIonizationDecayRate(state, state.player) == 4.7F);
}

TEST_CASE("ionization decay: owned-count and mod-type filtering",
          "[outfit][ionization]") {
  GameState state;
  SetPlayerClassDecay(state, 1.0F);
  state.scenario.outfits.resize(3);
  // id 0: ModType 39 but not owned.
  state.scenario.outfits[0].mod_type = kIon;
  state.scenario.outfits[0].mod_val = 100;
  // id 1: ModType 39 with a negative owned count.
  state.scenario.outfits[1].mod_type = kIon;
  state.scenario.outfits[1].mod_val = 100;
  // id 2: owned but a different mod type.
  state.scenario.outfits[2].mod_type =
      static_cast<std::int16_t>(OutfitEffect::kShield);
  state.scenario.outfits[2].mod_val = 1000;
  state.inventory.outfit_owned_count[1] = -3;
  state.inventory.outfit_owned_count[2] = 5;

  CHECK(NovaOutfit_ComputeIonizationDecayRate(state, state.player) == 1.0F);
}

TEST_CASE("ionization decay: 0.01 is the double constant",
          "[outfit][ionization]") {
  GameState state;
  SetPlayerClassDecay(state, 0.0F);
  AddOutfit(state, 0, kIon, 1);
  state.inventory.outfit_owned_count[0] = 5;

  // The original computes (1 * 0.01_double) * 5 = 0.05 in double and casts to
  // the float 0.05F. The old float path (5 * 1 * 0.01F) produced one ULP less:
  // 0.04999999701976776F.
  CHECK(NovaOutfit_ComputeIonizationDecayRate(state, state.player) == 0.05F);
}

TEST_CASE("ionization decay: player lazily caches until invalidation",
          "[outfit][ionization]") {
  GameState state;
  SetPlayerClassDecay(state, 2.0F);
  AddOutfit(state, 0, kIon, 100);
  state.inventory.outfit_owned_count[0] = 1;

  CHECK(NovaOutfit_ComputeIonizationDecayRate(state, state.player) == 3.0F);
  REQUIRE(state.cached_ionization_decay_rate == 3.0F);

  // Inventory changes without invalidation: the cached rate is returned.
  state.inventory.outfit_owned_count[0] = 5;
  CHECK(NovaOutfit_ComputeIonizationDecayRate(state, state.player) == 3.0F);

  state.InvalidateDerivedStatCaches();
  CHECK(NovaOutfit_ComputeIonizationDecayRate(state, state.player) == 7.0F);
  CHECK(state.cached_ionization_decay_rate == 7.0F);
}

TEST_CASE("ionization decay: negative or NaN result is recomputed",
          "[outfit][ionization]") {
  GameState state;
  SetPlayerClassDecay(state, 0.0F);
  AddOutfit(state, 0, kIon, -100);
  state.inventory.outfit_owned_count[0] = 1;

  CHECK(NovaOutfit_ComputeIonizationDecayRate(state, state.player) == -1.0F);
  REQUIRE(state.cached_ionization_decay_rate == -1.0F);

  // A negative cache entry is not a valid sentinel: change the inventory and
  // the getter recomputes (a stale-cache read would have returned -1.0).
  state.inventory.outfit_owned_count[0] = 2;
  CHECK(NovaOutfit_ComputeIonizationDecayRate(state, state.player) == -2.0F);

  state.cached_ionization_decay_rate = std::numeric_limits<float>::quiet_NaN();
  state.inventory.outfit_owned_count[0] = 3;
  CHECK(NovaOutfit_ComputeIonizationDecayRate(state, state.player) == -3.0F);
}

TEST_CASE("ionization decay: Deionize loader conversion",
          "[outfit][ionization][scenario]") {
  // Ghidra 0x004bd3c0 ship section (0x004c18a0): raw <= 0 -> 1.0;
  // raw > 0 -> (float)(raw * 0.01_double). There is no positive minimum.
  const auto decode = [](std::int16_t deionize) {
    std::vector<std::byte> bytes(0x370, std::byte{0});
    bytes[0x36a] = std::byte{static_cast<std::uint8_t>((deionize >> 8) & 0xff)};
    bytes[0x36b] = std::byte{static_cast<std::uint8_t>(deionize & 0xff)};
    return DecodeShipPayload(bytes);
  };
  CHECK(decode(0).ionization_decay_rate == 1.0F);
  CHECK(decode(-5).ionization_decay_rate == 1.0F);
  CHECK(decode(1).ionization_decay_rate == 0.01F);
  CHECK(decode(50).ionization_decay_rate == 0.5F);
  CHECK(decode(100).ionization_decay_rate == 1.0F);
  CHECK(decode(12345).ionization_decay_rate == Catch::Approx(123.45F));
}

} // namespace game
