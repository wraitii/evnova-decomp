#include <catch2/catch_test_macros.hpp>

#include "brgr_archive.hpp"
#include "game/game_state.hpp"

// The hyperspace jump sound: snd resource 200 'Etheric Wake.sfil' (the only
// non-weapon sound in the gameplay snd 200.. slot range; the original queues
// the jump handle g_random_encounter_fleet_defs[0].availability_expression
// + 0x94 via NovaEffects_QueueCenteredResource at engage, gated by
// g_playerHyperspaceAudioLatch). The spaceflight loop preloads and plays it at
// the tunnel fire, synced with the screen flash. Pin that the resource exists
// and decodes so a missing/renamed asset surfaces in the test suite.
TEST_CASE("hyperspace jump sound (snd 200) exists and decodes") {
  game::GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  const auto resource =
      NovaResource_LoadNamed(kResourceTypeSnd, static_cast<std::uint16_t>(200));
  REQUIRE(resource.has_value());
  // 'Etheric Wake.sfil' -- the jump wake; a renamed archive surfaces here.
  CHECK(resource->name.find("Etheric") != std::string::npos);

  const auto decoded = NovaSound_Decode(resource->bytes);
  REQUIRE(decoded.has_value());
  CHECK(decoded->samples.size() > 1000); // a real ~0.5s sound, not a blip
  CHECK(decoded->sample_rate > 0);
}
