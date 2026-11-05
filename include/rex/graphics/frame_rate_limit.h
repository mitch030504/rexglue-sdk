#pragma once
/**
 * @file        rex/graphics/frame_rate_limit.h
 * @brief       Effective frame-rate limit shared by the guest tick throttle and the vsync worker.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>

namespace rex::graphics {

// Effective frame rate limit:
//  - -1 = unlimited (returned as-is; no guest tick throttle),
//  - 0 = default (returned as-is; the guest tick throttle falls back to a safe 30 Hz),
//  - >0 = clamped to the native display refresh rate when it is known
//    (rex::ui::display_mode::GetNativeDisplayMode), logging a warning once per distinct clamp;
//    returned as-is when no native display mode is known.
//
// Recomputed on every call (the frame_rate_limit cvar is kHotReload); it is cheap.
int32_t GetEffectiveFrameRateLimit();

}  // namespace rex::graphics
