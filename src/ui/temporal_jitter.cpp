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
// Written by the guest projection hook on the CPU thread and read by the paint thread. Each value
// is a single float wide, so relaxed atomics are enough: a reader can only see a torn *set* (one
// field from one frame, another from the next), and every captured set is a valid camera anyway.
std::atomic<float> g_camera_near{0.0f};
std::atomic<float> g_camera_far{0.0f};
std::atomic<float> g_camera_fov_y{0.0f};

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

void SetCameraProjection(float near_plane, float far_plane, float vertical_fov_radians) {
  g_camera_near.store(near_plane, std::memory_order_relaxed);
  g_camera_far.store(far_plane, std::memory_order_relaxed);
  g_camera_fov_y.store(vertical_fov_radians, std::memory_order_relaxed);
}

bool GetCameraProjection(float& near_plane_out, float& far_plane_out,
                         float& vertical_fov_radians_out) {
  const float near_plane = g_camera_near.load(std::memory_order_relaxed);
  const float far_plane = g_camera_far.load(std::memory_order_relaxed);
  const float fov_y = g_camera_fov_y.load(std::memory_order_relaxed);
  // A valid perspective camera: positive near, a far plane past it, and a vertical FOV inside
  // (0, pi). Reject a partial capture rather than let a consumer build a nonsense depth mapping.
  if (!(near_plane > 0.0f) || !(far_plane > near_plane) || !(fov_y > 0.0f) ||
      !(fov_y < 3.14159265f)) {
    return false;
  }
  near_plane_out = near_plane;
  far_plane_out = far_plane;
  vertical_fov_radians_out = fov_y;
  return true;
}

void OnGuestSwap() { g_guest_swap_count.fetch_add(1, std::memory_order_relaxed); }

uint64_t GuestSwapCount() { return g_guest_swap_count.load(std::memory_order_relaxed); }

}  // namespace rex::ui::temporal
