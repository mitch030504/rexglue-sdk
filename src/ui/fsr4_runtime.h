#pragma once
/**
 * @file        src/ui/fsr4_runtime.h
 * @brief       Internal side of the FSR 4 provider seam (public API: rex/ui/fsr4.h).
 *
 * Loader-side helpers shared between fsr4_runtime.cpp (cvars + status) and
 * ffx_runtime.cpp (the dlopen seam for Api::kVulkan on non-Windows).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <filesystem>
#include <vector>

namespace rex::ui::fsr4 {

/// fsr4_data_path canonicalized, or the executable directory when empty.
std::filesystem::path DataRoot();

/// Folders searched for amd_fidelityfx_upscaler_vk.so, in order: the executable
/// directory (via /proc/self/exe), then fsr4_data_path when set. Computed once.
const std::vector<std::filesystem::path>& LibrarySearchPaths();

}  // namespace rex::ui::fsr4
