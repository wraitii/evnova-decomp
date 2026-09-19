#include <catch2/catch_test_macros.hpp>

#include "sdl_audio.hpp"

#include <array>
#include <cstddef>
#include <vector>

TEST_CASE("audio voice insertion preserves original descriptor ordering") {
  const std::array active{NovaAudioVoicePriority{10, 100},
                          NovaAudioVoicePriority{8, 80},
                          NovaAudioVoicePriority{6, 60}};

  CHECK(NovaAudio_SelectVoiceInsertion(active, {8, 80}) == 1);
  CHECK(NovaAudio_SelectVoiceInsertion(active, {7, 90}) == 2);
  CHECK(NovaAudio_SelectVoiceInsertion(active, {12, 40}) == 3);
}

TEST_CASE("full audio voice table rejects a descriptor that outranks none") {
  std::vector<NovaAudioVoicePriority> active(16,
                                             NovaAudioVoicePriority{10, 100});

  CHECK_FALSE(NovaAudio_SelectVoiceInsertion(active, {9, 100}).has_value());
  CHECK_FALSE(NovaAudio_SelectVoiceInsertion(active, {10, 99}).has_value());
}

TEST_CASE("full audio voice insertion selects the tail eviction boundary") {
  std::vector<NovaAudioVoicePriority> active;
  for (int rank = 16; rank >= 1; --rank) {
    active.push_back({rank, rank});
  }

  const auto insertion = NovaAudio_SelectVoiceInsertion(active, {8, 8});
  REQUIRE(insertion.has_value());
  CHECK(*insertion == 8);

  active.insert(active.begin() + static_cast<std::ptrdiff_t>(*insertion),
                {8, 8});
  active.pop_back();
  REQUIRE(active.size() == 16);
  CHECK(active[8].width == 8);
  CHECK(active.back().width == 2);
}

TEST_CASE("suppressed audio retains gameplay-clock voice completion") {
  SdlAudio audio;
  std::uint64_t now_ms = 100;
  audio.SetPlaybackSuppressed(true, [&now_ms] { return now_ms; });
  NovaSoundData sound{1000, 1, std::vector<std::int16_t>(2000)};

  audio.Play(sound, 1.0F, 1.0F, 128);
  CHECK(audio.CountActiveByKey(128) == 1);
  now_ms = 2099;
  CHECK(audio.CountActiveByKey(128) == 1);
  now_ms = 2100;
  CHECK(audio.CountActiveByKey(128) == 0);
}

TEST_CASE("playback rate is a speed multiplier for logical voice lifetime") {
  SdlAudio audio;
  std::uint64_t now_ms = 100;
  audio.SetPlaybackSuppressed(true, [&now_ms] { return now_ms; });
  NovaSoundData sound{1000, 1, std::vector<std::int16_t>(2000)};

  // The source is two seconds at 1x. A 2x playback rate halves that lifetime.
  audio.Play(sound, 1.0F, 2.0F, 128);
  now_ms = 1099;
  CHECK(audio.CountActiveByKey(128) == 1);
  now_ms = 1100;
  CHECK(audio.CountActiveByKey(128) == 0);

  // A rate below one extends the lifetime by the reciprocal amount.
  audio.Play(sound, 1.0F, 0.5F, 129);
  now_ms = 5099;
  CHECK(audio.CountActiveByKey(129) == 1);
  now_ms = 5100;
  CHECK(audio.CountActiveByKey(129) == 0);
}
