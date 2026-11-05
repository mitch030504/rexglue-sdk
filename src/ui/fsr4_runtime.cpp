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

#include "ui/fsr4_runtime.h"

#include <filesystem>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/fsr4.h>

REXCVAR_DEFINE_BOOL(fsr4, false, "GPU",
                    "Linux, AMD: FSR 4.1.2 temporal upscaling through a user-built "
                    "amd_fidelityfx_upscaler_vk.so (fsr4-vk-style provider built from the "
                    "user's own FidelityFX SDK 2.4.0 / FSR 4.1.2.4555 DLL). The .so is never "
                    "shipped or packaged; it is dlopen'd from the executable directory or "
                    "fsr4_data_path. Enables the FP8/cooperative-matrix device features.")
    // kRequiresRestart, not kInitOnly: the device features are created during presentation setup,
    // but the value must remain writable so the F4/F9 menus can save it for the next launch.
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(fsr4_data_path, "", "GPU",
                      "Folder that contains amd_fidelityfx_upscaler_vk.so. Empty: next to "
                      "the executable.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::ui::fsr4 {
namespace {

Status g_status;

// The device is created before the provider .so is loaded, so the
// requested/compiled_in/data_root fields are refreshed on every accessor; the
// state transitions (kInactive/kReady) are sticky. The cvars are restart-only:
// a change takes effect on the next launch.
void EnsureStatus() {
  g_status.compiled_in = IsCompiledIn();
  g_status.requested = REXCVAR_GET(fsr4);
  if (g_status.data_root.empty()) {
    g_status.data_root = DataRoot().string();
  }
  if (!g_status.requested && g_status.state != State::kInactive) {
    g_status.state = State::kOff;
  }
}

}  // namespace

bool IsCompiledIn() {
  // The dlopen seam is compiled into every build; only the provider .so is
  // user-supplied and never packaged.
  return true;
}

bool Selected() {
  EnsureStatus();
  return g_status.requested;
}

const Status& GetStatus() {
  EnsureStatus();
  return g_status;
}

void SetDeviceUnsupported(const std::string& reason) {
  EnsureStatus();
  if (g_status.requested && g_status.state != State::kInactive) {
    g_status.state = State::kInactive;
    g_status.reason = reason;
  }
}

void SetProviderUnavailable(const std::string& reason) {
  EnsureStatus();
  if (g_status.requested && g_status.state != State::kInactive) {
    g_status.state = State::kInactive;
    g_status.reason = reason;
  }
  REXLOG_WARN("FSR 4 inactive: {}", reason);
}

void SetProviderLoaded(const std::string& library_path) {
  EnsureStatus();
  if (!g_status.requested) {
    return;
  }
  g_status.state = State::kReady;
  g_status.reason.clear();
  g_status.library_path = library_path;
  REXLOG_INFO("FSR 4: provider {} loaded, all ffx entry points resolved", library_path);
}

std::filesystem::path DataRoot() {
  std::filesystem::path root = REXCVAR_GET(fsr4_data_path);
  if (root.empty()) {
    return rex::filesystem::GetExecutablePath().parent_path();
  }
  std::error_code ec;
  const std::filesystem::path absolute = std::filesystem::weakly_canonical(root, ec);
  return ec ? root : absolute;
}

const std::vector<std::filesystem::path>& LibrarySearchPaths() {
  static const std::vector<std::filesystem::path> paths = [] {
    std::vector<std::filesystem::path> result;
    // The executable directory first (where installs put it), then the
    // configured fsr4_data_path.
    result.push_back(rex::filesystem::GetExecutablePath().parent_path());
    const std::filesystem::path configured = DataRoot();
    if (!configured.empty() && configured != result.front()) {
      result.push_back(configured);
    }
    return result;
  }();
  return paths;
}

}  // namespace rex::ui::fsr4
