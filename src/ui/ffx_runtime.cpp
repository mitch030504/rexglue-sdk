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
#include <rex/platform.h>

#if defined(REX_FIDELITYFX_PREBUILT) && REX_FIDELITYFX_PREBUILT
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif !REX_PLATFORM_WIN32
// Non-Windows source build: when the user asks for FSR 4 (cvar fsr4), the Vulkan entry points are
// taken from a user-built provider .so instead of the linked FSR 3.1.4.
#include <dlfcn.h>

#include <filesystem>
#include <string>

#include <rex/ui/fsr4.h>

#include "ui/fsr4_runtime.h"
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

#elif !REX_PLATFORM_WIN32

// The source build links one backend (FSR 3.1.4); its entry points accept only that backend's
// contexts, and fail cleanly for the other.
static const Functions kLinked = {ffxCreateContext, ffxDestroyContext, ffxConfigure, ffxQuery,
                                  ffxDispatch};

namespace {

// Set by ForceLinkedBackend() when the provider loaded but cannot be used (context creation
// failed); Get() then serves the linked backend for the rest of the process.
std::atomic<bool> g_force_linked{false};
// Set once the user-supplied provider loaded and its entry points resolved, i.e. Get(kVulkan)
// serves it (until ForceLinkedBackend drops it).
std::atomic<bool> g_provider_loaded{false};

// ABI caveat: a 2.4.0-era provider .so (FSR 4.1.2) speaks the SDK 2.x ffxApi ABI, where
// ffxCreateBackendVKDesc carries the device in a VkDeviceContext struct (see the
// REX_FIDELITYFX_PREBUILT split in vulkan_presenter.cpp:323-330). This translation unit only
// resolves the five entry points; switching the presenter's create-desc to the 2.4.0 ABI is a
// later task (Track 4, T4.5). Until then the linked FSR 3.1.4 backend must keep serving dispatch,
// and the provider is used for queries only.
struct Loaded {
  std::once_flag once;
  Functions functions;
  bool ok = false;
};

const Functions* LoadVulkanProvider(Loaded& loaded) {
  std::call_once(loaded.once, [&] {
    if (!rex::ui::fsr4::Selected()) {
      return;
    }
    // Both names are in use: an fsr4-vk-style build produces
    // libamd_fidelityfx_upscaler_vk.so, while the Windows signed DLL and some
    // hand-built providers use amd_fidelityfx_upscaler_vk.so.
    constexpr const char* kLibraryNames[] = {
        "amd_fidelityfx_upscaler_vk.so",
        "libamd_fidelityfx_upscaler_vk.so",
    };
    void* module = nullptr;
    std::string loaded_from;
    std::string searched;
    std::string last_error;
    dlerror();  // Clear any stale error.
    // The executable directory first, then fsr4_data_path.
    for (const std::filesystem::path& folder : rex::ui::fsr4::LibrarySearchPaths()) {
      for (const char* library_name : kLibraryNames) {
        const std::filesystem::path candidate = folder / library_name;
        searched += candidate.string();
        searched += ", ";
        module = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (module) {
          loaded_from = candidate.string();
          break;
        }
        if (const char* error = dlerror()) {
          last_error = error;
        }
      }
      if (module) {
        break;
      }
    }
    if (!module) {
      rex::ui::fsr4::SetProviderUnavailable("cannot load amd_fidelityfx_upscaler_vk.so "
                                            "(searched " + searched + ")" +
                                            (last_error.empty() ? "" : ": " + last_error));
      return;
    }
    Functions& f = loaded.functions;
    f.CreateContext = reinterpret_cast<PfnFfxCreateContext>(dlsym(module, "ffxCreateContext"));
    f.DestroyContext = reinterpret_cast<PfnFfxDestroyContext>(dlsym(module, "ffxDestroyContext"));
    f.Configure = reinterpret_cast<PfnFfxConfigure>(dlsym(module, "ffxConfigure"));
    f.Query = reinterpret_cast<PfnFfxQuery>(dlsym(module, "ffxQuery"));
    f.Dispatch = reinterpret_cast<PfnFfxDispatch>(dlsym(module, "ffxDispatch"));
    loaded.ok = f.CreateContext && f.DestroyContext && f.Configure && f.Query && f.Dispatch;
    if (!loaded.ok) {
      rex::ui::fsr4::SetProviderUnavailable("the FSR 4 provider is missing ffx API entry points");
      dlclose(module);
      return;
    }
    // The module stays loaded for the process lifetime: the provider owns Vulkan objects, so
    // dlclose at exit would not be safe.
    g_provider_loaded.store(true, std::memory_order_relaxed);
    rex::ui::fsr4::SetProviderLoaded(loaded_from);
    REXLOG_INFO("FidelityFX: using {} for Vulkan", loaded_from);
  });
  return loaded.ok ? &loaded.functions : nullptr;
}

}  // namespace

const Functions* Get(Api api) {
  static Loaded vulkan_provider;
  if (api == Api::kVulkan && !g_force_linked.load(std::memory_order_relaxed)) {
    // The FSR 4 provider .so when fsr4 is on and loadable; any failure is recorded in
    // rex::ui::fsr4::GetStatus() and falls back to the linked build below.
    if (const Functions* functions = LoadVulkanProvider(vulkan_provider)) {
      return functions;
    }
  }
  return &kLinked;
}

void ForceLinkedBackend(const char* reason) {
  if (g_force_linked.exchange(true)) {
    return;
  }
  rex::ui::fsr4::SetProviderUnavailable(reason != nullptr ? reason : "the FSR 4 provider is unusable");
  REXLOG_WARN("FidelityFX: FSR 4 provider dropped ({}); using the linked FSR 3.1 backend",
              reason != nullptr ? reason : "unusable");
}

bool IsProviderActive() {
  return g_provider_loaded.load(std::memory_order_relaxed) &&
         !g_force_linked.load(std::memory_order_relaxed);
}

const Functions* GetAny() {
  return Get(Api::kVulkan);
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
