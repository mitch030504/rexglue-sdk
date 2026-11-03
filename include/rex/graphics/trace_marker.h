#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace rex::graphics {

// Diagnostic metadata travels with PM4 commands, rather than racing their consumer.
struct TraceMarker {
  static constexpr uint32_t kMagic = 0x5245584D;  // REXM
  static constexpr uint32_t kVersion = 1;
  static constexpr uint32_t kWords = 6;
  static constexpr uint32_t kPacket = 0xC0001000 | ((kWords - 1) << 16);
  // kMotion carries a per-draw 64-bit motion key: `node` is the low word and
  // `geometry` the high word. It reuses the six-word payload, so the wire format,
  // kVersion and every previously written record stay valid.
  enum Kind : uint32_t { kBegin = 1, kEnd, kInstance, kPhase, kMotion };
  Kind kind;
  uint32_t frame;
  uint32_t node;
  uint32_t geometry;

  static std::optional<TraceMarker> Parse(std::span<const uint32_t> words) {
    if (words.size() != kWords || words[0] != kMagic || words[1] != kVersion || words[2] < kBegin ||
        words[2] > kMotion) {
      return std::nullopt;
    }
    return TraceMarker{Kind(words[2]), words[3], words[4], words[5]};
  }
};

}  // namespace rex::graphics
