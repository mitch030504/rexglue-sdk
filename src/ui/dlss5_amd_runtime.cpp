/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/dlss5_amd.h>
#include <rex/ui/dlss5_amd_vulkan.h>

#if defined(REX_HAS_DLSS5_AMD) && REX_HAS_DLSS5_AMD
#include "nr_runtime.hpp"
#endif

REXCVAR_DEFINE_BOOL(dlss5_amd, false, "GPU",
                    "Linux, AMD: native DLSS 5 neural rendering (DLSSNR-AMD) before HUD. "
                    "Separate from the temporal upscaler and from the NVIDIA DLSS 5 layer. "
                    "Needs the exported small model and shaders under dlss5_amd_data_path.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(dlss5_amd_data_path, "", "GPU",
                      "Folder that contains dlssnr-amd/ (the model and shaders). Empty: next to "
                      "the executable. Point at the parent, not at dlssnr-amd itself.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::ui::dlss5_amd {
namespace {

constexpr uint64_t kSmallModelBytes = 113917248ull;
constexpr uint32_t kSmallModelEntries = 255;
constexpr const char* kSmallModelSha256 =
    "7e966e2639dbec6596aa0bac3b8761137d2c40ca8cb4aca30df7ca10043c6e33";

Status g_status;
std::string g_device_reason;
#if defined(REX_HAS_DLSS5_AMD) && REX_HAS_DLSS5_AMD
std::unique_ptr<nr::Runtime> g_runtime;
#endif
bool g_assets_initialized = false;

// --- SHA-256 (FIPS 180-4), for the model pin. Small and self-contained so the
// SDK does not gain a crypto dependency just to validate one user-supplied file.

constexpr uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

uint32_t Rotr(uint32_t x, uint32_t n) {
  return (x >> n) | (x << (32 - n));
}

struct Sha256 {
  uint32_t state[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                       0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  uint64_t length = 0;
  uint8_t block[64] = {};
  size_t used = 0;
};

void Sha256Transform(Sha256& ctx, const uint8_t* data) {
  uint32_t w[64];
  for (uint32_t i = 0; i < 16; ++i) {
    w[i] = (uint32_t(data[i * 4]) << 24) | (uint32_t(data[i * 4 + 1]) << 16) |
           (uint32_t(data[i * 4 + 2]) << 8) | uint32_t(data[i * 4 + 3]);
  }
  for (uint32_t i = 16; i < 64; ++i) {
    const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = ctx.state[0], b = ctx.state[1], c = ctx.state[2], d = ctx.state[3];
  uint32_t e = ctx.state[4], f = ctx.state[5], g = ctx.state[6], h = ctx.state[7];
  for (uint32_t i = 0; i < 64; ++i) {
    const uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t temp1 = h + s1 + ch + kSha256K[i] + w[i];
    const uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  ctx.state[0] += a;
  ctx.state[1] += b;
  ctx.state[2] += c;
  ctx.state[3] += d;
  ctx.state[4] += e;
  ctx.state[5] += f;
  ctx.state[6] += g;
  ctx.state[7] += h;
}

void Sha256Update(Sha256& ctx, const uint8_t* data, size_t size) {
  ctx.length += size;
  while (size != 0) {
    const size_t take = std::min(size, sizeof(ctx.block) - ctx.used);
    std::memcpy(ctx.block + ctx.used, data, take);
    ctx.used += take;
    data += take;
    size -= take;
    if (ctx.used == sizeof(ctx.block)) {
      Sha256Transform(ctx, ctx.block);
      ctx.used = 0;
    }
  }
}

std::string Sha256Final(Sha256& ctx) {
  const uint64_t bits = ctx.length * 8;
  const uint8_t pad = 0x80;
  Sha256Update(ctx, &pad, 1);
  const uint8_t zero = 0;
  while (ctx.used != 56) {
    Sha256Update(ctx, &zero, 1);
  }
  uint8_t length[8];
  for (uint32_t i = 0; i < 8; ++i) {
    length[i] = uint8_t(bits >> (56 - i * 8));
  }
  Sha256Update(ctx, length, 8);
  static const char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(64);
  for (uint32_t word : ctx.state) {
    for (int shift = 28; shift >= 0; shift -= 4) {
      result.push_back(kHex[(word >> shift) & 0xF]);
    }
  }
  return result;
}

bool Sha256File(const std::filesystem::path& path, std::string& hex, std::string& reason) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    reason = "cannot open " + path.string();
    return false;
  }
  Sha256 ctx;
  std::array<char, 1 << 16> buffer;
  while (file) {
    file.read(buffer.data(), std::streamsize(buffer.size()));
    const std::streamsize got = file.gcount();
    if (got > 0) {
      Sha256Update(ctx, reinterpret_cast<const uint8_t*>(buffer.data()), size_t(got));
    }
  }
  hex = Sha256Final(ctx);
  return true;
}

bool ValidateModelPack(const std::filesystem::path& path, std::string& reason,
                       std::string& sha256) {
  std::error_code ec;
  const uint64_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    reason = "cannot stat " + path.string();
    return false;
  }
  if (size != kSmallModelBytes) {
    reason = "dlssnr-small.bin is " + std::to_string(size) + " bytes, expected " +
             std::to_string(kSmallModelBytes);
    return false;
  }
  std::ifstream file(path, std::ios::binary);
  char magic[8] = {};
  uint32_t count = 0, reserved = 0;
  file.read(magic, 8);
  file.read(reinterpret_cast<char*>(&count), 4);
  file.read(reinterpret_cast<char*>(&reserved), 4);
  if (!file || std::memcmp(magic, "NRMODEL1", 8) != 0 || count != kSmallModelEntries ||
      reserved != 0) {
    reason = "dlssnr-small.bin is not a valid NRMODEL1 pack (255 entries expected)";
    return false;
  }
  uint64_t previous_end = 0;
  uint64_t max_end = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t name_length = 0;
    file.read(reinterpret_cast<char*>(&name_length), 4);
    if (!file || name_length == 0 || name_length > 4096) {
      reason = "dlssnr-small.bin has an invalid name length";
      return false;
    }
    std::string name(name_length, '\0');
    file.read(name.data(), std::streamsize(name_length));
    uint64_t offset = 0, entry_size = 0;
    file.read(reinterpret_cast<char*>(&offset), 8);
    file.read(reinterpret_cast<char*>(&entry_size), 8);
    if (!file || offset % 256 != 0 || entry_size == 0 || offset < previous_end ||
        offset + entry_size > size) {
      reason = "dlssnr-small.bin has an invalid entry (offset/size)";
      return false;
    }
    previous_end = offset + entry_size;
    max_end = std::max(max_end, previous_end);
  }
  if (max_end != size) {
    reason = "dlssnr-small.bin has trailing or missing data";
    return false;
  }
  if (!Sha256File(path, sha256, reason)) {
    return false;
  }
  if (sha256 != kSmallModelSha256) {
    reason = "dlssnr-small.bin is not the pinned 310.9.16 small model (SHA-256 " + sha256 + ")";
    return false;
  }
  return true;
}

bool ValidateShaders(const std::filesystem::path& data, std::string& reason) {
  const std::filesystem::path shaders = data / "shaders";
  const char* const required[] = {
      "shader-constants.txt",           "accumulation.txt",
      "swin-bias-storage.txt",          "coherent-act.txt",
      "runtime/runtime_alpha.spv",      "runtime/runtime_transfer.spv",
      "runtime/runtime_depth.spv",      "temporal/shader-constants.txt",
      "temporal/temporal_pre_fp32.spv", "temporal/temporal_post_fp32.spv"};
  for (const char* name : required) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(shaders / name, ec)) {
      reason = std::string("missing shader file ") + name;
      return false;
    }
  }
  std::ifstream constants(shaders / "shader-constants.txt");
  std::string text((std::istreambuf_iterator<char>(constants)), std::istreambuf_iterator<char>());
  if (text.find("math_profile") == std::string::npos || text.find("3") == std::string::npos) {
    reason = "shaders/shader-constants.txt does not declare math_profile 3";
    return false;
  }
  return true;
}

}  // namespace

bool IsCompiledIn() {
#if defined(REX_HAS_DLSS5_AMD) && REX_HAS_DLSS5_AMD
  return true;
#else
  return false;
#endif
}

bool Selected() {
  return IsCompiledIn() && REXCVAR_GET(dlss5_amd);
}

const Status& GetStatus() {
  return g_status;
}

void SetDeviceUnsupported(const std::string& reason) {
  g_device_reason = reason;
  if (g_status.requested && g_status.state != State::kInactive) {
    g_status.state = State::kInactive;
    g_status.reason = reason;
  }
}

namespace {
void SetInactive(const std::string& reason) {
  g_status.state = State::kInactive;
  g_status.reason = reason;
  REXLOG_WARN("DLSS 5 NR (AMD) inactive: {}", reason);
}
}  // namespace

void InitializeAssets() {
  if (g_assets_initialized) {
    return;
  }
  g_assets_initialized = true;
  g_status.compiled_in = IsCompiledIn();
  g_status.requested = REXCVAR_GET(dlss5_amd);
  if (!g_status.requested) {
    g_status.state = State::kOff;
    return;
  }
  if (!g_status.compiled_in) {
    SetInactive("this build has no DLSS 5 NR (AMD) support");
    return;
  }
  if (!g_device_reason.empty()) {
    SetInactive(g_device_reason);
    return;
  }

  std::filesystem::path root = REXCVAR_GET(dlss5_amd_data_path);
  if (root.empty()) {
    root = rex::filesystem::GetExecutablePath().parent_path();
  }
  std::error_code ec;
  const std::filesystem::path absolute = std::filesystem::weakly_canonical(root, ec);
  if (!ec) {
    root = absolute;
  }
  g_status.data_root = root.string();
  const std::filesystem::path data = root / "dlssnr-amd";
  if (!std::filesystem::is_directory(data, ec)) {
    if (root.filename() == "dlssnr-amd") {
      SetInactive("dlss5_amd_data_path points at dlssnr-amd itself; point it at the parent folder");
    } else {
      SetInactive("no dlssnr-amd folder at " + root.string());
    }
    return;
  }
  g_status.model_path = (data / "dlssnr-small.bin").string();
  std::string reason;
  if (!ValidateModelPack(g_status.model_path, reason, g_status.model_sha256)) {
    SetInactive(reason);
    return;
  }
  g_status.model_entries = kSmallModelEntries;
  if (!ValidateShaders(data, reason)) {
    SetInactive(reason);
    return;
  }
  g_status.state = State::kReady;
  g_status.reason.clear();
  REXLOG_INFO("DLSS 5 NR (AMD): assets ready at {} ({} weights, {})", g_status.data_root,
              g_status.model_entries, g_status.model_sha256);
}

void InitializeRuntime(const vulkan::VulkanDevice& device, uint32_t width, uint32_t height) {
#if defined(REX_HAS_DLSS5_AMD) && REX_HAS_DLSS5_AMD
  if (!Selected() || g_status.state != State::kReady || g_runtime) {
    return;
  }
  if (device.queue_family_graphics_compute() == UINT32_MAX) {
    SetInactive("no graphics+compute queue family");
    return;
  }
  try {
    vulkan::VulkanDevice::Queue::Acquisition queue =
        device.AcquireQueue(device.queue_family_graphics_compute(), 0);
    nr::HostDevice host;
    host.instance = device.vulkan_instance()->instance();
    host.physical = device.physical_device();
    host.device = device.device();
    host.queue = queue.queue();
    host.queue_family = device.queue_family_graphics_compute();
    nr::RuntimeConfig config;
    config.root = g_status.data_root;
    config.width = width;
    config.height = height;
    config.colour_format = VK_FORMAT_R8G8B8A8_UNORM;
    config.linear_input = false;
    config.white_point = 1.0f;
    config.network = 1;
    config.model_scale = 1.0f;
    config.max_passes = 1;
    nr::TemporalConfig temporal;
    temporal.enable = true;
    temporal.history_strength = 1.0f;
    g_runtime = std::make_unique<nr::Runtime>(host, config, nr::ControlMaskConfig{}, temporal);
    if (int(g_runtime->network()) != 1) {
      g_runtime.reset();
      SetInactive("the runtime did not load the small model");
      return;
    }
    g_status.gpu = device.properties().deviceName;
    g_status.state = State::kReady;
    REXLOG_INFO("DLSS 5 NR (AMD): runtime built on {} at {}x{} (small model)", g_status.gpu, width,
                height);
  } catch (const std::exception& e) {
    g_runtime.reset();
    SetInactive(std::string("runtime initialization failed: ") + e.what());
  }
#else
  (void)device;
  (void)width;
  (void)height;
#endif
}

void ReportRecordedFrame(bool applied, float gpu_ms) {
  if (!applied) {
    return;
  }
  ++g_status.recorded_frames;
  g_status.average_gpu_ms = gpu_ms;
  if (g_status.state == State::kReady) {
    g_status.state = State::kActive;
  }
}

void Shutdown() {
#if defined(REX_HAS_DLSS5_AMD) && REX_HAS_DLSS5_AMD
  g_runtime.reset();
#endif
}

}  // namespace rex::ui::dlss5_amd
