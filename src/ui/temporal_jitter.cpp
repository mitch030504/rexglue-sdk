/**
 * @file        src/ui/temporal_jitter.cpp
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/temporal_jitter.h>

#include <atomic>

namespace rex::ui::temporal {

namespace {

std::atomic<uint64_t> g_active_render_size{0};  // width << 32 | height, 0 when inactive
std::atomic<uint64_t> g_guest_swap_count{0};

float Halton(uint32_t index, uint32_t base) {
  float f = 1.0f, result = 0.0f;
  while (index > 0) {
    f /= float(base);
    result += f * float(index % base);
    index /= base;
  }
  return result;
}

}  // namespace

void JitterForFrame(uint64_t frame_index, float& x_out, float& y_out) {
  // Halton index 0 is (0, 0); start at 1 like the FidelityFX sample does.
  const uint32_t index = uint32_t(frame_index % kJitterPhaseCount) + 1;
  x_out = Halton(index, 2) - 0.5f;
  y_out = Halton(index, 3) - 0.5f;
}

void SetActive(uint32_t render_width, uint32_t render_height) {
  g_active_render_size.store(uint64_t(render_width) << 32 | render_height,
                             std::memory_order_relaxed);
}

void SetInactive() { g_active_render_size.store(0, std::memory_order_relaxed); }

bool IsActive(uint32_t& render_width_out, uint32_t& render_height_out) {
  const uint64_t size = g_active_render_size.load(std::memory_order_relaxed);
  render_width_out = uint32_t(size >> 32);
  render_height_out = uint32_t(size);
  return render_width_out && render_height_out;
}

void OnGuestSwap() { g_guest_swap_count.fetch_add(1, std::memory_order_relaxed); }

uint64_t GuestSwapCount() { return g_guest_swap_count.load(std::memory_order_relaxed); }

}  // namespace rex::ui::temporal
