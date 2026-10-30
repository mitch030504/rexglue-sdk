/**
 * @file        src/ui/dlss_runtime.cpp
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "ui/dlss_runtime.h"

#include <fmt/format.h>

#include <rex/cvar.h>

REXCVAR_DECLARE(std::string, temporal_upscaler);

namespace rex::ui::dlss {

#if defined(REX_HAS_DLSS) && REX_HAS_DLSS

bool Selected() {
  return REXCVAR_GET(temporal_upscaler) == "dlss";
}

const wchar_t* ApplicationDataPath() {
  return L"logs";
}

NVSDK_NGX_PerfQuality_Value QualityForRatio(float ratio) {
  if (ratio <= 1.05f) {
    return NVSDK_NGX_PerfQuality_Value_DLAA;
  }
  if (ratio <= 1.6f) {
    return NVSDK_NGX_PerfQuality_Value_MaxQuality;  // 1.5x
  }
  if (ratio <= 1.85f) {
    return NVSDK_NGX_PerfQuality_Value_Balanced;  // 1.72x
  }
  if (ratio <= 2.5f) {
    return NVSDK_NGX_PerfQuality_Value_MaxPerf;  // 2x
  }
  return NVSDK_NGX_PerfQuality_Value_UltraPerformance;  // 3x
}

void SetRenderPresets(NVSDK_NGX_Parameter* parameters) {
  for (const char* name : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality}) {
    NVSDK_NGX_Parameter_SetUI(parameters, name, NVSDK_NGX_DLSS_Hint_Render_Preset_K);
  }
}

std::string ResultString(NVSDK_NGX_Result result) {
  return fmt::format("0x{:08X}", uint32_t(result));
}

#else

bool Selected() {
  return false;
}

#endif

}  // namespace rex::ui::dlss
