#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <SoundTouch.h>

namespace rex::audio::sdl {

// Used only by the output callback; its host timing never feeds guest audio scheduling.
class GuestAudioStretch {
 public:
  static constexpr size_t kMaxBufferedFrames = 24000;
  explicit GuestAudioStretch(uint32_t channels);
  void Push(std::span<const float> input);
  size_t Read(std::span<float> output);
  size_t BufferedFrames() const;

 private:
  void Reset();
  soundtouch::SoundTouch dsp_;
  uint32_t channels_;
  size_t input_since_read_ = 0;
  size_t silent_frames_ = 0;
  double input_rate_ = 1;
  bool active_ = false;
  bool playing_ = false;
};

}  // namespace rex::audio::sdl
