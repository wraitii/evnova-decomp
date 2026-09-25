#include "sdl_audio.hpp"
#include "log.hpp"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_timer.h>

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

bool SdlAudio::VoiceActive(const Voice &voice) const {
  if (voice.logical_end_ms.has_value()) {
    return gameplay_clock_ && gameplay_clock_() < *voice.logical_end_ms;
  }
  SDL_AudioStream *const stream = voice.stream.get();
  if (stream == nullptr) {
    return false;
  }
  // A stream that has been flushed and drained reports no queued bytes and so
  // is available for reuse. Nonzero queued data means it is still playing.
  const auto queued = SDL_GetAudioStreamQueued(stream);
  return queued > 0;
}

std::optional<std::size_t>
NovaAudio_SelectVoiceInsertion(std::span<const NovaAudioVoicePriority> active,
                               NovaAudioVoicePriority incoming,
                               std::size_t capacity) {
  std::size_t index = 0;
  while (index < active.size() && (incoming.width < active[index].width ||
                                   incoming.level < active[index].level)) {
    ++index;
  }
  if (active.size() >= capacity && index == active.size()) {
    return std::nullopt;
  }
  return index;
}

// @port 0x0046aad0 95% audio
// Ghidra 0x0046aad0 NovaAudio_QueueCenteredSound; 0x004d64f0
// NovaAudio_FillVoiceSlotDescriptor; 0x004d6550 Audio_AllocateVoiceSlot.
void SdlAudio::Play(const NovaSoundData &sound,
                    float gain,
                    float playback_rate,
                    int sound_key,
                    int priority_width) {
  if ((!initialized_ && !playback_suppressed_) || sound.samples.empty() ||
      sound.sample_rate <= 0 || sound.channel_count <= 0) {
    return;
  }

  // Retire drained descriptors before applying the original ordered 16-slot
  // insertion policy (Audio_AllocateVoiceSlot 0x004d6550).
  constexpr std::size_t kMaxVoices = 16;
  voices_.erase(std::remove_if(
                    voices_.begin(),
                    voices_.end(),
                    [this](const Voice &voice) { return !VoiceActive(voice); }),
                voices_.end());
  const int incoming_width = std::max(1, priority_width);
  // QueueCenteredSound supplies levels on a 0..0x100 scale; the allocator
  // clamps each channel to 0x80 before comparing their sum. SDL's gain is
  // already normalized after that clamp, so recover one channel's rank here.
  const int incoming_level = std::clamp(
      static_cast<int>(std::lround(std::max(0.0F, gain) * master_gain_ * 128)),
      0,
      0x80);
  std::vector<NovaAudioVoicePriority> active_priorities;
  active_priorities.reserve(voices_.size());
  for (const Voice &voice : voices_) {
    active_priorities.push_back(voice.priority);
  }
  const auto insertion_index = NovaAudio_SelectVoiceInsertion(
      active_priorities, {incoming_width, incoming_level}, kMaxVoices);
  if (!insertion_index.has_value()) {
    return;
  }

  Voice incoming;
  if (voices_.size() == kMaxVoices) {
    incoming = std::move(voices_.back());
    if (incoming.stream) {
      SDL_ClearAudioStream(incoming.stream.get());
    }
    voices_.pop_back();
  } else if (!playback_suppressed_) {
    incoming.stream.reset(SDL_CreateAudioStream(nullptr, nullptr));
    if (!incoming.stream) {
      NovaLog::Error("SDL audio stream creation failed: {}", SDL_GetError());
      return;
    }
    if (!SDL_BindAudioStream(device_id_, incoming.stream.get())) {
      NovaLog::Error("SDL audio stream bind failed: {}", SDL_GetError());
      return;
    }
  }
  incoming.key = sound_key;
  incoming.priority = {incoming_width, incoming_level};
  incoming.source_gain = std::max(0.0F, gain);
  incoming.logical_end_ms.reset();
  auto voice_it =
      voices_.insert(voices_.begin() + *insertion_index, std::move(incoming));
  Voice *voice = &*voice_it;

  const int playback_sample_rate =
      static_cast<int>(std::lround(sound.sample_rate * playback_rate));
  if (playback_sample_rate <= 0) {
    return;
  }
  if (playback_suppressed_) {
    const auto frames =
        sound.samples.size() / static_cast<std::size_t>(sound.channel_count);
    const auto duration_ms = static_cast<std::uint64_t>(
        std::ceil(static_cast<double>(frames) * 1000.0 /
                  static_cast<double>(playback_sample_rate)));
    voice->logical_end_ms = gameplay_clock_() + duration_ms;
    return;
  }
  const SDL_AudioSpec source_spec{
      SDL_AUDIO_S16, sound.channel_count, playback_sample_rate};
  if (!SDL_SetAudioStreamFormat(voice->stream.get(), &source_spec, nullptr)) {
    NovaLog::Warn("SDL audio stream format failed: {}", SDL_GetError());
    return;
  }
  SDL_SetAudioStreamGain(voice->stream.get(),
                         voice->source_gain * master_gain_);
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

// Ghidra 0x004d6770 NovaAudio_CountActiveByHandle.
int SdlAudio::CountActiveByKey(int sound_key) const {
  if (sound_key < 0) {
    return 0;
  }
  int active = 0;
  for (const Voice &voice : voices_) {
    if (voice.key == sound_key && VoiceActive(voice)) {
      ++active;
    }
  }
  return active;
}

// Ghidra 0x004d67d0 NovaAudio_UnregisterCallbacks.
void SdlAudio::StopByKey(int sound_key) {
  if (sound_key < 0) {
    return;
  }
  for (Voice &voice : voices_) {
    if (voice.key == sound_key) {
      if (voice.stream) {
        SDL_ClearAudioStream(voice.stream.get());
      }
      voice.key = -1;
    }
  }
}

void SdlAudio::StopAll() {
  for (Voice &voice : voices_) {
    if (voice.stream) {
      SDL_ClearAudioStream(voice.stream.get());
    }
    voice.key = -1;
  }
  voices_.clear();
}

void SdlAudio::SetMasterVolume(float volume) {
  const float clamped = std::clamp(volume, 0.0F, 1.0F);
  master_gain_ = clamped;
  for (Voice &voice : voices_) {
    if (voice.stream) {
      SDL_SetAudioStreamGain(voice.stream.get(),
                             voice.source_gain * master_gain_);
    }
  }
}

void SdlAudio::SetPlaybackSuppressed(
    bool suppressed, std::function<std::uint64_t()> gameplay_clock) {
  if (playback_suppressed_ == suppressed) {
    return;
  }
  StopAll();
  playback_suppressed_ = suppressed;
  if (suppressed) {
    gameplay_clock_ = gameplay_clock
                          ? std::move(gameplay_clock)
                          : std::function<std::uint64_t()>{[] {
                              return static_cast<std::uint64_t>(SDL_GetTicks());
                            }};
  } else {
    gameplay_clock_ = {};
  }
}

bool SdlAudio::IsEnabled() const { return initialized_; }
