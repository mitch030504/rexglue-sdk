/**
 * @file        src/ui/display_mode.cpp
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/display_mode.h>

#include <mutex>

namespace rex::ui::display_mode {

namespace {

// Set once at startup by the SDL window and read from cvar consumers on arbitrary threads,
// so guard it with a mutex rather than relying on a benign data race.
std::mutex g_mutex;
bool g_is_set = false;
int32_t g_width = 0;
int32_t g_height = 0;
double g_refresh_hz = 0.0;

}  // namespace

void SetNativeDisplayMode(int32_t width, int32_t height, double refresh_hz) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_width = width;
  g_height = height;
  g_refresh_hz = refresh_hz;
  g_is_set = true;
}

bool GetNativeDisplayMode(int32_t& width_out, int32_t& height_out, double& refresh_hz_out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_is_set) {
    return false;
  }
  width_out = g_width;
  height_out = g_height;
  refresh_hz_out = g_refresh_hz;
  return true;
}

}  // namespace rex::ui::display_mode
