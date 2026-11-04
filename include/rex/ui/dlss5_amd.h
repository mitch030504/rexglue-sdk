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

#include <cstdint>
#include <string>

namespace rex::ui::dlss5_amd {

// DLSS 5 neural rendering on AMD (DLSSNR-AMD) - status only.
//
// The feature is a separate, restart-required cvar. This API is safe to call
// from the app/UI and never exposes Vulkan or upstream types; the runtime
// construction is in dlss5_amd_vulkan.h.

enum class State {
  // Not requested.
  kOff,
  // Requested but unusable; `reason` names the first missing prerequisite.
  kInactive,
  // Assets and device are valid and the runtime was constructed; no frame has
  // been recorded yet (or recording is gated off in this build).
  kReady,
  // A frame has actually been recorded and applied.
  kActive,
};

struct Status {
  State state = State::kOff;
  bool compiled_in = false;
  bool requested = false;
  // Empty unless kInactive.
  std::string reason;
  // Resolved absolute folder that must contain dlssnr-amd/.
  std::string data_root;
  std::string model_path;
  std::string model_sha256;
  uint32_t model_entries = 0;
  std::string gpu;
  uint64_t recorded_frames = 0;
  float average_gpu_ms = 0.0f;
};

// True when the SDK was built with REXGLUE_ENABLE_DLSS5_AMD.
bool IsCompiledIn();

// True when the user requested DLSS 5 NR (the cvar is on and the build has it).
// This is the predicate used at Vulkan device creation to enable the required
// features; it does not validate the model or the shaders.
bool Selected();

const Status& GetStatus();

// Resolve the root, validate the model pack and the shader tree, and update the
// status. Called once at setup; no Vulkan objects are created.
void InitializeAssets();

// Record the first device-level reason the requested feature cannot run. Kept
// separate from the asset checks so a missing extension is reported even when
// the data path is also wrong (the device is created before the files are read).
void SetDeviceUnsupported(const std::string& reason);

// Drop any runtime state. Must be called only after the queue is idle.
void Shutdown();

}  // namespace rex::ui::dlss5_amd
