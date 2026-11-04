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
#include <vector>

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
REXCVAR_DEFINE_INT32(dlss5_amd_mode, 0, "GPU",
                     "Development: 0 = off, 1 = substitute a pre-upscaled identity scene, "
                     "2 = substitute an NR image. Requires dlss5_amd=true and dlss5_amd_image.")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(dlss5_amd_image, "", "GPU",
                      "Development: 1920x1080 raw RGBA8 image substituted for the composition "
                      "source by dlss5_amd_mode 1/2. Used for the seam mapping gate.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(dlss5_amd_frame, -1, "GPU",
                     "Development: only substitute on this guest frame; -1 = every frame.")
    .range(-1, 1000000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(dlss5_amd_view, 1, "GPU",
                     "Development: view format used for the substitute image - 0 = "
                     "R8G8B8A8_UNORM (domain-identical to the guest composition fetch, which "
                     "is UNORM end-to-end), 1 = R8G8B8A8_SRGB (legacy). The image itself is "
                     "always created UNORM so both views are legal reinterprets.")
    .range(0, 1)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

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
VkImage g_substitute_image = VK_NULL_HANDLE;
VkDeviceMemory g_substitute_memory = VK_NULL_HANDLE;
// Both reinterprets of the one UNORM image, created together at upload; the
// active one is picked per dlss5_amd_view in SubstituteView().
VkImageView g_substitute_view_unorm = VK_NULL_HANDLE;
VkImageView g_substitute_view_srgb = VK_NULL_HANDLE;
VkDevice g_substitute_device = VK_NULL_HANDLE;
PFN_vkDestroyImageView g_substitute_destroy_view = nullptr;
PFN_vkDestroyImage g_substitute_destroy_image = nullptr;
PFN_vkFreeMemory g_substitute_free_memory = nullptr;
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

bool SubstituteEnabled() {
  return g_substitute_view_unorm != VK_NULL_HANDLE && REXCVAR_GET(dlss5_amd_mode) != 0;
}

bool SubstituteForFrame(uint64_t frame) {
  const int32_t first = REXCVAR_GET(dlss5_amd_frame);
  return first < 0 || uint64_t(first) == frame;
}

VkImageView SubstituteView() {
  // dlss5_amd_view is kInitOnly and both views are created at upload, so this
  // is a pure selection between the UNORM and SRGB reinterpret of the image.
  return REXCVAR_GET(dlss5_amd_view) == 0 ? g_substitute_view_unorm : g_substitute_view_srgb;
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

uint32_t FindMemoryType(const VkPhysicalDeviceMemoryProperties& memory, uint32_t bits,
                        VkMemoryPropertyFlags flags) {
  for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
    if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) {
      return i;
    }
  }
  return UINT32_MAX;
}

void TransitionSubstitute(const vulkan::VulkanDevice::Functions& dfn,
                          VkCommandBuffer command_buffer, VkImage image, VkImageLayout from,
                          VkImageLayout to, VkPipelineStageFlags src_stage,
                          VkPipelineStageFlags dst_stage, VkAccessFlags src_access,
                          VkAccessFlags dst_access) {
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = dst_access;
  barrier.oldLayout = from;
  barrier.newLayout = to;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  dfn.vkCmdPipelineBarrier(command_buffer, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1,
                           &barrier);
}

// One-shot upload of the 1920x1080 raw RGBA8 substitute image. The image is
// only used by the seam mapping gate; the production path will build it from
// the live scene instead.
bool UploadSubstituteImage(const vulkan::VulkanDevice& device, VkQueue queue, uint32_t family,
                           const std::string& path) {
  const vulkan::VulkanInstance::Functions& ifn = device.vulkan_instance()->functions();
  const vulkan::VulkanDevice::Functions& dfn = device.functions();
  constexpr uint32_t kWidth = 1920;
  constexpr uint32_t kHeight = 1080;
  constexpr VkDeviceSize kBytes = VkDeviceSize(kWidth) * kHeight * 4;
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    REXLOG_WARN("DLSS 5 NR (AMD): cannot open substitute image {}", path);
    return false;
  }
  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  file.seekg(0, std::ios::beg);
  if (size != std::streamoff(kBytes)) {
    REXLOG_WARN("DLSS 5 NR (AMD): substitute image is {} bytes, expected {}", int64_t(size),
                int64_t(kBytes));
    return false;
  }
  std::vector<uint8_t> pixels(static_cast<size_t>(kBytes));
  file.read(reinterpret_cast<char*>(pixels.data()), std::streamsize(kBytes));
  if (!file) {
    return false;
  }

  VkPhysicalDeviceMemoryProperties memory;
  ifn.vkGetPhysicalDeviceMemoryProperties(device.physical_device(), &memory);

  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  // UNORM image so that both the UNORM and the SRGB view below are legal
  // reinterprets of the same texels; the guest chain is UNORM end-to-end, so
  // the raw bytes are stored verbatim and only the view picks the domain.
  image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  image_info.extent = {kWidth, kHeight, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (dfn.vkCreateImage(device.device(), &image_info, nullptr, &g_substitute_image) != VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements image_requirements;
  dfn.vkGetImageMemoryRequirements(device.device(), g_substitute_image, &image_requirements);
  VkMemoryAllocateInfo image_allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  image_allocate.allocationSize = image_requirements.size;
  image_allocate.memoryTypeIndex = FindMemoryType(memory, image_requirements.memoryTypeBits,
                                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (image_allocate.memoryTypeIndex == UINT32_MAX ||
      dfn.vkAllocateMemory(device.device(), &image_allocate, nullptr, &g_substitute_memory) !=
          VK_SUCCESS ||
      dfn.vkBindImageMemory(device.device(), g_substitute_image, g_substitute_memory, 0) !=
          VK_SUCCESS) {
    return false;
  }

  VkBufferCreateInfo staging_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  staging_info.size = kBytes;
  staging_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory staging_memory = VK_NULL_HANDLE;
  if (dfn.vkCreateBuffer(device.device(), &staging_info, nullptr, &staging) != VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements staging_requirements;
  dfn.vkGetBufferMemoryRequirements(device.device(), staging, &staging_requirements);
  VkMemoryAllocateInfo staging_allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  staging_allocate.allocationSize = staging_requirements.size;
  staging_allocate.memoryTypeIndex =
      FindMemoryType(memory, staging_requirements.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (staging_allocate.memoryTypeIndex == UINT32_MAX ||
      dfn.vkAllocateMemory(device.device(), &staging_allocate, nullptr, &staging_memory) !=
          VK_SUCCESS ||
      dfn.vkBindBufferMemory(device.device(), staging, staging_memory, 0) != VK_SUCCESS) {
    return false;
  }
  void* mapped = nullptr;
  if (dfn.vkMapMemory(device.device(), staging_memory, 0, kBytes, 0, &mapped) != VK_SUCCESS) {
    return false;
  }
  std::memcpy(mapped, pixels.data(), size_t(kBytes));
  dfn.vkUnmapMemory(device.device(), staging_memory);

  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.queueFamilyIndex = family;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  if (dfn.vkCreateCommandPool(device.device(), &pool_info, nullptr, &pool) != VK_SUCCESS) {
    return false;
  }
  VkCommandBufferAllocateInfo buffer_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  buffer_info.commandPool = pool;
  buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  buffer_info.commandBufferCount = 1;
  if (dfn.vkAllocateCommandBuffers(device.device(), &buffer_info, &command_buffer) != VK_SUCCESS) {
    return false;
  }
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (dfn.vkCreateFence(device.device(), &fence_info, nullptr, &fence) != VK_SUCCESS) {
    return false;
  }
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  dfn.vkBeginCommandBuffer(command_buffer, &begin);
  TransitionSubstitute(dfn, command_buffer, g_substitute_image, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {kWidth, kHeight, 1};
  dfn.vkCmdCopyBufferToImage(command_buffer, staging, g_substitute_image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  TransitionSubstitute(dfn, command_buffer, g_substitute_image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT);
  dfn.vkEndCommandBuffer(command_buffer);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command_buffer;
  const bool submitted =
      dfn.vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS &&
      dfn.vkWaitForFences(device.device(), 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
  dfn.vkDestroyFence(device.device(), fence, nullptr);
  dfn.vkDestroyCommandPool(device.device(), pool, nullptr);
  dfn.vkDestroyBuffer(device.device(), staging, nullptr);
  dfn.vkFreeMemory(device.device(), staging_memory, nullptr);
  if (!submitted) {
    return false;
  }

  // Two views of the one UNORM image: UNORM is domain-identical to the guest
  // composition fetch (no gamma PWL anywhere on the guest path), SRGB inserts
  // a decode the guest never applies. dlss5_amd_view picks the active one.
  VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = g_substitute_image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  if (dfn.vkCreateImageView(device.device(), &view_info, nullptr, &g_substitute_view_unorm) !=
      VK_SUCCESS) {
    return false;
  }
  view_info.format = VK_FORMAT_R8G8B8A8_SRGB;
  if (dfn.vkCreateImageView(device.device(), &view_info, nullptr, &g_substitute_view_srgb) !=
      VK_SUCCESS) {
    return false;
  }
  g_substitute_device = device.device();
  g_substitute_destroy_view = dfn.vkDestroyImageView;
  g_substitute_destroy_image = dfn.vkDestroyImage;
  g_substitute_free_memory = dfn.vkFreeMemory;
  const bool view_unorm = REXCVAR_GET(dlss5_amd_view) == 0;
  REXLOG_INFO("DLSS 5 NR (AMD): substitute image loaded from {} (1920x1080 RGBA8 UNORM image, "
              "active view {})",
              path, view_unorm ? "R8G8B8A8_UNORM" : "R8G8B8A8_SRGB");
  return true;
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
    const std::string substitute_path = REXCVAR_GET(dlss5_amd_image);
    if (!substitute_path.empty() && REXCVAR_GET(dlss5_amd_mode) != 0) {
      if (!UploadSubstituteImage(device, queue.queue(), device.queue_family_graphics_compute(),
                                 substitute_path)) {
        SetInactive("substitute image could not be loaded");
        return;
      }
    }
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
  if (g_substitute_device != VK_NULL_HANDLE) {
    if (g_substitute_view_unorm != VK_NULL_HANDLE) {
      g_substitute_destroy_view(g_substitute_device, g_substitute_view_unorm, nullptr);
      g_substitute_view_unorm = VK_NULL_HANDLE;
    }
    if (g_substitute_view_srgb != VK_NULL_HANDLE) {
      g_substitute_destroy_view(g_substitute_device, g_substitute_view_srgb, nullptr);
      g_substitute_view_srgb = VK_NULL_HANDLE;
    }
    if (g_substitute_image != VK_NULL_HANDLE) {
      g_substitute_destroy_image(g_substitute_device, g_substitute_image, nullptr);
      g_substitute_image = VK_NULL_HANDLE;
    }
    if (g_substitute_memory != VK_NULL_HANDLE) {
      g_substitute_free_memory(g_substitute_device, g_substitute_memory, nullptr);
      g_substitute_memory = VK_NULL_HANDLE;
    }
    g_substitute_device = VK_NULL_HANDLE;
  }
}

}  // namespace rex::ui::dlss5_amd
