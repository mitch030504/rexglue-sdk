#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

namespace rex::ui {

// Conversion between the motion guide's diagnostic field and the neural
// renderer's motion input.
//
// The guide stores a forward delta in guest clip space: `current_ndc -
// previous_ndc`, with x right and y up (the guest NDC convention used by the
// scene-UV reconstruction). The DLSS NR EngineFrame wants backward motion in
// normalized screen UV, with v down, applied by the consumer as
// `prev_uv = uv + motion * motion_scale`.
//
// A forward guest-NDC delta G maps to a backward top-left UV delta as:
//   u = 0.5 + 0.5 * x_ndc, v = 0.5 - 0.5 * y_ndc
//   prev_uv - cur_uv = (-0.5 * Gx, +0.5 * Gy)
// The +0.5 offsets cancel in the difference, so the conversion is a pure scale
// and can be supplied either by transforming the field or by passing these
// values as the runtime's motion_scale. There is no resolution factor: guest
// NDC is already the full output rectangle, and the runtime normalizes by the
// motion texture extent itself.
constexpr float kMotionNdcToBackwardUvScaleX = -0.5f;
constexpr float kMotionNdcToBackwardUvScaleY = 0.5f;

constexpr void MotionNdcToBackwardUv(float ndc_x, float ndc_y, float& uv_x, float& uv_y) {
  uv_x = kMotionNdcToBackwardUvScaleX * ndc_x;
  uv_y = kMotionNdcToBackwardUvScaleY * ndc_y;
}

}  // namespace rex::ui
