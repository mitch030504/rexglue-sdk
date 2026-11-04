#pragma once
/**
 * @file        src/ui/ffx_runtime.h
 * @brief       The FidelityFX API entry points for the presenters' temporal upscaler.
 *
 * A source build of FidelityFX (FSR 3.1, Linux) links one backend's entry points directly. The
 * prebuilt AMD FidelityFX SDK 2.4 (Windows: FSR 4.1.2 with FSR 3.1 as fallback) ships one loader
 * DLL per graphics API, both exporting the same names, so a build with D3D12 and Vulkan loads the
 * right one at runtime instead.
 *
 * On non-Windows, Get(Api::kVulkan) first tries a user-supplied provider .so
 * (amd_fidelityfx_upscaler_vk.so, e.g. an fsr4-vk-style FSR 4.1.2 build) when the `fsr4` cvar is
 * on, and falls back to the linked entry points on any failure (status: rex::ui::fsr4).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME

#include <ffx_api/ffx_api.h>

namespace rex::ui::ffx {

struct Functions {
  PfnFfxCreateContext CreateContext = nullptr;
  PfnFfxDestroyContext DestroyContext = nullptr;
  PfnFfxConfigure Configure = nullptr;
  PfnFfxQuery Query = nullptr;
  PfnFfxDispatch Dispatch = nullptr;
};

enum class Api { kD3D12, kVulkan };

/// The entry points for a graphics API, or nullptr (logged once) if its runtime isn't available.
const Functions* Get(Api api);

/// Whichever runtime is available, for queries that don't depend on the graphics API.
const Functions* GetAny();

}  // namespace rex::ui::ffx

#endif
