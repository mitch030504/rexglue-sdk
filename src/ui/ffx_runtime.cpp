/**
 * @file        src/ui/ffx_runtime.cpp
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "ui/ffx_runtime.h"

#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME

#include <mutex>

#include <rex/logging.h>

#if defined(REX_FIDELITYFX_PREBUILT) && REX_FIDELITYFX_PREBUILT
#include <rex/platform.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace rex::ui::ffx {

#if defined(REX_FIDELITYFX_PREBUILT) && REX_FIDELITYFX_PREBUILT

namespace {

struct Loaded {
  std::once_flag once;
  Functions functions;
  bool ok = false;
};

const Functions* Load(Loaded& loaded, const wchar_t* dll_name, const char* log_name) {
  std::call_once(loaded.once, [&] {
    // Next to the executable, where the build and the installers put it.
    HMODULE module = LoadLibraryExW(
        dll_name, nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) {
      REXLOG_WARN("FidelityFX: {} not found next to the executable; no temporal upscaling",
                  log_name);
      return;
    }
    Functions& f = loaded.functions;
    f.CreateContext = reinterpret_cast<PfnFfxCreateContext>(
        reinterpret_cast<void*>(GetProcAddress(module, "ffxCreateContext")));
    f.DestroyContext = reinterpret_cast<PfnFfxDestroyContext>(
        reinterpret_cast<void*>(GetProcAddress(module, "ffxDestroyContext")));
    f.Configure = reinterpret_cast<PfnFfxConfigure>(
        reinterpret_cast<void*>(GetProcAddress(module, "ffxConfigure")));
    f.Query =
        reinterpret_cast<PfnFfxQuery>(reinterpret_cast<void*>(GetProcAddress(module, "ffxQuery")));
    f.Dispatch = reinterpret_cast<PfnFfxDispatch>(
        reinterpret_cast<void*>(GetProcAddress(module, "ffxDispatch")));
    loaded.ok = f.CreateContext && f.DestroyContext && f.Configure && f.Query && f.Dispatch;
    if (!loaded.ok) {
      REXLOG_WARN("FidelityFX: {} is missing API entry points", log_name);
    }
  });
  return loaded.ok ? &loaded.functions : nullptr;
}

}  // namespace

const Functions* Get(Api api) {
  static Loaded d3d12, vulkan;
  if (api == Api::kD3D12) {
    return Load(d3d12, L"amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_loader_dx12.dll");
  }
  return Load(vulkan, L"amd_fidelityfx_loader_vk.dll", "amd_fidelityfx_loader_vk.dll");
}

const Functions* GetAny() {
  const Functions* functions = Get(Api::kD3D12);
  return functions ? functions : Get(Api::kVulkan);
}

#else

// The source build links one backend; its entry points accept only that backend's contexts, and
// fail cleanly for the other.
static const Functions kLinked = {ffxCreateContext, ffxDestroyContext, ffxConfigure, ffxQuery,
                                  ffxDispatch};

const Functions* Get(Api) {
  return &kLinked;
}

const Functions* GetAny() {
  return &kLinked;
}

#endif

}  // namespace rex::ui::ffx

#endif
