#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <functional>
#include <memory>
#include <vector>

#include <rex/input/device_assignment.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/state_merge.h>
#include <rex/system/interfaces/input.h>

namespace rex::ui {
class Window;
}

namespace rex::input {

class InputSystem : public system::IInputSystem {
 public:
  explicit InputSystem(rex::ui::Window* window);
  ~InputSystem() override;

  rex::ui::Window* window() const { return window_; }

  X_STATUS Setup() override;
  void Shutdown() override;

  void AddDriver(std::unique_ptr<InputDriver> driver);
  void AttachWindow(rex::ui::Window* window);
  void SetActiveCallback(std::function<bool()> callback);

  /// Replaces any previous assignment. Call before the guest starts polling.
  void SetDeviceAssignment(std::unique_ptr<DeviceAssignment> assignment);

  /// Sees every guest state/keystroke read after the devices answer, and may rewrite it: for
  /// input record/replay and netplay, which must control exactly what the guest reads. Takes the
  /// devices' result and returns the one the guest gets. Set before the guest starts polling.
  using CapabilitiesFilter = std::function<X_RESULT(uint32_t, X_RESULT, X_INPUT_CAPABILITIES*)>;
  void SetCapabilitiesFilter(CapabilitiesFilter filter) { capabilities_filter_ = std::move(filter); }

  using StateFilter = std::function<X_RESULT(uint32_t user_index, X_RESULT result,
                                             X_INPUT_STATE* state)>;
  using KeystrokeFilter = std::function<X_RESULT(uint32_t user_index, X_RESULT result,
                                                 X_INPUT_KEYSTROKE* keystroke)>;
  void SetStateFilter(StateFilter filter) { state_filter_ = std::move(filter); }
  void SetKeystrokeFilter(KeystrokeFilter filter) { keystroke_filter_ = std::move(filter); }

  // Remaps vibration to a local slot; false drops remote rumble. While installed, the guest
  // always sees success so local hardware failures cannot affect a synchronized simulation.
  void SetVibrationFilter(std::function<bool(uint32_t&)> filter) {
    vibration_filter_ = std::move(filter);
  }

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags, X_INPUT_CAPABILITIES* out_caps);
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state);
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration);
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags, X_INPUT_KEYSTROKE* out_keystroke);

 private:
  /// Re-enumerates every driver and notifies the assignment when the set
  /// changed.
  void RefreshDevices();
  X_RESULT SetDevicesVibration(uint32_t user_index, X_INPUT_VIBRATION* vibration);
  X_RESULT GetDevicesCapabilities(uint32_t user_index, uint32_t flags, X_INPUT_CAPABILITIES* caps);
  X_RESULT GetDevicesState(uint32_t user_index, X_INPUT_STATE* out_state);
  X_RESULT GetDevicesKeystroke(uint32_t user_index, uint32_t flags,
                               X_INPUT_KEYSTROKE* out_keystroke);
  InputDriver* DriverForDevice(DeviceId id);
  const DeviceInfo* DeviceInfoFor(DeviceId id) const;

  rex::ui::Window* window_ = nullptr;

  std::vector<std::unique_ptr<InputDriver>> drivers_;

  std::unique_ptr<DeviceAssignment> assignment_;
  std::function<bool(uint32_t&)> vibration_filter_;
  CapabilitiesFilter capabilities_filter_;
  StateFilter state_filter_;
  KeystrokeFilter keystroke_filter_;
  ActiveDeviceTracker active_devices_;

  // Ordered by ordinal. Ordinals are never recycled, so unplugging pad one
  // does not renumber pad two.
  std::vector<DeviceInfo> devices_;
  std::vector<InputDriver*> device_owners_;
};

/// Create a default InputSystem with SDL + NOP drivers.
/// In tool mode, only the NOP driver is added.
std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode);

}  // namespace rex::input
