#include "audio/sdl/guest_audio_stretch.h"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

using rex::audio::sdl::GuestAudioStretch;
namespace {
constexpr unsigned kRate = 48000, kBlock = 256;
struct Result { unsigned missing = 0, crossings = 0, frames = 0; double stereo_error = 0; };
Result Simulate(unsigned channels, bool variable, double constant_speed = .5) {
  GuestAudioStretch stretch(channels);
  std::vector<float> input(kBlock * channels), output(kBlock * channels);
  double pending = 0; uint64_t source = 0; float previous = 0; Result result;
  for (unsigned block = 0; block < kRate * 12 / kBlock; ++block) {
    const double seconds = double(block * kBlock) / kRate;
    const double speed = variable ? (seconds < 3 ? 1.0 : seconds < 6 ? .5 : seconds < 9 ? .75 : 1.0) : constant_speed;
    pending += kBlock * speed;
    // Bursty frame delivery, like 3-4 buffers produced together at a guest swap.
    if (block % 6 == 0) while (pending >= kBlock) {
      for (unsigned i = 0; i < kBlock; ++i, ++source) {
        const float wave = .5f * std::sin(2 * std::numbers::pi * 440 * double(source) / kRate);
        for (unsigned c = 0; c < channels; ++c) input[i * channels + c] = wave * (c % 2 ? -.5f : 1.f);
      }
      stretch.Push(input);
      pending -= kBlock;
    }
    const auto written = stretch.Read(output);
    CHECK(stretch.BufferedFrames() <= GuestAudioStretch::kMaxBufferedFrames);
    if (seconds > 2) {
      result.missing += kBlock - written;
      for (unsigned i = 0; i < written; ++i) {
        const float value = output[i * channels];
        if (previous <= 0 && value > 0) ++result.crossings;
        previous = value;
        for (unsigned c = 1; c < channels; ++c)
          result.stereo_error = std::max(result.stereo_error, double(std::abs(output[i * channels + c] - value * (c % 2 ? -.5f : 1.f))));
      }
      result.frames += written;
    }
  }
  return result;
}
}
TEST_CASE("Guest-paced output stretches slow audio without changing pitch", "[audio-stretch]") {
  for (unsigned channels : {2u, 6u}) for (double speed : {.35, .5, .75, 1.0, 1.25}) {
    INFO("channels=" << channels << " speed=" << speed);
    const auto result = Simulate(channels, false, speed);
    CHECK(result.missing == 0);
    REQUIRE(result.frames > kRate * 9);
    CHECK(std::abs(double(result.crossings) * kRate / result.frames - 440) < 3);
    CHECK(result.stereo_error < 0.0001);
  }
}
TEST_CASE("Guest-paced output follows changing bursty delivery", "[audio-stretch]") {
  const auto result = Simulate(2, true);
  CHECK(result.missing < kRate / 100); // Less than 10ms over ten seconds, including speed transitions.
  REQUIRE(result.frames > kRate * 9);
  CHECK(std::abs(double(result.crossings) * kRate / result.frames - 440) < 3);
}
TEST_CASE("Guest-paced output bounds bursts and resets after starvation", "[audio-stretch]") {
  GuestAudioStretch stretch(2);
  std::vector<float> burst(kRate * 2, .25f);
  std::array<float, kBlock * 2> output;
  stretch.Push(burst);
  CHECK(stretch.BufferedFrames() <= GuestAudioStretch::kMaxBufferedFrames);
  for (unsigned i = 0; i < kRate * 2 / kBlock; ++i) stretch.Read(output);
  CHECK(stretch.BufferedFrames() == 0);
  CHECK(stretch.Read(output) == 0);
  CHECK(std::all_of(output.begin(), output.end(), [](float v) { return v == 0; }));
  for (unsigned i = 0; i < 60; ++i) {
    stretch.Push(std::span<const float>(burst.data(), kBlock * 2));
    stretch.Read(output);
  }
  CHECK(stretch.Read(output) == kBlock);
}
