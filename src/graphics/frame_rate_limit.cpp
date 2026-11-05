/**
 * @file        frame_rate_limit.cpp
 * @brief       Effective frame-rate limit resolution (see rex/graphics/frame_rate_limit.h).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/frame_rate_limit.h>

#include <atomic>
#include <cmath>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/display_mode.h>

REXCVAR_DECLARE(int32_t, frame_rate_limit);

namespace rex::graphics {

int32_t GetEffectiveFrameRateLimit() {
  const int32_t frame_rate_limit_value = REXCVAR_GET(frame_rate_limit);
  // -1 (unlimited) and 0 (default) pass through untouched.
  if (frame_rate_limit_value <= 0) {
    return frame_rate_limit_value;
  }
  // A positive cap above the display's own refresh rate can never be shown (vsync would clamp
  // it anyway), so clamp to the native display refresh rate when it is known.
  int32_t native_width = 0;
  int32_t native_height = 0;
  double native_refresh_hz = 0.0;
  if (!rex::ui::display_mode::GetNativeDisplayMode(native_width, native_height,
                                                   native_refresh_hz) ||
      native_refresh_hz <= 0.0) {
    return frame_rate_limit_value;
  }
  const int32_t refresh_cap = static_cast<int32_t>(std::lround(native_refresh_hz));
  if (frame_rate_limit_value <= refresh_cap) {
    return frame_rate_limit_value;
  }
  // Warn once per distinct clamped request (the cvar is hot-reloadable, so this can be called
  // repeatedly with the same over-limit value).
  static std::atomic<int32_t> last_warned_request{0};
  if (last_warned_request.exchange(frame_rate_limit_value) != frame_rate_limit_value) {
    REXLOG_WARN("frame_rate_limit={} exceeds the display refresh rate ({:.3f} Hz); clamping to {}",
                frame_rate_limit_value, native_refresh_hz, refresh_cap);
  }
  return refresh_cap;
}

}  // namespace rex::graphics
