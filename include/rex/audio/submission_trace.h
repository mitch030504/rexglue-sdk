#pragma once

// Dev-only submission trace for localizing audio duplication. Enabled by setting
// GIANTS_AUDIO_TRACE to an output path; normal runs are unaffected (one cached getenv check).
//
// CSV columns:
//   kind,time_us,seq,client,ptr,frames,queued,available,channels,hash,first,last
// kind=submit is one SDLAudioDriver::SubmitFrame (6-channel guest mix, 256 frames);
// kind=sdl is one SDL_PutAudioStreamData (2-channel device stream, pre-rate-conversion).
// A caller-side duplicate shows up as identical submit hashes; a queue-side duplicate shows
// up as identical sdl hashes without a matching submit duplicate.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace rex::audio::trace {

inline uint64_t HashFloats(const float* data, size_t count) {
  uint64_t hash = 1469598103934665603ull;  // FNV-1a
  const auto* bytes = reinterpret_cast<const uint8_t*>(data);
  for (size_t i = 0; i < count * sizeof(float); ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

struct State {
  std::mutex mutex;
  FILE* file = nullptr;
  uint64_t sequence = 0;
  bool initialized = false;
  bool enabled = false;
};

inline State& state() {
  static State value;
  return value;
}

inline bool Enabled() {
  State& s = state();
  if (!s.initialized) {
    const char* path = std::getenv("GIANTS_AUDIO_TRACE");
    s.enabled = path && *path;
    if (s.enabled) {
      s.file = std::fopen(path, "wb");
      if (!s.file) {
        s.enabled = false;
      } else {
        std::setvbuf(s.file, nullptr, _IOLBF, 0);
        std::fprintf(s.file, "kind,time_us,seq,client,ptr,frames,queued,available,channels,"
                             "hash,first,last\n");
      }
    }
    s.initialized = true;
  }
  return s.enabled;
}

inline void LogValue(const char* kind, uint32_t a, uint32_t b) {
  if (!Enabled()) {
    return;
  }
  State& s = state();
  const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
  std::lock_guard<std::mutex> guard(s.mutex);
  if (!s.file) {
    return;
  }
  std::fprintf(s.file, "%s,%lld,%llu,0,0,0,0,0,0,0,%u,%u\n", kind,
               static_cast<long long>(now), static_cast<unsigned long long>(s.sequence++), a, b);
}

inline void Log(const char* kind, uint32_t client, uint32_t ptr, uint32_t frames, int queued,
                int available, uint32_t channels, const float* data, size_t sample_count) {
  if (!Enabled()) {
    return;
  }
  State& s = state();
  const uint64_t hash = HashFloats(data, sample_count);
  const float first = sample_count ? data[0] : 0.0f;
  const float last = sample_count ? data[sample_count - 1] : 0.0f;
  const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
  std::lock_guard<std::mutex> guard(s.mutex);
  if (!s.file) {
    return;
  }
  std::fprintf(s.file, "%s,%lld,%llu,%u,%u,%u,%d,%d,%u,%016llx,%.8g,%.8g\n", kind,
               static_cast<long long>(now), static_cast<unsigned long long>(s.sequence++), client,
               ptr, frames, queued, available, channels,
               static_cast<unsigned long long>(hash), static_cast<double>(first),
               static_cast<double>(last));
}

}  // namespace rex::audio::trace
