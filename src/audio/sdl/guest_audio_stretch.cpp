#include "guest_audio_stretch.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace rex::audio::sdl {
namespace {
constexpr unsigned kSampleRate = 48000;
constexpr unsigned kTargetFrames = 4800; // 100ms absorbs guest frame bursts and tempo transitions.
}

GuestAudioStretch::GuestAudioStretch(uint32_t channels) : channels_(channels) {
  assert(channels == 2 || channels == 6);
  dsp_.setChannels(channels);
  dsp_.setSampleRate(kSampleRate);
  dsp_.setSetting(SETTING_SEQUENCE_MS, 40);
  dsp_.setSetting(SETTING_SEEKWINDOW_MS, 15);
  dsp_.setSetting(SETTING_OVERLAP_MS, 8);
}

size_t GuestAudioStretch::BufferedFrames() const {
  return dsp_.numSamples() + dsp_.numUnprocessedSamples();
}

void GuestAudioStretch::Reset() {
  dsp_.clear();
  dsp_.setTempo(1);
  input_since_read_ = silent_frames_ = 0;
  input_rate_ = 1;
  active_ = playing_ = false;
}

void GuestAudioStretch::Push(std::span<const float> input) {
  assert(input.size() % channels_ == 0);
  if (input.empty()) return;
  const size_t max_input = kMaxBufferedFrames / 4 * channels_;
  if (input.size() > max_input) input = input.last(max_input);
  input_since_read_ += input.size() / channels_;
  active_ = true;
  dsp_.putSamples(input.data(), static_cast<unsigned>(input.size() / channels_));
  if (BufferedFrames() > kMaxBufferedFrames) {
    const auto excess = BufferedFrames() - kMaxBufferedFrames;
    dsp_.receiveSamples(static_cast<unsigned>(std::min<size_t>(excess, dsp_.numSamples())));
    if (BufferedFrames() > kMaxBufferedFrames) Reset();
  }
}

size_t GuestAudioStretch::Read(std::span<float> output) {
  assert(output.size() % channels_ == 0);
  std::fill(output.begin(), output.end(), 0);
  const size_t frames = output.size() / channels_;
  if (!frames || !active_) return 0;
  if (input_since_read_) silent_frames_ = 0;
  else silent_frames_ += frames;
  if (silent_frames_ >= kSampleRate / 2) {
    Reset();
    return 0;
  }
  const double alpha = 1 - std::exp(-double(frames) / (kSampleRate * .1));
  input_rate_ += alpha * (double(input_since_read_) / frames - input_rate_);
  input_since_read_ = 0;
  // Feed-forward follows production speed; a small backlog correction prevents long-term drift.
  const double correction = std::clamp(double(dsp_.numSamples()) / kTargetFrames, .85, 1.15);
  dsp_.setTempo(std::clamp(input_rate_ * correction, .25, 2.0));
  if (!playing_) {
    if (dsp_.numSamples() < kTargetFrames) return 0;
    playing_ = true;
  }
  return dsp_.receiveSamples(output.data(), static_cast<unsigned>(frames));
}

}  // namespace rex::audio::sdl
