/**
 * @file        rex/ui/overlay/settings_overlay.h
 *
 * @brief       ImGui settings overlay dialog for cvar editing with save-to-config.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

class SettingsDialog : public ImGuiDialog {
 public:
  // config_path: where "Save to config" writes (e.g. exe_dir / "app.toml")
  // restart_baseline: the values restart-only settings started with, so the dialog can tell when
  // a change needs a restart; on_restart relaunches the app. Both are optional.
  SettingsDialog(ImGuiDrawer* imgui_drawer, std::filesystem::path config_path,
                 const std::unordered_map<std::string, std::string>* restart_baseline = nullptr,
                 std::function<void()> on_restart = {});
  ~SettingsDialog();

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  std::filesystem::path config_path_;
  const std::unordered_map<std::string, std::string>* restart_baseline_;
  std::function<void()> on_restart_;
  char search_buf_[128] = {};
  std::string selected_category_;
  std::string capturing_bind_name_;
};

}  // namespace rex::ui
