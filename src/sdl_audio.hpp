#pragma once

#include <SDL3/SDL_audio.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

// Decoded, device-agnostic PCM audio buffer used as the playback unit by the
// SDL output layer. Menus/effects decode game sound resources into this form
// before handing it to SdlAudio. Samples are signed 16-bit little-endian.
struct NovaSoundData {
  int sample_rate = 0;
  int channel_count = 0;
  std::vector<std::int16_t> samples;
};

struct NovaAudioVoicePriority {
  int width = 1;
  int level = 0;
};

// Pure decision step from Audio_AllocateVoiceSlot (0x004d6550), exposed for
// saturation tests. Returns the ordered insertion index, or nullopt when a
// full table rejects an incoming descriptor that outranks no active entry.
[[nodiscard]] std::optional<std::size_t>
NovaAudio_SelectVoiceInsertion(std::span<const NovaAudioVoicePriority> active,
                               NovaAudioVoicePriority incoming,
                               std::size_t capacity = 16);

// RAII wrapper around an SDL3 audio device. Owns one output device plus a small
// pool of streaming voices (SDL_AudioStream) so several one-shot effects can
// play and mix simultaneously, mirroring the original game's voice-slot pool.
class SdlAudio {
public:
  SdlAudio() = default;
  ~SdlAudio();

  SdlAudio(const SdlAudio &) = delete;
  SdlAudio &operator=(const SdlAudio &) = delete;
  SdlAudio(SdlAudio &&) = delete;
  SdlAudio &operator=(SdlAudio &&) = delete;

  // Opens the default output device. Safe to call once; idempotent.
  [[nodiscard]] bool Initialize();

  // Plays one effect through the original's ordered 16-voice policy. A full
  // pool admits the incoming descriptor only when it outranks an active entry;
  // insertion then drops the final, lower-ranked voice.
  // sound_key tags the voice for CountActiveByKey (the original counts active
  // instances of a sound handle before retriggering no-stack effects); pass
  // -1 for untagged one-shots.
  // playback_rate is an SDL speed multiplier: 2.0 plays twice as fast and
  // halves the voice lifetime. Original Nova descriptors store the reciprocal
  // duration scale, so callers convert that representation before this API.
  void Play(const NovaSoundData &sound,
            float gain = 1.0F,
            float playback_rate = 1.0F,
            int sound_key = -1,
            int priority_width = 1);
  // Number of voices still playing (draining) with the given key. Mirrors
  // NovaAudio_CountActiveByHandle for the no-stack fire-sound gate.
  [[nodiscard]] int CountActiveByKey(int sound_key) const;
  // Stops every active voice tagged with the given key (clears its queued
  // PCM). Mirrors NovaAudio_UnregisterCallbacks on a playing handle -- used
  // when the disabled-jump collapse cancels the 'Warp up' cue mid-play.
  void StopByKey(int sound_key);
  void StopAll();
  void SetMasterVolume(float volume);

  // Probe-only output suppression. Play still creates logical voices whose
  // lifetimes use gameplay_clock, preserving completion and no-stack gates.
  void
  SetPlaybackSuppressed(bool suppressed,
                        std::function<std::uint64_t()> gameplay_clock = {});

  [[nodiscard]] bool IsEnabled() const;

private:
  struct StreamDeleter {
    void operator()(SDL_AudioStream *stream) const;
  };

  struct Voice {
    std::unique_ptr<SDL_AudioStream, StreamDeleter> stream;
    int key = -1;
    // Nova's AudioVoiceSlot allocator keeps voices ordered by this descriptor
    // width and then its channel level. A full table admits a louder/wider
    // incoming cue by replacing the weakest voice rather than dropping every
    // later effect.
    NovaAudioVoicePriority priority;
    float source_gain = 1.0F;
    std::optional<std::uint64_t> logical_end_ms;
  };

  [[nodiscard]] bool VoiceActive(const Voice &voice) const;

  SDL_AudioDeviceID device_id_ = 0;
  std::vector<Voice> voices_;
  float master_gain_ = 1.0F;
  bool initialized_ = false;
  bool playback_suppressed_ = false;
  std::function<std::uint64_t()> gameplay_clock_;
};
