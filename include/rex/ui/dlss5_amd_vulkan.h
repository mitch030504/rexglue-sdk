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

#include <rex/ui/dlss5_amd.h>
#include <rex/ui/vulkan/device.h>

namespace rex::ui::dlss5_amd {

// Construct the native NR runtime on the application's device and queue.
//
// Construction uploads the weights and builds pipelines, so it must run before
// frame recording and while the queue is otherwise idle. The width/height are
// the NR evaluation extent (1920x1080 for the amended plan); the engine scene
// is pre-upscaled to it. A failure leaves the status kInactive with the
// exception text and does not affect normal rendering.
void InitializeRuntime(const vulkan::VulkanDevice& device, uint32_t width, uint32_t height);

// Non-blocking diagnostics for the UI, updated after a recorded frame.
void ReportRecordedFrame(bool applied, float gpu_ms);

// Development seam gate: when a substitute image is loaded and
// dlss5_amd_mode != 0, the composition draw that samples the scene source
// (base page 0x11C6F) uses this 1920x1080 view instead of the guest texture.
// The substitute image is created UNORM; SubstituteView() returns the UNORM or
// the SRGB reinterpret of it, selected by the dlss5_amd_view cvar.
bool SubstituteEnabled();
bool SubstituteForFrame(uint64_t frame);
VkImageView SubstituteView();

}  // namespace rex::ui::dlss5_amd
