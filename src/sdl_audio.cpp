#include "sdl_audio.hpp"
#include "log.hpp"

#include <SDL3/SDL_init.h>

#include <algorithm>
#include <cmath>

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

void SdlAudio::Play(const NovaSoundData &sound,
                    float gain,
                    float playback_rate,
                    int sound_key) {
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
    Voice voice{std::unique_ptr<SDL_AudioStream, StreamDeleter>{
                    SDL_CreateAudioStream(nullptr, nullptr)},
                -1};
    if (!voice.stream) {
      NovaLog::Error("SDL audio stream creation failed: {}", SDL_GetError());
      return;
    }
    if (!SDL_BindAudioStream(device_id_, voice.stream.get())) {
      NovaLog::Error("SDL audio stream bind failed: {}", SDL_GetError());
    }
    voices_.push_back(std::move(voice));
  }

  // Find a free voice (one that has drained its previous effect). If every
  // voice is still busy, fall back to the least-recently-used voice, replacing
  // the tail of an older effect -- the same way the original's small voice pool
  // reuses slots.
  Voice *voice = &voices_[next_voice_];
  for (std::size_t attempt = 0; attempt < voices_.size(); ++attempt) {
    Voice &candidate = voices_[(next_voice_ + attempt) % voices_.size()];
    if (!StreamActive(candidate.stream.get())) {
      voice = &candidate;
      break;
    }
  }
  next_voice_ = (next_voice_ + 1) % voices_.size();
  voice->key = sound_key;

  const int playback_sample_rate =
      static_cast<int>(std::lround(sound.sample_rate * playback_rate));
  if (playback_sample_rate <= 0) {
    return;
  }
  const SDL_AudioSpec source_spec{
      SDL_AUDIO_S16, sound.channel_count, playback_sample_rate};
  if (!SDL_SetAudioStreamFormat(voice->stream.get(), &source_spec, nullptr)) {
    NovaLog::Warn("SDL audio stream format failed: {}", SDL_GetError());
    return;
  }
  SDL_SetAudioStreamGain(voice->stream.get(), gain * master_gain_);
  SDL_ClearAudioStream(voice->stream.get());
  const auto data = std::span<const std::byte>{
      reinterpret_cast<const std::byte *>(sound.samples.data()),
      sound.samples.size() * sizeof(std::int16_t)};
  if (!SDL_PutAudioStreamData(
          voice->stream.get(), data.data(), static_cast<int>(data.size()))) {
    NovaLog::Warn("SDL audio stream read failed: {}", SDL_GetError());
    return;
  }
  SDL_FlushAudioStream(voice->stream.get());
}

int SdlAudio::CountActiveByKey(int sound_key) const {
  if (sound_key < 0) {
    return 0;
  }
  int active = 0;
  for (const Voice &voice : voices_) {
    if (voice.key == sound_key && StreamActive(voice.stream.get())) {
      ++active;
    }
  }
  return active;
}

void SdlAudio::StopByKey(int sound_key) {
  if (sound_key < 0) {
    return;
  }
  for (Voice &voice : voices_) {
    if (voice.key == sound_key) {
      SDL_ClearAudioStream(voice.stream.get());
      voice.key = -1;
    }
  }
}

void SdlAudio::StopAll() {
  for (Voice &voice : voices_) {
    SDL_ClearAudioStream(voice.stream.get());
    voice.key = -1;
  }
  voices_.clear();
}

void SdlAudio::SetMasterVolume(float volume) {
  const float clamped = std::clamp(volume, 0.0F, 1.0F);
  master_gain_ = clamped;
  for (Voice &voice : voices_) {
    SDL_SetAudioStreamGain(voice.stream.get(), master_gain_);
  }
}

bool SdlAudio::IsEnabled() const { return initialized_; }
