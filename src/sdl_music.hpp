#pragma once

#include <SDL3/SDL_audio.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Background music playback.
//
// In the original (Ghidra 0x00534380 FUN_00534380) background "bass" playback
// streams a :Music:SongNN file through a codec into a ring buffer that the
// mixer drains; NovaAudio_Initialize(8,0) opens the "music" device during
// session bootstrap, before any splash frame. The port decodes the shipped
// menu track once with the vendored dr_mp3 decoder and loops the PCM through a
// dedicated SDL audio stream. It is intentionally a separate logical device
// from SdlAudio's one-shot SFX voices; SDL mixes both into the shared default
// output, and Load+Play swaps the decoded track the way the original swaps the
// current song index.
class SdlMusic {
public:
  SdlMusic() = default;
  ~SdlMusic();

  SdlMusic(const SdlMusic &) = delete;
  SdlMusic &operator=(const SdlMusic &) = delete;
  SdlMusic(SdlMusic &&) = delete;
  SdlMusic &operator=(SdlMusic &&) = delete;

  // Enables the SDL audio subsystem. Idempotent and safe to call after SdlAudio
  // has already enabled it: the subsystem is reference-counted. The playback
  // stream itself is opened by Load, once the decoded source format is known.
  [[nodiscard]] bool Initialize();

  // Decodes an MP3 from disk into memory and opens the looping playback stream.
  // Any prior track is stopped/replaced, mirroring the original's song swap.
  // Returns false if the file is missing or undecodable.
  [[nodiscard]] bool Load(const std::string &path);

  // Starts (looping forever) the loaded track. Restarts from the beginning if
  // already playing.
  void Play();
  void Stop();

  // 0.0..1.0 gain applied to the music stream.
  void SetVolume(float volume);
  void SetPlaybackSuppressed(bool suppressed);

  [[nodiscard]] bool IsPlaying() const;

private:
  static void SDLCALL AudioCallback(void *userdata,
                                    SDL_AudioStream *stream,
                                    int additional_amount,
                                    int total_amount);

  struct StreamDeleter {
    void operator()(SDL_AudioStream *stream) const;
  };

  // Decoded interleaved S16 PCM at source_spec_'s rate/channels. Replaced only
  // while the stream is paused (Load calls Stop first), so the audio callback
  // never observes a concurrent mutation.
  std::vector<std::int16_t> pcm_;
  SDL_AudioSpec source_spec_{};
  // Read/written from both the main thread and the audio callback thread; the
  // callback only ever moves cursor forward within the fixed pcm_ buffer, so
  // relaxed atomics are enough (a racing Play/Stop restart may drop one audio
  // buffer's worth of samples, which is inaudible for a music loop).
  std::atomic<std::size_t> cursor_{0};
  std::atomic<bool> playing_{false};
  std::unique_ptr<SDL_AudioStream, StreamDeleter> stream_;
  float gain_ = 1.0F;
  bool initialized_ = false;
  bool playback_suppressed_ = false;
  bool resume_after_suppression_ = false;
};
