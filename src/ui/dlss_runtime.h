#pragma once
/**
 * @file        src/ui/dlss_runtime.h
 * @brief       Shared pieces of the presenters' NVIDIA DLSS (NGX) Super Resolution backends.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <string>

#if defined(REX_HAS_DLSS) && REX_HAS_DLSS
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>
#endif

namespace rex::ui::dlss {

/// Whether temporal_upscaler asks for DLSS (and this build has it).
bool Selected();

#if defined(REX_HAS_DLSS) && REX_HAS_DLSS

/// NGX project id for a custom engine (any fixed GUID).
inline constexpr const char* kProjectId = "6e1c0c6a-3f1a-4bd1-9a3c-7d2f3b5e9c41";

/// Where NGX writes its logs: the game's log folder.
const wchar_t* ApplicationDataPath();

/// The DLSS quality mode closest to an output/input scale ratio (1.0 is DLAA).
NVSDK_NGX_PerfQuality_Value QualityForRatio(float ratio);

/// Asks for the transformer model (preset K) in every quality mode.
void SetRenderPresets(NVSDK_NGX_Parameter* parameters);

/// Short text for an NGX result, for the log.
std::string ResultString(NVSDK_NGX_Result result);

#endif

}  // namespace rex::ui::dlss
