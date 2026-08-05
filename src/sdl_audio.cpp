#include "sdl_audio.hpp"
#include "log.hpp"

#include <SDL3/SDL_init.h>

void SdlAudio::StreamDeleter::operator()(SDL_AudioStream *stream) const {
  SDL_DestroyAudioStream(stream);
}

SdlAudio::~SdlAudio() {
  if (initialized_) {
    StopAll();
    SDL_CloseAudioDevice(device_id_);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
  }
}

bool SdlAudio::Initialize() {
  if (initialized_) {
    return true;
  }
  // The SDL_INIT_AUDIO subsystem must be enabled before an audio device can be
  // opened. SdlPlatform only initializes SDL_INIT_VIDEO, so do it here.
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    NovaLog::Error("SDL audio subsystem failed to initialize: {}",
                   SDL_GetError());
    return false;
  }
  const SDL_AudioSpec device_format{SDL_AUDIO_S16, 2, 44100};
  device_id_ =
      SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &device_format);
  if (device_id_ == 0) {
    NovaLog::Error("SDL audio device open failed: {}", SDL_GetError());
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return false;
  }
  initialized_ = true;
  NovaLog::Info("SDL audio output enabled ({})",
                SDL_GetAudioDeviceName(device_id_));
  return true;
}

bool SdlAudio::StreamActive(SDL_AudioStream *stream) {
  // A stream that has been flushed and drained reports no queued bytes and so
  // is available for reuse. Nonzero queued data means it is still playing.
  const auto queued = SDL_GetAudioStreamQueued(stream);
  return queued > 0;
}

void SdlAudio::Play(const NovaSoundData &sound, float gain) {
  if (!initialized_ || sound.samples.empty() || sound.sample_rate <= 0 ||
      sound.channel_count <= 0) {
    return;
  }

  // Grow the voice pool on demand; the menu references at most a few sound
  // effects at once.
  if (voices_.empty()) {
    voices_.reserve(8);
  }
  if (voices_.size() < 8) {
    voices_.push_back(std::unique_ptr<SDL_AudioStream, StreamDeleter>{
        SDL_CreateAudioStream(nullptr, nullptr)});
    if (!voices_.back()) {
      NovaLog::Error("SDL audio stream creation failed: {}", SDL_GetError());
      voices_.pop_back();
      return;
    }
    if (!SDL_BindAudioStream(device_id_, voices_.back().get())) {
      NovaLog::Error("SDL audio stream bind failed: {}", SDL_GetError());
    }
  }

  // Find a free voice (one that has drained its previous effect). If every
  // voice is still busy, fall back to the least-recently-used voice, replacing
  // the tail of an older effect -- the same way the original's small voice pool
  // reuses slots.
  auto *voice = voices_[next_voice_].get();
  for (std::size_t attempt = 0; attempt < voices_.size(); ++attempt) {
    const auto candidate =
        voices_[(next_voice_ + attempt) % voices_.size()].get();
    if (!StreamActive(candidate)) {
      voice = candidate;
      break;
    }
  }
  next_voice_ = (next_voice_ + 1) % voices_.size();

  const SDL_AudioSpec source_spec{
      SDL_AUDIO_S16, sound.channel_count, sound.sample_rate};
  if (!SDL_SetAudioStreamFormat(voice, &source_spec, nullptr)) {
    NovaLog::Warn("SDL audio stream format failed: {}", SDL_GetError());
    return;
  }
  SDL_SetAudioStreamGain(voice, gain);
  SDL_ClearAudioStream(voice);
  const auto data = std::span<const std::byte>{
      reinterpret_cast<const std::byte *>(sound.samples.data()),
      sound.samples.size() * sizeof(std::int16_t)};
  if (!SDL_PutAudioStreamData(
          voice, data.data(), static_cast<int>(data.size()))) {
    NovaLog::Warn("SDL audio stream read failed: {}", SDL_GetError());
    return;
  }
  SDL_FlushAudioStream(voice);
}

void SdlAudio::StopAll() {
  for (auto &voice : voices_) {
    SDL_ClearAudioStream(voice.get());
  }
  voices_.clear();
}

bool SdlAudio::IsEnabled() const { return initialized_; }
