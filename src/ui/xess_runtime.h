#pragma once
/**
 * @file        src/ui/xess_runtime.h
 * @brief       Intel XeSS Super Resolution entry points, loaded from libxess.dll on demand.
 *
 * XeSS only ships as a Windows DLL. Loading it at runtime (rather than linking its import library)
 * keeps the game starting without it; the temporal upscaler then falls back to FSR.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#if defined(REX_HAS_XESS) && REX_HAS_XESS

#include <rex/platform.h>
#if REX_HAS_D3D12
#include <rex/ui/d3d12/d3d12_api.h>
#endif
#if REX_HAS_VULKAN
#include <rex/ui/vulkan/api.h>
#endif

#include <xess/xess.h>
#if REX_HAS_D3D12
#include <xess/xess_d3d12.h>
#endif
#if REX_HAS_VULKAN
#include <xess/xess_vk.h>
#endif

namespace rex::ui::xess {

struct Functions {
  decltype(&xessGetVersion) GetVersion = nullptr;
  decltype(&xessDestroyContext) DestroyContext = nullptr;
  decltype(&xessGetOptimalInputResolution) GetOptimalInputResolution = nullptr;
  decltype(&xessSetLoggingCallback) SetLoggingCallback = nullptr;
#if REX_HAS_D3D12
  decltype(&xessD3D12CreateContext) D3D12CreateContext = nullptr;
  decltype(&xessD3D12Init) D3D12Init = nullptr;
  decltype(&xessD3D12Execute) D3D12Execute = nullptr;
#endif
#if REX_HAS_VULKAN
  decltype(&xessVKGetRequiredInstanceExtensions) VKGetRequiredInstanceExtensions = nullptr;
  decltype(&xessVKGetRequiredDeviceExtensions) VKGetRequiredDeviceExtensions = nullptr;
  decltype(&xessVKGetRequiredDeviceFeatures) VKGetRequiredDeviceFeatures = nullptr;
  decltype(&xessVKCreateContext) VKCreateContext = nullptr;
  decltype(&xessVKInit) VKInit = nullptr;
  decltype(&xessVKExecute) VKExecute = nullptr;
#endif
};

/// The entry points, or nullptr (logged once) if libxess.dll can't be loaded.
const Functions* Get();

/// Routes the context's warnings and errors to the game's log.
void AttachLogging(const Functions& functions, xess_context_handle_t context);

/// The quality preset whose input resolution range for `output` contains `input`, preferring the
/// highest quality; false if none does (XeSS can't upscale by that ratio).
bool ChooseQuality(const Functions& functions, xess_context_handle_t context, xess_2d_t input,
                   xess_2d_t output, xess_quality_settings_t& quality_out);

}  // namespace rex::ui::xess

#endif

namespace rex::ui::xess {

/// Whether temporal_upscaler asks for XeSS (and this build has it).
bool Selected();

}  // namespace rex::ui::xess
