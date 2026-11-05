#pragma once
/**
 * @file        rex/ui/display_mode.h
 * @brief       Native display mode of the selected monitor, published by the window backend.
 *
 * The window backend enumerates displays (SDL_GetDisplays) and publishes the current display
 * mode of the monitor selected by the `monitor` cvar here, so that resolution presets
 * ("native"/"display") and frame-rate caps can be resolved against the real display instead
 * of a hardcoded preset list.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>

namespace rex::ui::display_mode {

// Publishes the selected monitor's current display mode. Called by the window backend once the
// display mode is known (window creation / monitor change).
void SetNativeDisplayMode(int32_t width, int32_t height, double refresh_hz);

// Returns the native display mode last published via SetNativeDisplayMode.
// Returns false if no native display mode has been set yet.
bool GetNativeDisplayMode(int32_t& width_out, int32_t& height_out, double& refresh_hz_out);

}  // namespace rex::ui::display_mode
