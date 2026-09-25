#include "sdl_music.hpp"

#include "log.hpp"

#include <SDL3/SDL.h>

#define DR_MP3_IMPLEMENTATION
#include <dr_mp3.h>

#include <algorithm>

namespace {

// The callback must always feed the requested byte count, so pad with silence
// when the stream is stopped or a request is unexpectedly small.
void PushSilence(SDL_AudioStream *stream, int bytes) {
  static constexpr std::uint8_t kZero[1024] = {};
  while (bytes > 0) {
    const int chunk = std::min<int>(bytes, static_cast<int>(sizeof(kZero)));
    SDL_PutAudioStreamData(stream, kZero, chunk);
    bytes -= chunk;
  }
}

} // namespace

void SdlMusic::StreamDeleter::operator()(SDL_AudioStream *stream) const {
  // A stream from SDL_OpenAudioDeviceStream also owns and destroys its device.
  SDL_DestroyAudioStream(stream);
}

SdlMusic::~SdlMusic() {
  // Tear the stream/device down before releasing the audio subsystem.
  stream_.reset();
  if (initialized_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
  }
  initialized_ = false;
}

bool SdlMusic::Initialize() {
  if (initialized_) {
    return true;
  }
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    NovaLog::Error("music: SDL audio subsystem failed to initialize: {}",
                   SDL_GetError());
    return false;
  }
  initialized_ = true;
  NovaLog::Info("music: SDL audio ready");
  return true;
}

// @port 0x00534380 100%
// Ghidra 0x00534380 NovaMusic_SwitchBackgroundTrack: ported as Load/Play; see
// sdl_music.hpp for the dr_mp3 + SDL stream backend substitution.
bool SdlMusic::Load(const std::string &path) {
  if (!initialized_) {
    NovaLog::Error("music Load called before Initialize()");
    return false;
  }
  // Pause and drop the previous stream before touching pcm_; Stop() waits for
  // the audio callback to finish, so the buffer can be replaced safely.
  Stop();
  stream_.reset();
  pcm_.clear();
  source_spec_ = {};

  drmp3 decoder;
  if (!drmp3_init_file(&decoder, path.c_str(), nullptr)) {
    NovaLog::Error("failed to load music '{}': not a decodable MP3", path);
    return false;
  }
  const int channels = static_cast<int>(decoder.channels);
  const int sample_rate = static_cast<int>(decoder.sampleRate);
  if (channels <= 0 || sample_rate <= 0) {
    NovaLog::Error("music '{}': unsupported format ({} ch, {} Hz)",
                   path,
                   channels,
                   sample_rate);
    drmp3_uninit(&decoder);
    return false;
  }

  // Decode the whole track up front. The shipped menu loop is short, and
  // holding it in RAM keeps the audio callback free of file I/O.
  constexpr drmp3_uint64 kChunkFrames = 4096;
  const auto channel_count = static_cast<std::size_t>(channels);
  for (;;) {
    const std::size_t base = pcm_.size();
    pcm_.resize(base + kChunkFrames * channel_count);
    const drmp3_uint64 read =
        drmp3_read_pcm_frames_s16(&decoder, kChunkFrames, pcm_.data() + base);
    pcm_.resize(base + read * channel_count);
    if (read < kChunkFrames) {
      break;
    }
  }
  drmp3_uninit(&decoder);

  if (pcm_.empty()) {
    NovaLog::Error("music '{}': decoder produced no samples", path);
    return false;
  }

  source_spec_ = SDL_AudioSpec{SDL_AUDIO_S16, channels, sample_rate};
  stream_.reset(SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                          &source_spec_,
                                          &SdlMusic::AudioCallback,
                                          this));
  if (!stream_) {
    NovaLog::Error("music: could not open playback stream for '{}': {}",
                   path,
                   SDL_GetError());
    pcm_.clear();
    return false;
  }
  SDL_SetAudioStreamGain(stream_.get(), gain_);
  const auto frames = pcm_.size() / channel_count;
  NovaLog::Info("music loaded '{}' ({} Hz, {} ch, {:.1f}s)",
                path,
                sample_rate,
                channels,
                static_cast<double>(frames) / sample_rate);
  return true;
}

void SdlMusic::Play() {
  if (!initialized_ || !stream_ || playback_suppressed_ || pcm_.empty()) {
    return;
  }
  cursor_.store(0, std::memory_order_relaxed);
  playing_.store(true, std::memory_order_relaxed);
  SDL_ResumeAudioStreamDevice(stream_.get());
}

void SdlMusic::Stop() {
  if (!stream_) {
    return;
  }
  playing_.store(false, std::memory_order_relaxed);
  SDL_PauseAudioStreamDevice(stream_.get());
  cursor_.store(0, std::memory_order_relaxed);
}

void SdlMusic::SetVolume(float volume) {
  gain_ = std::clamp(volume, 0.0F, 1.0F);
  if (stream_) {
    SDL_SetAudioStreamGain(stream_.get(), gain_);
  }
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
  return initialized_ && playing_.load(std::memory_order_relaxed);
}

void SDLCALL SdlMusic::AudioCallback(void *userdata,
                                     SDL_AudioStream *stream,
                                     int additional_amount,
                                     int total_amount) {
  (void)total_amount;
  auto &self = *static_cast<SdlMusic *>(userdata);
  if (additional_amount <= 0) {
    return;
  }
  const int channels = self.source_spec_.channels;
  const int bytes_per_frame = channels * static_cast<int>(sizeof(std::int16_t));
  if (!self.playing_.load(std::memory_order_relaxed) || self.pcm_.empty() ||
      bytes_per_frame <= 0) {
    PushSilence(stream, additional_amount);
    return;
  }

  const auto total_samples = static_cast<std::size_t>(self.pcm_.size());
  const auto channel_count = static_cast<std::size_t>(channels);
  std::size_t cursor = self.cursor_.load(std::memory_order_relaxed);
  int remaining = additional_amount;
  while (remaining > 0) {
    const auto frames_available =
        (total_samples - std::min(cursor, total_samples)) / channel_count;
    const auto frames_wanted = static_cast<std::size_t>(remaining) /
                               static_cast<std::size_t>(bytes_per_frame);
    const auto frames = std::min(frames_available, frames_wanted);
    if (frames == 0) {
      // Request shorter than one frame (should not happen); avoid spinning.
      PushSilence(stream, remaining);
      break;
    }
    const int bytes =
        static_cast<int>(frames * channel_count * sizeof(std::int16_t));
    SDL_PutAudioStreamData(stream, self.pcm_.data() + cursor, bytes);
    cursor += static_cast<std::size_t>(bytes / sizeof(std::int16_t));
    remaining -= bytes;
    if (cursor >= total_samples) {
      cursor = 0;
    }
  }
  self.cursor_.store(cursor, std::memory_order_relaxed);
}
