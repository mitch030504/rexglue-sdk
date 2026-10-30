/**
 * @file        src/ui/xess_runtime.cpp
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "ui/xess_runtime.h"

#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DECLARE(std::string, temporal_upscaler);

#if defined(REX_HAS_XESS) && REX_HAS_XESS

#include <array>
#include <mutex>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace rex::ui::xess {

const Functions* Get() {
  static std::once_flag once;
  static Functions functions;
  static bool ok = false;
  std::call_once(once, [] {
    HMODULE module =
        LoadLibraryExW(L"libxess.dll", nullptr,
                       LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) {
      REXLOG_WARN("XeSS: libxess.dll not found next to the executable; using FSR instead");
      return;
    }
    auto load = [&](auto& pointer, const char* name) {
      pointer = reinterpret_cast<std::remove_reference_t<decltype(pointer)>>(
          reinterpret_cast<void*>(GetProcAddress(module, name)));
      return pointer != nullptr;
    };
    ok = load(functions.GetVersion, "xessGetVersion");
    ok &= load(functions.DestroyContext, "xessDestroyContext");
    ok &= load(functions.GetOptimalInputResolution, "xessGetOptimalInputResolution");
    ok &= load(functions.SetLoggingCallback, "xessSetLoggingCallback");
#if REX_HAS_D3D12
    ok &= load(functions.D3D12CreateContext, "xessD3D12CreateContext");
    ok &= load(functions.D3D12Init, "xessD3D12Init");
    ok &= load(functions.D3D12Execute, "xessD3D12Execute");
#endif
#if REX_HAS_VULKAN
    ok &= load(functions.VKGetRequiredInstanceExtensions, "xessVKGetRequiredInstanceExtensions");
    ok &= load(functions.VKGetRequiredDeviceExtensions, "xessVKGetRequiredDeviceExtensions");
    ok &= load(functions.VKGetRequiredDeviceFeatures, "xessVKGetRequiredDeviceFeatures");
    ok &= load(functions.VKCreateContext, "xessVKCreateContext");
    ok &= load(functions.VKInit, "xessVKInit");
    ok &= load(functions.VKExecute, "xessVKExecute");
#endif
    if (!ok) {
      REXLOG_WARN("XeSS: libxess.dll is missing API entry points; using FSR instead");
      return;
    }
    xess_version_t version = {};
    if (functions.GetVersion(&version) == XESS_RESULT_SUCCESS) {
      REXLOG_INFO("XeSS: libxess {}.{}.{} loaded", version.major, version.minor, version.patch);
    }
  });
  return ok ? &functions : nullptr;
}

void AttachLogging(const Functions& functions, xess_context_handle_t context) {
  functions.SetLoggingCallback(context, XESS_LOGGING_LEVEL_WARNING,
                               [](const char* message, xess_logging_level_t level) {
                                 if (level >= XESS_LOGGING_LEVEL_ERROR) {
                                   REXLOG_ERROR("XeSS: {}", message);
                                 } else {
                                   REXLOG_WARN("XeSS: {}", message);
                                 }
                               });
}

bool ChooseQuality(const Functions& functions, xess_context_handle_t context, xess_2d_t input,
                   xess_2d_t output, xess_quality_settings_t& quality_out) {
  // Highest quality first: the first preset whose range holds the input is the closest to it.
  static constexpr std::array<xess_quality_settings_t, 7> kQualities = {
      XESS_QUALITY_SETTING_AA,
      XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS,
      XESS_QUALITY_SETTING_ULTRA_QUALITY,
      XESS_QUALITY_SETTING_QUALITY,
      XESS_QUALITY_SETTING_BALANCED,
      XESS_QUALITY_SETTING_PERFORMANCE,
      XESS_QUALITY_SETTING_ULTRA_PERFORMANCE,
  };
  for (xess_quality_settings_t quality : kQualities) {
    xess_2d_t optimal = {}, min = {}, max = {};
    if (functions.GetOptimalInputResolution(context, &output, quality, &optimal, &min, &max) !=
        XESS_RESULT_SUCCESS) {
      continue;
    }
    if (input.x >= min.x && input.y >= min.y && input.x <= max.x && input.y <= max.y) {
      quality_out = quality;
      return true;
    }
  }
  return false;
}

bool Selected() {
  return REXCVAR_GET(temporal_upscaler) == "xess" && Get() != nullptr;
}

}  // namespace rex::ui::xess

#else

namespace rex::ui::xess {

bool Selected() {
  return false;
}

}  // namespace rex::ui::xess

#endif
