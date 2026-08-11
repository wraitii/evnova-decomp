#include <catch2/catch_test_macros.hpp>

#include "brgr_archive.hpp"
#include "game/game_state.hpp"

// The hyperspace jump sounds. The original preloads the jump handles in
// FUN_004b0740 via LoadStringResourceCopyById(0x80/0x81/0x82):
//   snd 128 'Warp up'    -- the ~6 s rising 'hyperspace imminent' cue played
//                           while the ship holds at the jump point (its length
//                           gates the fire; snd 129 'Warp up.x2' is the faster
//                           engine variant).
//   snd 130 'Warp out'   -- the ~2.5 s boom played at the fire, synced with
//                           the screen flash (its length gates the in-tunnel
//                           coast).
// The spaceflight loop preloads/decodes them into GameState.warp_up_sound /
// warp_out_sound. Pin that the resources exist and decode so a missing/
// renamed asset surfaces in the test suite.
TEST_CASE("hyperspace jump sounds (snd 128/130) exist and decode") {
  game::GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  const auto warp_up =
      NovaResource_LoadNamed(kResourceTypeSnd, static_cast<std::uint16_t>(128));
  REQUIRE(warp_up.has_value());
  CHECK(warp_up->name.find("Warp up") != std::string::npos);
  const auto warp_up_decoded = NovaSound_Decode(warp_up->bytes);
  REQUIRE(warp_up_decoded.has_value());
  // A real multi-second rising cue, not a blip; the hold lasts its length.
  CHECK(warp_up_decoded->samples.size() > 100000);
  CHECK(warp_up_decoded->sample_rate > 0);

  const auto warp_out = NovaResource_LoadNamed(kResourceTypeSnd,
                                               static_cast<std::uint16_t>(130));
  REQUIRE(warp_out.has_value());
  CHECK(warp_out->name.find("Warp out") != std::string::npos);
  const auto warp_out_decoded = NovaSound_Decode(warp_out->bytes);
  REQUIRE(warp_out_decoded.has_value());
  CHECK(warp_out_decoded->samples.size() > 10000); // a real ~2.5 s boom
  CHECK(warp_out_decoded->sample_rate > 0);

  // The engine-variant warp-up is optional but present in the shipped set.
  const auto warp_up_x2 =
      NovaResource_LoadNamed(kResourceTypeSnd, static_cast<std::uint16_t>(129));
  REQUIRE(warp_up_x2.has_value());
  CHECK(warp_up_x2->name.find("Warp up.x2") != std::string::npos);
}
