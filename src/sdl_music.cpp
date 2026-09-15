#include "sdl_music.hpp"

#include "log.hpp"

namespace {

// Request a 44.1 kHz stereo S16 device hint; SDL3_mixer converts any source
// format (MP3/Ogg/Wav) to whichever the hardware actually uses.
constexpr SDL_AudioSpec kMusicSpec{SDL_AUDIO_S16, 2, 44100};

} // namespace

void SdlMusic::MixerDeleter::operator()(MIX_Mixer *mixer) const {
  MIX_DestroyMixer(mixer);
}

void SdlMusic::AudioDeleter::operator()(MIX_Audio *audio) const {
  MIX_DestroyAudio(audio);
}

void SdlMusic::TrackDeleter::operator()(MIX_Track *track) const {
  MIX_DestroyTrack(track);
}

SdlMusic::~SdlMusic() {
  // MIX_DestroyMixer also closes this mixer's device and calls
  // SDL_QuitSubSystem(SDL_INIT_AUDIO); the members are torn down here,
  // before MIX_Quit(), and SdlAudio (a separate logical device) cleans up
  // its own device afterwards.
  track_.reset();
  audio_.reset();
  mixer_.reset();
  if (initialized_) {
    MIX_Quit();
  }
  initialized_ = false;
}

bool SdlMusic::Initialize() {
  if (initialized_) {
    return true;
  }
  if (!MIX_Init()) {
    NovaLog::Error("SDL_mixer init failed: {}", SDL_GetError());
    return false;
  }
  mixer_.reset(
      MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &kMusicSpec));
  if (!mixer_) {
    NovaLog::Error("SDL_mixer device open failed: {}", SDL_GetError());
    MIX_Quit();
    return false;
  }
  track_.reset(MIX_CreateTrack(mixer_.get()));
  if (!track_) {
    NovaLog::Error("SDL_mixer music track creation failed: {}", SDL_GetError());
    mixer_.reset();
    MIX_Quit();
    return false;
  }
  initialized_ = true;
  NovaLog::Info("SDL_mixer music ready (44.1 kHz stereo)");
  return true;
}

bool SdlMusic::Load(const std::string &path) {
  if (!initialized_ || !mixer_) {
    NovaLog::Error("music Load called before Initialize()");
    return false;
  }
  Stop();
  audio_.reset();
  // predecode=false keeps the track streaming from disk rather than holding the
  // decoded PCM in RAM, matching the original's streaming song buffer.
  auto *audio = MIX_LoadAudio(mixer_.get(), path.c_str(), false);
  if (!audio) {
    NovaLog::Error("failed to load music '{}': {}", path, SDL_GetError());
    return false;
  }
  audio_.reset(audio);
  if (!MIX_SetTrackAudio(track_.get(), audio_.get())) {
    NovaLog::Error(
        "SDL_mixer could not bind music '{}': {}", path, SDL_GetError());
    return false;
  }
  // Loop the whole track forever.
  if (!MIX_SetTrackLoops(track_.get(), -1)) {
    NovaLog::Warn("SDL_mixer could not enable music looping: {}",
                  SDL_GetError());
  }
  NovaLog::Info("music loaded '{}'", path);
  return true;
}

void SdlMusic::Play() {
  if (!initialized_ || !track_ || playback_suppressed_) {
    return;
  }
  if (MIX_TrackPlaying(track_.get())) {
    // Restart from the top on an explicit re-Play.
    if (MIX_TrackPaused(track_.get())) {
      MIX_ResumeTrack(track_.get());
    }
    MIX_SetTrackPlaybackPosition(track_.get(), 0);
    return;
  }
  MIX_PlayTrack(track_.get(), 0);
}

void SdlMusic::Stop() {
  if (!initialized_ || !track_) {
    return;
  }
  MIX_StopTrack(track_.get(), 0);
}

void SdlMusic::SetVolume(float volume) {
  if (!initialized_ || !track_) {
    return;
  }
  const auto clamped = volume < 0.0F ? 0.0F : (volume > 1.0F ? 1.0F : volume);
  MIX_SetTrackGain(track_.get(), clamped);
}

void SdlMusic::SetPlaybackSuppressed(bool suppressed) {
  if (playback_suppressed_ == suppressed) {
    return;
  }
  if (suppressed) {
    resume_after_suppression_ = IsPlaying();
    Stop();
    playback_suppressed_ = true;
    return;
  }
  playback_suppressed_ = false;
  if (resume_after_suppression_) {
    resume_after_suppression_ = false;
    Play();
  }
}

bool SdlMusic::IsPlaying() const {
  return initialized_ && track_ && MIX_TrackPlaying(track_.get());
}
