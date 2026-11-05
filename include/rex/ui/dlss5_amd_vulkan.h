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

// Development seam gate: when dlss5_amd_mode != 0 and either a substitute image
// is loaded (modes 1/2) or live mode is ready (mode 3, see LiveEnabled()), the
// composition draw that samples the scene source (base page 0x11C6F) uses a
// 1920x1080 substitute view instead of the guest texture.
// The substitute image is created UNORM; SubstituteView() returns the UNORM or
// the SRGB reinterpret of it, selected by the dlss5_amd_view cvar.
// The composition samples base page 0x11C6F twice per draw - once unsigned
// (guest host view UNORM) and once signed (guest host view SNORM) - so
// SubstituteViewForFetch() picks the view per fetch signedness:
// dlss5_amd_view for unsigned fetches, dlss5_amd_signed_view (default SNORM,
// domain-identical to the guest signed view) for signed ones.
bool SubstituteEnabled();
bool SubstituteForFrame(uint64_t frame);
VkImageView SubstituteView();
VkImageView SubstituteViewForFetch(bool is_signed);
// The VkFormat of the view SubstituteViewForFetch() selects for this fetch
// (for the one-shot substitution log).
VkFormat SubstituteViewFormatForFetch(bool is_signed);

// Live per-frame NR (dlss5_amd_mode = 3): the runtime was constructed, the
// persistent target/motion/colour images (at the runtime's own extent) and the
// prep pipeline were created, and mode 3 is selected. The command processor
// calls this at the composition seam. Live mode needs the Phase 2 motion guide;
// without a valid field there is no motion input and no record.
bool LiveEnabled();
// The view substituted for the composition source fetch in live mode: the NR
// answer (a separate target when the runtime takes one, else the in-place
// colour), left in SHADER_READ_ONLY_OPTIMAL by the last record.
VkImageView LiveTargetView();

// One live frame's inputs, all owned by the command processor.
struct LiveFrameInput {
  // The guest composition source (base page 0x11C6F), 1000x600.
  VkImageView colour_view = VK_NULL_HANDLE;
  uint32_t colour_width = 0;
  uint32_t colour_height = 0;
  // The Phase 2 guide resolve output, colour_width x colour_height uvec2 words.
  VkBuffer guide_buffer = VK_NULL_HANDLE;
  // The guest's own scene depth resolve (1000x600 kD24S8, sampled as
  // R32_SFLOAT), or null on a frame without a usable depth resolve - NR then
  // runs without the silhouette guide, as before.
  VkImageView depth_view = VK_NULL_HANDLE;
  uint32_t depth_width = 0;
  uint32_t depth_height = 0;
  // The sampler the composition draw binds for this fetch: the pre-upscale must
  // resample exactly as the composition would, or the live answer's mapping
  // differs from the guest path.
  VkSampler sampler = VK_NULL_HANDLE;
  uint64_t frame = 0;
  // Suppress history for this frame (first frame, or a guide-frame jump).
  bool reset = false;
  // The prep descriptor-set ring slot whose previous submission has completed.
  uint32_t descriptor_slot = 0;
};

// Records one live NR frame into `cmd` (a one-off command buffer submitted on
// the graphics queue between two halves of the split submission): the
// motion/colour prep dispatch over the guide buffer and the guest colour view,
// then the runtime's record_engine (or, with dlss5_live_bypass, a copy of the
// pre-upscaled colour into the answer - the live identity control). Returns
// result.frame.applied; on any failure the caller falls back to the guest
// view (no substitution).
bool RecordLiveFrame(VkCommandBuffer cmd, const LiveFrameInput& input);

}  // namespace rex::ui::dlss5_amd
