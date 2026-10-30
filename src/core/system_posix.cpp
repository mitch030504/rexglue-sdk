/**
 * @file        core/system_posix.cpp
 * @brief       POSIX implementations of rex/system.h platform helpers.
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <rex/platform.h>
#include <rex/system.h>

#if REX_PLATFORM_LINUX
#include <spawn.h>
#include <unistd.h>

extern char** environ;
#endif

namespace rex {

// TODO(tomc): add linux support for showing a native message box
void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  const char* level = "INFO";
  switch (type) {
    case SimpleMessageBoxType::Help:
      level = "INFO";
      break;
    case SimpleMessageBoxType::Warning:
      level = "WARNING";
      break;
    case SimpleMessageBoxType::Error:
      level = "ERROR";
      break;
  }
  std::fprintf(stderr, "[%s] %.*s\n", level, static_cast<int>(message.size()), message.data());
  std::fflush(stderr);
}

bool LaunchCopyOfSelf() {
#if REX_PLATFORM_LINUX
  // /proc/self/cmdline holds the arguments NUL-separated, argv[0] included.
  std::ifstream cmdline_file("/proc/self/cmdline", std::ios::binary);
  std::string cmdline((std::istreambuf_iterator<char>(cmdline_file)),
                      std::istreambuf_iterator<char>());
  std::vector<std::string> args;
  for (size_t start = 0; start < cmdline.size();) {
    size_t end = cmdline.find('\0', start);
    if (end == std::string::npos) {
      end = cmdline.size();
    }
    args.emplace_back(cmdline, start, end - start);
    start = end + 1;
  }
  if (args.empty()) {
    return false;
  }
  std::vector<char*> argv;
  for (auto& arg : args) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);

  char exe[4096];
  ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (len <= 0) {
    return false;
  }
  exe[len] = '\0';
  pid_t pid;
  return posix_spawn(&pid, exe, nullptr, nullptr, argv.data(), environ) == 0;
#else
  return false;
#endif
}

}  // namespace rex
