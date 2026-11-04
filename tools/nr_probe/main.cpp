// One-shot offline DLSS 5 NR probe on captured engine dumps.
//
// Proves the DLSSNR-AMD runtime accepts our colour/depth/motion data at
// 1920x1080 and produces a valid image, without touching the game's seam. It
// creates its own Vulkan device (nrvk::Context::create, RX 9070 XT / NR_GPU),
// loads a same-frame c/d/m triple, pre-upscales colour (and nearest-upsamples
// motion/depth) to 1920x1080, records one or two frames with reset on the first,
// and writes the answer as a PPM plus statistics.
//
// This is not a game integration and must not be reported as one.

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "nrvk.hpp"
#include "nr_runtime.hpp"

namespace {

constexpr uint32_t kSceneWidth = 1000;
constexpr uint32_t kSceneHeight = 600;
constexpr uint32_t kOutWidth = 1920;
constexpr uint32_t kOutHeight = 1080;

std::vector<uint8_t> ReadFile(const std::string& path, size_t expected) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot open " + path);
  }
  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  file.seekg(0, std::ios::beg);
  if (size != std::streamoff(expected)) {
    throw std::runtime_error(path + ": " + std::to_string(size) + " bytes, expected " +
                             std::to_string(expected));
  }
  std::vector<uint8_t> data(expected);
  file.read(reinterpret_cast<char*>(data.data()), std::streamsize(expected));
  if (!file) {
    throw std::runtime_error("short read from " + path);
  }
  return data;
}

float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  uint32_t exponent = (h >> 10) & 0x1Fu;
  uint32_t mantissa = h & 0x3FFu;
  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      exponent = 127 - 15 + 1;
      while ((mantissa & 0x400u) == 0) {
        mantissa <<= 1;
        --exponent;
      }
      mantissa &= 0x3FFu;
      bits = sign | (exponent << 23) | (mantissa << 13);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
  }
  return std::bit_cast<float>(bits);
}

uint16_t FloatToHalf(float value) {
  const uint32_t bits = std::bit_cast<uint32_t>(value);
  const uint32_t sign = (bits >> 16) & 0x8000u;
  int32_t exponent = int32_t((bits >> 23) & 0xFFu) - 127 + 15;
  uint32_t mantissa = bits & 0x7FFFFFu;
  if (exponent <= 0) {
    if (exponent < -10) {
      return uint16_t(sign);
    }
    mantissa = (mantissa | 0x800000u) >> uint32_t(1 - exponent);
    return uint16_t(sign | ((mantissa + 0x1000u) >> 13));
  }
  if (exponent >= 31) {
    return uint16_t(sign | 0x7C00u);
  }
  return uint16_t(sign | (uint32_t(exponent) << 10) | ((mantissa + 0x1000u) >> 13));
}

// Encoded SDR, bilinear. The PPM path is a probe, not the production resampler.
std::vector<uint8_t> UpscaleColour(const std::vector<uint8_t>& input) {
  std::vector<uint8_t> output(size_t(kOutWidth) * kOutHeight * 4);
  const float scale_x = float(kSceneWidth) / float(kOutWidth);
  const float scale_y = float(kSceneHeight) / float(kOutHeight);
  for (uint32_t y = 0; y < kOutHeight; ++y) {
    const float sy = (float(y) + 0.5f) * scale_y - 0.5f;
    const int32_t y0 = int32_t(std::floor(sy));
    const float fy = sy - float(y0);
    const int32_t y0c = std::clamp(y0, 0, int32_t(kSceneHeight) - 1);
    const int32_t y1c = std::clamp(y0 + 1, 0, int32_t(kSceneHeight) - 1);
    for (uint32_t x = 0; x < kOutWidth; ++x) {
      const float sx = (float(x) + 0.5f) * scale_x - 0.5f;
      const int32_t x0 = int32_t(std::floor(sx));
      const float fx = sx - float(x0);
      const int32_t x0c = std::clamp(x0, 0, int32_t(kSceneWidth) - 1);
      const int32_t x1c = std::clamp(x0 + 1, 0, int32_t(kSceneWidth) - 1);
      const uint8_t* p00 = input.data() + (size_t(y0c) * kSceneWidth + x0c) * 4;
      const uint8_t* p10 = input.data() + (size_t(y0c) * kSceneWidth + x1c) * 4;
      const uint8_t* p01 = input.data() + (size_t(y1c) * kSceneWidth + x0c) * 4;
      const uint8_t* p11 = input.data() + (size_t(y1c) * kSceneWidth + x1c) * 4;
      uint8_t* out = output.data() + (size_t(y) * kOutWidth + x) * 4;
      for (uint32_t c = 0; c < 4; ++c) {
        const float v0 = float(p00[c]) * (1.0f - fx) + float(p10[c]) * fx;
        const float v1 = float(p01[c]) * (1.0f - fx) + float(p11[c]) * fx;
        out[c] = uint8_t(std::clamp(v0 * (1.0f - fy) + v1 * fy + 0.5f, 0.0f, 255.0f));
      }
    }
  }
  return output;
}

std::vector<float> UpscaleDepth(const std::vector<uint8_t>& input) {
  const float* source = reinterpret_cast<const float*>(input.data());
  std::vector<float> output(size_t(kOutWidth) * kOutHeight);
  for (uint32_t y = 0; y < kOutHeight; ++y) {
    const uint32_t sy = std::min(y * kSceneHeight / kOutHeight, kSceneHeight - 1);
    for (uint32_t x = 0; x < kOutWidth; ++x) {
      const uint32_t sx = std::min(x * kSceneWidth / kOutWidth, kSceneWidth - 1);
      output[size_t(y) * kOutWidth + x] = source[size_t(sy) * kSceneWidth + sx];
    }
  }
  return output;
}

std::vector<uint16_t> UpscaleMotion(const std::vector<uint8_t>& input) {
  std::vector<uint16_t> output(size_t(kOutWidth) * kOutHeight * 2);
  for (uint32_t y = 0; y < kOutHeight; ++y) {
    const uint32_t sy = std::min(y * kSceneHeight / kOutHeight, kSceneHeight - 1);
    for (uint32_t x = 0; x < kOutWidth; ++x) {
      const uint32_t sx = std::min(x * kSceneWidth / kOutWidth, kSceneWidth - 1);
      const uint8_t* pixel = input.data() + (size_t(sy) * kSceneWidth + sx) * 8;
      uint32_t packed;
      std::memcpy(&packed, pixel, sizeof(packed));
      const float mx = HalfToFloat(uint16_t(packed & 0xFFFFu));
      const float my = HalfToFloat(uint16_t(packed >> 16));
      uint16_t* out = output.data() + (size_t(y) * kOutWidth + x) * 2;
      out[0] = FloatToHalf(mx);
      out[1] = FloatToHalf(my);
    }
  }
  return output;
}

uint32_t FindMemoryType(VkPhysicalDeviceMemoryProperties mem, uint32_t bits,
                        VkMemoryPropertyFlags properties) {
  for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
    if ((bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & properties) == properties) {
      return i;
    }
  }
  throw std::runtime_error("no suitable Vulkan memory type");
}

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0;
  uint32_t height = 0;
};

Image CreateImage(const nrvk::Context& ctx, VkFormat format, uint32_t width, uint32_t height,
                  VkImageUsageFlags usage) {
  Image result;
  result.format = format;
  result.width = width;
  result.height = height;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  NRVK_CHECK(vkCreateImage(ctx.device, &info, nullptr, &result.image));
  VkMemoryRequirements requirements;
  vkGetImageMemoryRequirements(ctx.device, result.image, &requirements);
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex =
      FindMemoryType(ctx.mem, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  NRVK_CHECK(vkAllocateMemory(ctx.device, &allocate, nullptr, &result.memory));
  NRVK_CHECK(vkBindImageMemory(ctx.device, result.image, result.memory, 0));
  return result;
}

void DestroyImage(const nrvk::Context& ctx, Image& image) {
  if (image.image != VK_NULL_HANDLE) {
    vkDestroyImage(ctx.device, image.image, nullptr);
    image.image = VK_NULL_HANDLE;
  }
  if (image.memory != VK_NULL_HANDLE) {
    vkFreeMemory(ctx.device, image.memory, nullptr);
    image.memory = VK_NULL_HANDLE;
  }
}

struct CommandContext {
  VkDevice device = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer buffer = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;

  explicit CommandContext(const nrvk::Context& ctx) : device(ctx.device) {
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = ctx.family;
    NRVK_CHECK(vkCreateCommandPool(ctx.device, &pool_info, nullptr, &pool));
    VkCommandBufferAllocateInfo buffer_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    buffer_info.commandPool = pool;
    buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    buffer_info.commandBufferCount = 1;
    NRVK_CHECK(vkAllocateCommandBuffers(ctx.device, &buffer_info, &buffer));
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    NRVK_CHECK(vkCreateFence(ctx.device, &fence_info, nullptr, &fence));
  }

  ~CommandContext() {
    if (device != VK_NULL_HANDLE) {
      if (fence != VK_NULL_HANDLE) {
        vkDestroyFence(device, fence, nullptr);
      }
      if (pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, pool, nullptr);
      }
    }
  }

  void Begin() {
    NRVK_CHECK(vkResetCommandBuffer(buffer, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    NRVK_CHECK(vkBeginCommandBuffer(buffer, &begin));
  }

  void Submit(const nrvk::Context& ctx) {
    NRVK_CHECK(vkEndCommandBuffer(buffer));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &buffer;
    NRVK_CHECK(vkQueueSubmit(ctx.queue, 1, &submit, fence));
    NRVK_CHECK(vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, UINT64_MAX));
    NRVK_CHECK(vkResetFences(ctx.device, 1, &fence));
  }
};

void TransitionImage(VkCommandBuffer buffer, VkImage image, VkImageLayout from, VkImageLayout to) {
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask =
      from == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? VK_ACCESS_TRANSFER_WRITE_BIT : 0;
  barrier.dstAccessMask =
      to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
          ? VK_ACCESS_TRANSFER_WRITE_BIT
          : (to == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ? VK_ACCESS_TRANSFER_READ_BIT
                                                        : VK_ACCESS_MEMORY_READ_BIT);
  barrier.oldLayout = from;
  barrier.newLayout = to;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void UploadImage(const nrvk::Context& ctx, CommandContext& commands, Image& image, const void* data,
                 VkDeviceSize bytes) {
  VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = bytes;
  buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory staging_memory = VK_NULL_HANDLE;
  NRVK_CHECK(vkCreateBuffer(ctx.device, &buffer_info, nullptr, &staging));
  VkMemoryRequirements requirements;
  vkGetBufferMemoryRequirements(ctx.device, staging, &requirements);
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex =
      FindMemoryType(ctx.mem, requirements.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  NRVK_CHECK(vkAllocateMemory(ctx.device, &allocate, nullptr, &staging_memory));
  NRVK_CHECK(vkBindBufferMemory(ctx.device, staging, staging_memory, 0));
  void* mapped = nullptr;
  NRVK_CHECK(vkMapMemory(ctx.device, staging_memory, 0, bytes, 0, &mapped));
  std::memcpy(mapped, data, size_t(bytes));
  vkUnmapMemory(ctx.device, staging_memory);

  commands.Begin();
  TransitionImage(commands.buffer, image.image, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {image.width, image.height, 1};
  vkCmdCopyBufferToImage(commands.buffer, staging, image.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  TransitionImage(commands.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_GENERAL);
  commands.Submit(ctx);

  vkDestroyBuffer(ctx.device, staging, nullptr);
  vkFreeMemory(ctx.device, staging_memory, nullptr);
}

std::vector<uint8_t> DownloadImage(const nrvk::Context& ctx, CommandContext& commands, Image& image,
                                   VkDeviceSize bytes) {
  VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = bytes;
  buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  NRVK_CHECK(vkCreateBuffer(ctx.device, &buffer_info, nullptr, &buffer));
  VkMemoryRequirements requirements;
  vkGetBufferMemoryRequirements(ctx.device, buffer, &requirements);
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex =
      FindMemoryType(ctx.mem, requirements.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  NRVK_CHECK(vkAllocateMemory(ctx.device, &allocate, nullptr, &memory));
  NRVK_CHECK(vkBindBufferMemory(ctx.device, buffer, memory, 0));

  commands.Begin();
  TransitionImage(commands.buffer, image.image, VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {image.width, image.height, 1};
  vkCmdCopyImageToBuffer(commands.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer,
                         1, &copy);
  commands.Submit(ctx);

  std::vector<uint8_t> result(static_cast<size_t>(bytes));
  void* mapped = nullptr;
  NRVK_CHECK(vkMapMemory(ctx.device, memory, 0, bytes, 0, &mapped));
  std::memcpy(result.data(), mapped, size_t(bytes));
  vkUnmapMemory(ctx.device, memory);
  vkDestroyBuffer(ctx.device, buffer, nullptr);
  vkFreeMemory(ctx.device, memory, nullptr);
  return result;
}

void WritePpm(const std::string& path, const std::vector<uint8_t>& rgba, uint32_t width,
              uint32_t height) {
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot write " + path);
  }
  file << "P6\n" << width << " " << height << "\n255\n";
  std::vector<uint8_t> rgb(size_t(width) * height * 3);
  for (size_t i = 0, o = 0; i < size_t(width) * height; ++i, o += 4) {
    rgb[i * 3 + 0] = rgba[o + 0];
    rgb[i * 3 + 1] = rgba[o + 1];
    rgb[i * 3 + 2] = rgba[o + 2];
  }
  file.write(reinterpret_cast<const char*>(rgb.data()), std::streamsize(rgb.size()));
}

void PrintStats(const char* label, const std::vector<uint8_t>& rgba) {
  double sum[3] = {};
  double sum_sq[3] = {};
  uint8_t min_value[3] = {255, 255, 255};
  uint8_t max_value[3] = {0, 0, 0};
  const size_t pixels = rgba.size() / 4;
  for (size_t i = 0; i < pixels; ++i) {
    for (int c = 0; c < 3; ++c) {
      const uint8_t v = rgba[i * 4 + size_t(c)];
      sum[c] += v;
      sum_sq[c] += double(v) * v;
      min_value[c] = std::min(min_value[c], v);
      max_value[c] = std::max(max_value[c], v);
    }
  }
  std::printf("%s: %zu px, mean %.2f/%.2f/%.2f, std ", label, pixels, sum[0] / pixels,
              sum[1] / pixels, sum[2] / pixels);
  for (int c = 0; c < 3; ++c) {
    const double mean = sum[c] / pixels;
    std::printf("%.2f", std::sqrt(std::max(0.0, sum_sq[c] / pixels - mean * mean)));
    if (c != 2)
      std::printf("/");
  }
  std::printf(", min %u/%u/%u, max %u/%u/%u\n", min_value[0], min_value[1], min_value[2],
              max_value[0], max_value[1], max_value[2]);
}

nr::ColourFrame MakeFrame(const Image& image, VkImageUsageFlags usage) {
  nr::ColourFrame frame;
  frame.image = image.image;
  frame.format = image.format;
  frame.width = image.width;
  frame.height = image.height;
  frame.before = VK_IMAGE_LAYOUT_GENERAL;
  frame.after = VK_IMAGE_LAYOUT_GENERAL;
  frame.before_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  frame.before_access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  frame.after_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  frame.after_access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  frame.usage = usage;
  return frame;
}

const char* Usage() {
  return "usage: nr_probe --root ROOT --colour c.bin --depth d.bin --motion m.bin "
         "--out out.ppm [--frames N]\n"
         "  ROOT is the folder that contains dlssnr-amd/ (not dlssnr-amd itself).\n"
         "  c.bin: 1000x600 RGBA8 (2400000 B), d.bin: 1000x600 float32 (2400000 B),\n"
         "  m.bin: 1000x600 RGBA16F packed xy (4800000 B), forward guest-NDC.\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string root;
  std::string colour_path;
  std::string depth_path;
  std::string motion_path;
  std::string out_path;
  uint32_t frames = 2;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto take = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error(std::string("missing value for ") + name);
      }
      return argv[++i];
    };
    if (arg == "--root") {
      root = take("--root");
    } else if (arg == "--colour") {
      colour_path = take("--colour");
    } else if (arg == "--depth") {
      depth_path = take("--depth");
    } else if (arg == "--motion") {
      motion_path = take("--motion");
    } else if (arg == "--out") {
      out_path = take("--out");
    } else if (arg == "--frames") {
      frames = uint32_t(std::stoul(take("--frames")));
    } else if (arg == "--help" || arg == "-h") {
      std::printf("%s", Usage());
      return 0;
    } else {
      throw std::runtime_error("unknown argument " + arg);
    }
  }
  if (root.empty() || colour_path.empty() || depth_path.empty() || motion_path.empty() ||
      out_path.empty()) {
    std::printf("%s", Usage());
    return 2;
  }
  if (!std::filesystem::is_directory(std::filesystem::path(root) / "dlssnr-amd")) {
    throw std::runtime_error(root + " does not contain dlssnr-amd/ (point at the parent)");
  }
  if (frames == 0) {
    frames = 1;
  }

  nrvk::Context ctx;
  ctx.create();
  std::printf("device: %s (%s)\n", ctx.gpu_name.c_str(), ctx.driver_name.c_str());

  CommandContext commands(ctx);

  const std::vector<uint8_t> colour_scene =
      ReadFile(colour_path, size_t(kSceneWidth) * kSceneHeight * 4);
  const std::vector<uint8_t> depth_scene =
      ReadFile(depth_path, size_t(kSceneWidth) * kSceneHeight * 4);
  const std::vector<uint8_t> motion_scene =
      ReadFile(motion_path, size_t(kSceneWidth) * kSceneHeight * 8);
  const std::vector<uint8_t> colour_1080 = UpscaleColour(colour_scene);
  const std::vector<float> depth_1080 = UpscaleDepth(depth_scene);
  const std::vector<uint16_t> motion_1080 = UpscaleMotion(motion_scene);

  const VkImageUsageFlags colour_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                         VK_IMAGE_USAGE_SAMPLED_BIT;
  const VkImageUsageFlags target_usage = colour_usage | VK_IMAGE_USAGE_STORAGE_BIT;
  const VkImageUsageFlags input_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                        VK_IMAGE_USAGE_SAMPLED_BIT;
  Image colour = CreateImage(ctx, VK_FORMAT_R8G8B8A8_UNORM, kOutWidth, kOutHeight, colour_usage);
  Image target = CreateImage(ctx, VK_FORMAT_R8G8B8A8_UNORM, kOutWidth, kOutHeight, target_usage);
  Image motion = CreateImage(ctx, VK_FORMAT_R16G16_SFLOAT, kOutWidth, kOutHeight, input_usage);
  Image depth = CreateImage(ctx, VK_FORMAT_R32_SFLOAT, kOutWidth, kOutHeight, input_usage);
  UploadImage(ctx, commands, colour, colour_1080.data(), colour_1080.size());
  UploadImage(ctx, commands, motion, motion_1080.data(), motion_1080.size() * 2);
  UploadImage(ctx, commands, depth, depth_1080.data(), depth_1080.size() * 4);

  nr::HostDevice host;
  host.instance = ctx.instance;
  host.physical = ctx.physical;
  host.device = ctx.device;
  host.queue = ctx.queue;
  host.queue_family = ctx.family;
  nr::RuntimeConfig config;
  config.root = root;
  config.width = kOutWidth;
  config.height = kOutHeight;
  config.colour_format = VK_FORMAT_R8G8B8A8_UNORM;
  config.linear_input = false;
  config.white_point = 1.0f;
  config.network = 1;
  config.model_scale = 1.0f;
  config.max_passes = 1;
  nr::TemporalConfig temporal;
  temporal.enable = true;
  temporal.history_strength = 1.0f;
  nr::Runtime runtime(host, config, nr::ControlMaskConfig{}, temporal);
  std::printf("runtime: network=%d model=%ux%u live_features=%u\n", int(runtime.network()),
              runtime.model_width(), runtime.model_height(), uint32_t(runtime.live_features()));

  nr::Controls controls;
  const bool use_target = runtime.takes_target(controls);
  std::printf("takes_target=%d frames=%u\n", use_target ? 1 : 0, frames);

  nr::EngineFrame frame;
  frame.motion = MakeFrame(motion, input_usage);
  frame.motion_scale_x = -0.5f;
  frame.motion_scale_y = 0.5f;
  frame.motion_texture_width = kOutWidth;
  frame.motion_texture_height = kOutHeight;
  frame.depth = MakeFrame(depth, input_usage);
  frame.depth_inverted = false;
  frame.feature = 0;

  VkDeviceSize answer_bytes = VkDeviceSize(kOutWidth) * kOutHeight * 4;
  for (uint32_t i = 0; i < frames; ++i) {
    commands.Begin();
    frame.colour = MakeFrame(colour, colour_usage);
    if (use_target) {
      frame.target = MakeFrame(target, target_usage);
    } else {
      frame.target = nr::ColourFrame{};
    }
    frame.reset = (i == 0);
    nr::EngineResult result = runtime.record_engine(commands.buffer, frame, controls);
    commands.Submit(ctx);
    std::printf("frame %u: applied=%d dispatches=%u history_consumed=%d provider=%s gpu_ms=%.3f\n",
                i, result.frame.applied ? 1 : 0, result.frame.network_dispatches,
                result.history_consumed ? 1 : 0, result.motion_provider,
                double(runtime.last_gpu_ms()));
  }

  Image& answer = use_target ? target : colour;
  const std::vector<uint8_t> result_rgba = DownloadImage(ctx, commands, answer, answer_bytes);
  PrintStats("output", result_rgba);
  WritePpm(out_path, result_rgba, kOutWidth, kOutHeight);
  std::printf("wrote %s\n", out_path.c_str());

  DestroyImage(ctx, colour);
  DestroyImage(ctx, target);
  DestroyImage(ctx, motion);
  DestroyImage(ctx, depth);
  return 0;
}
