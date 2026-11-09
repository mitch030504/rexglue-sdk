#pragma once
/**
 * @file        rex/ui/temporal_jitter.h
 * @brief       Sub-pixel jitter shared by a game's projection hook and the temporal upscaler.
 *
 * A temporal upscaler needs every frame rendered with a known sub-pixel offset. The recompiled
 * game builds its projection on the guest CPU, while the upscaler runs when the presenter shows
 * that frame, so both sides derive the offset from the guest frame index: the number of VdSwap
 * calls before the frame on the CPU side, and the number of swaps the GPU had processed before it
 * on the presenter side. Those agree because every VdSwap becomes exactly one swap packet.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>

namespace rex::ui::temporal {

// Halton(2,3) points; 32 covers the 8 * scale^2 phases AMD recommends up to 2x per axis.
constexpr uint32_t kJitterPhaseCount = 32;

/// Jitter for a guest frame, in render pixels, each axis in [-0.5, 0.5).
void JitterForFrame(uint64_t frame_index, float& x_out, float& y_out);

/// Set by the presenter while a temporal upscaler is producing frames from a render_width x
/// render_height guest output, cleared when it stops. A projection hook jitters only while active.
void SetActive(uint32_t render_width, uint32_t render_height);
void SetInactive();
bool IsActive(uint32_t& render_width_out, uint32_t& render_height_out);

/// The guest's latest scene perspective camera, captured by the game's projection hook for
/// upscalers that map device depth back to view space (FSR's cameraNear/cameraFar/
/// cameraFovAngleVertical). Values are the ones the guest used to build this frame's projection:
/// the near and far plane distances and the full vertical field of view, in radians. The hook
/// supplies them before any sub-pixel jitter, which shifts both frustum edges equally and leaves
/// the vertical span unchanged. GetCameraProjection returns false until a valid capture exists.
void SetCameraProjection(float near_plane, float far_plane, float vertical_fov_radians);
bool GetCameraProjection(float& near_plane_out, float& far_plane_out,
                         float& vertical_fov_radians_out);

/// Guest frame index on the CPU side: how many times the guest has called VdSwap.
void OnGuestSwap();
uint64_t GuestSwapCount();

}  // namespace rex::ui::temporal
