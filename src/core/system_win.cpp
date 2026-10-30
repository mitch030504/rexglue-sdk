/**
 * @file        core/system_win.cpp
 * @brief       Windows implementations of rex/system.h platform helpers.
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <iterator>
#include <string>

#include <rex/string.h>
#include <rex/system.h>

namespace rex {

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  UINT flags = MB_OK | MB_TOPMOST | MB_SETFOREGROUND;
  const wchar_t* title = L"ReXGlue";
  switch (type) {
    case SimpleMessageBoxType::Help:
      flags |= MB_ICONINFORMATION;
      break;
    case SimpleMessageBoxType::Warning:
      flags |= MB_ICONWARNING;
      break;
    case SimpleMessageBoxType::Error:
      flags |= MB_ICONERROR;
      break;
  }
  auto wide = rex::string::to_utf16(message);
  ::MessageBoxW(nullptr, reinterpret_cast<const wchar_t*>(wide.c_str()), title, flags);
}

bool LaunchCopyOfSelf() {
  wchar_t exe[MAX_PATH * 4];
  DWORD len = ::GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
  if (len == 0 || len >= std::size(exe)) {
    return false;
  }
  // CreateProcessW may modify the command-line buffer, so pass a copy.
  std::wstring command_line = ::GetCommandLineW();
  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  PROCESS_INFORMATION process_info = {};
  if (!::CreateProcessW(exe, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &startup_info, &process_info)) {
    return false;
  }
  ::CloseHandle(process_info.hThread);
  ::CloseHandle(process_info.hProcess);
  return true;
}

}  // namespace rex
