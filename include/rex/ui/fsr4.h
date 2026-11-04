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

#include <string>

namespace rex::ui::fsr4 {

// FSR 4.1.2 on native Linux Vulkan through a user-built provider library
// (amd_fidelityfx_upscaler_vk.so, e.g. an fsr4-vk-style build the user makes
// from their own FidelityFX SDK 2.4.0 / FSR 4.1.2.4555 dx12 DLL). The SDK never
// builds, ships, or packages that .so; it is dlopen'd at runtime when the
// `fsr4` cvar is on, and any failure falls back to the linked FSR 3.1.4.
//
// This API is status-only, safe to call from the app/UI, and never exposes
// Vulkan or upstream types; the loader seam itself is in src/ui/ffx_runtime.cpp.

enum class State {
  // Not requested.
  kOff,
  // Requested but unusable; `reason` names the first missing prerequisite.
  kInactive,
  // The provider .so was dlopen'd and all five ffx entry points resolved.
  kReady,
  // Reserved: a frame has actually been dispatched through the provider (set by
  // the presenter once the SDK 2.4 ABI switch lands).
  kActive,
};

struct Status {
  State state = State::kOff;
  // Always true: the dlopen seam is compiled into every build (only the .so is
  // user-supplied).
  bool compiled_in = false;
  bool requested = false;
  // Empty unless kInactive.
  std::string reason;
  // Resolved fsr4_data_path, or the executable directory when empty.
  std::string data_root;
  // The .so that was loaded, when state is kReady.
  std::string library_path;
};

// Always true: the dlopen seam is part of every build.
bool IsCompiledIn();

// True when the user requested FSR 4 (the fsr4 cvar is on). This is the
// predicate used at Vulkan device creation to enable the FP8 /
// cooperative-matrix features; it does not validate the provider .so.
bool Selected();

const Status& GetStatus();

// Record the first device-level reason the requested feature cannot run (the
// device is created before the provider .so is loaded).
void SetDeviceUnsupported(const std::string& reason);

// Record that the provider .so could not be loaded; `reason` names the first
// failed step (not found in any search path, dlopen error, missing entry
// point).
void SetProviderUnavailable(const std::string& reason);

// Record a successfully loaded provider .so.
void SetProviderLoaded(const std::string& library_path);

}  // namespace rex::ui::fsr4
