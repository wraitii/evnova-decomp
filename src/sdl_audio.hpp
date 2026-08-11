#pragma once

#include <SDL3/SDL_audio.h>

#include <cstdint>
#include <memory>
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

  // Plays a one-shot effect by streaming the provided PCM through a free voice.
  // If every voice is busy the oldest voice is reused (the new blip replaces
  // the tail of an earlier one, as the original's small voice pool does).
  void Play(const NovaSoundData &sound,
            float gain = 1.0F,
            float playback_rate = 1.0F);
  void StopAll();

  [[nodiscard]] bool IsEnabled() const;

private:
  struct StreamDeleter {
    void operator()(SDL_AudioStream *stream) const;
  };

  [[nodiscard]] static bool StreamActive(SDL_AudioStream *stream);

  SDL_AudioDeviceID device_id_ = 0;
  std::vector<std::unique_ptr<SDL_AudioStream, StreamDeleter>> voices_;
  std::size_t next_voice_ = 0;
  bool initialized_ = false;
};
