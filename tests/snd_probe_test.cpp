#include <catch2/catch_test_macros.hpp>

#include "brgr_archive.hpp"
#include "game/game_state.hpp"
#include "game/spaceflight.hpp"
#include "sdl_audio.hpp"

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

  const auto warp_out =
      NovaResource_LoadNamed(kResourceTypeSnd, static_cast<std::uint16_t>(130));
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

TEST_CASE("combat chatter resolves voice families and ship parity") {
  game::GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  SdlAudio audio;

  SECTION("invalid government falls back to voice zero") {
    game::NovaFrame_QueueCombatChatter(state, 0, -1, 0);
    game::NovaFrame_UpdateCombatChatter(state, audio);
    CHECK(state.pending_combat_chatter_kind == -1);
    CHECK(state.active_combat_chatter_sound_id >= 1000);
    CHECK(state.active_combat_chatter_sound_id <= 1006);
  }

  SECTION("even-sized voice family preserves the ship parity") {
    state.scenario.governments.resize(1);
    state.scenario.governments[0].voice_type_code = 4;
    game::NovaFrame_QueueCombatChatter(state, 2, 0, 1);
    game::NovaFrame_UpdateCombatChatter(state, audio);
    CHECK(state.pending_combat_chatter_kind == -1);
    CHECK(state.active_combat_chatter_sound_id >= 1420);
    CHECK(state.active_combat_chatter_sound_id <= 1427);
    CHECK((state.active_combat_chatter_sound_id - 1420) % 2 == 1);
  }
}

TEST_CASE("combat chatter cancellation resets the original sentinels") {
  game::GameState state;
  SdlAudio audio;
  CHECK(state.pending_combat_chatter_kind == -1);
  CHECK(state.pending_combat_chatter_government_id == -1);
  CHECK(state.pending_combat_chatter_variant == -1);

  game::NovaFrame_QueueCombatChatter(state, 2, 17, 1);
  game::NovaFrame_CancelCombatChatter(state, audio);
  CHECK(state.pending_combat_chatter_kind == -1);
  CHECK(state.pending_combat_chatter_government_id == -1);
  CHECK(state.pending_combat_chatter_variant == -1);
}
