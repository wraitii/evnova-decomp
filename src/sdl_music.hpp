#pragma once

#include <SDL3_mixer/SDL_mixer.h>

#include <memory>
#include <string>

// RAII wrapper around SDL3_mixer's streaming music track.
//
// In the original (Ghidra 0x00534380 FUN_00534380) background "bass" playback
// streams a :Music:SongNN file through a codec into a ring buffer that the
// mixer drains; the audio hardware itself is opened far earlier, in
// NovaAudio_Initialize(8,0) during session bootstrap -- before any splash
// frame. That single pre-initialised "music" device tracks the current song
// index (DAT_00870db8). We approximate the same model with one looping
// SDL3_mixer track (which decodes MP3/Ogg/Wav directly), so re-assigning
// SdlMusic::Load+Play just swaps the decoded stream. It is intentionally a
// separate logical device from SdlAudio's one-shot SFX voices; SDL3 mixes both
// into the same physical default playback device.
class SdlMusic {
public:
  SdlMusic() = default;
  ~SdlMusic();

  SdlMusic(const SdlMusic &) = delete;
  SdlMusic &operator=(const SdlMusic &) = delete;
  SdlMusic(SdlMusic &&) = delete;
  SdlMusic &operator=(SdlMusic &&) = delete;

  // Initialises SDL_mixer and opens a mixer device on the default playback
  // output. Idempotent. Safe to call after SdlAudio has already opened its own
  // default device: SDL3 logical devices mix into the shared physical output.
  [[nodiscard]] bool Initialize();

  // Loads an audio file (MP3/Ogg/Wav) from disk for the music track. Any prior
  // track is stopped/replaced, mirroring the original's song swap. Returns
  // false if the file is missing or the decoder is unavailable.
  [[nodiscard]] bool Load(const std::string &path);

  // Starts (looping forever) the loaded track. Restarts from the beginning if
  // already playing.
  void Play();
  void Stop();

  // 0.0..1.0 gain applied to the music track.
  void SetVolume(float volume);

  [[nodiscard]] bool IsPlaying() const;

private:
  struct MixerDeleter {
    void operator()(MIX_Mixer *mixer) const;
  };

  struct AudioDeleter {
    void operator()(MIX_Audio *audio) const;
  };

  struct TrackDeleter {
    void operator()(MIX_Track *track) const;
  };

  std::unique_ptr<MIX_Mixer, MixerDeleter> mixer_;
  std::unique_ptr<MIX_Audio, AudioDeleter> audio_;
  std::unique_ptr<MIX_Track, TrackDeleter> track_;
  bool initialized_ = false;
};
