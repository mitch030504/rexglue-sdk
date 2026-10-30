/**
 * @file        src/ui/vulkan/vulkan_temporal_inputs.cpp
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/vulkan/temporal_inputs.h>

#include <rex/logging.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/util.h>

namespace rex::ui::vulkan {

namespace shaders {
#include "../shaders/vulkan_spirv/temporal_motion_cs.h"
}  // namespace shaders

namespace {

// Bindings of shaders/temporal_motion_cs.glsl.
enum Binding : uint32_t {
  kBindingCurrentColor,
  kBindingPreviousColor,
  kBindingCurrentLumaQuarter,
  kBindingPreviousLumaQuarter,
  kBindingCoarseMotion,
  kBindingMotion,
  kBindingCount,
};

struct PushConstants {
  uint32_t width;
  uint32_t height;
  uint32_t pass_index;
};

// Group size of the shader.
constexpr uint32_t kGroupSize = 8;
// Quarter-resolution pixels per coarse block, and full-resolution pixels per fine block.
constexpr uint32_t kCoarseBlock = 4;
constexpr uint32_t kFineBlock = 8;
// Constant depth handed to the upscaler, which requires one but gets nothing better here.
constexpr float kConstantDepth = 0.5f;

uint32_t DivideRoundUp(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

VkImageMemoryBarrier ImageBarrier(VkImage image, VkAccessFlags src_access,
                                  VkAccessFlags dst_access, VkImageLayout old_layout,
                                  VkImageLayout new_layout) {
  VkImageMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = dst_access;
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = util::InitializeSubresourceRange();
  return barrier;
}

}  // namespace

std::unique_ptr<VulkanTemporalInputs> VulkanTemporalInputs::Create(
    const VulkanDevice* vulkan_device) {
  std::unique_ptr<VulkanTemporalInputs> inputs(new VulkanTemporalInputs(vulkan_device));
  if (!inputs->Initialize()) {
    return nullptr;
  }
  return inputs;
}

VulkanTemporalInputs::~VulkanTemporalInputs() {
  DestroySizedResources();
  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, pipeline_);
  util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device, pipeline_layout_);
  util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device, descriptor_pool_);
  util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device, descriptor_set_layout_);
}

bool VulkanTemporalInputs::Initialize() {
  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  std::array<VkDescriptorSetLayoutBinding, kBindingCount> bindings = {};
  for (uint32_t i = 0; i < kBindingCount; i++) {
    bindings[i].binding = i;
    bindings[i].descriptorType = i <= kBindingPreviousColor ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                                            : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo set_layout_info = {};
  set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_layout_info.bindingCount = uint32_t(bindings.size());
  set_layout_info.pBindings = bindings.data();
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr,
                                      &descriptor_set_layout_) != VK_SUCCESS) {
    REXLOG_ERROR("VulkanTemporalInputs: Failed to create the descriptor set layout");
    return false;
  }

  std::array<VkDescriptorPoolSize, 2> pool_sizes = {};
  pool_sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  pool_sizes[0].descriptorCount = 2 * uint32_t(descriptor_sets_.size());
  pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  pool_sizes[1].descriptorCount = (kBindingCount - 2) * uint32_t(descriptor_sets_.size());
  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = uint32_t(descriptor_sets_.size());
  pool_info.poolSizeCount = uint32_t(pool_sizes.size());
  pool_info.pPoolSizes = pool_sizes.data();
  if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
    REXLOG_ERROR("VulkanTemporalInputs: Failed to create the descriptor pool");
    return false;
  }
  std::array<VkDescriptorSetLayout, 2> set_layouts = {descriptor_set_layout_,
                                                      descriptor_set_layout_};
  VkDescriptorSetAllocateInfo set_allocate_info = {};
  set_allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  set_allocate_info.descriptorPool = descriptor_pool_;
  set_allocate_info.descriptorSetCount = uint32_t(set_layouts.size());
  set_allocate_info.pSetLayouts = set_layouts.data();
  if (dfn.vkAllocateDescriptorSets(device, &set_allocate_info, descriptor_sets_.data()) !=
      VK_SUCCESS) {
    REXLOG_ERROR("VulkanTemporalInputs: Failed to allocate the descriptor sets");
    return false;
  }

  VkPushConstantRange push_constant_range = {};
  push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  push_constant_range.size = sizeof(PushConstants);
  VkPipelineLayoutCreateInfo pipeline_layout_info = {};
  pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_info.setLayoutCount = 1;
  pipeline_layout_info.pSetLayouts = &descriptor_set_layout_;
  pipeline_layout_info.pushConstantRangeCount = 1;
  pipeline_layout_info.pPushConstantRanges = &push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout_) !=
      VK_SUCCESS) {
    REXLOG_ERROR("VulkanTemporalInputs: Failed to create the pipeline layout");
    return false;
  }
  pipeline_ = util::CreateComputePipeline(vulkan_device_, pipeline_layout_,
                                          shaders::temporal_motion_cs,
                                          sizeof(shaders::temporal_motion_cs));
  if (pipeline_ == VK_NULL_HANDLE) {
    REXLOG_ERROR("VulkanTemporalInputs: Failed to create the motion estimation pipeline");
    return false;
  }
  return true;
}

bool VulkanTemporalInputs::CreateImage(Image& image, VkFormat format, uint32_t width,
                                       uint32_t height, VkImageUsageFlags usage) {
  image.info = {};
  image.info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image.info.imageType = VK_IMAGE_TYPE_2D;
  image.info.format = format;
  image.info.extent = {width, height, 1};
  image.info.mipLevels = 1;
  image.info.arrayLayers = 1;
  image.info.samples = VK_SAMPLE_COUNT_1_BIT;
  image.info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image.info.usage = usage;
  image.info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image.info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!util::CreateDedicatedAllocationImage(vulkan_device_, image.info,
                                            util::MemoryPurpose::kDeviceLocal, image.image,
                                            image.memory)) {
    return false;
  }
  VkImageViewCreateInfo view_info = {};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = image.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format;
  view_info.subresourceRange = util::InitializeSubresourceRange();
  return vulkan_device_->functions().vkCreateImageView(vulkan_device_->device(), &view_info,
                                                       nullptr, &image.view) == VK_SUCCESS;
}

void VulkanTemporalInputs::DestroyImage(Image& image) {
  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, image.view);
  util::DestroyAndNullHandle(dfn.vkDestroyImage, device, image.image);
  util::DestroyAndNullHandle(dfn.vkFreeMemory, device, image.memory);
}

void VulkanTemporalInputs::DestroySizedResources() {
  for (Image& image : colors_) DestroyImage(image);
  for (Image& image : lumas_quarter_) DestroyImage(image);
  DestroyImage(coarse_motion_);
  DestroyImage(motion_);
  DestroyImage(depth_);
  width_ = 0;
  height_ = 0;
}

bool VulkanTemporalInputs::CreateSizedResources(uint32_t width, uint32_t height) {
  DestroySizedResources();
  const uint32_t quarter_width = std::max(width / 4, 1u);
  const uint32_t quarter_height = std::max(height / 4, 1u);
  bool ok = true;
  for (Image& image : colors_) {
    ok = ok && CreateImage(image, VulkanPresenter::kGuestOutputFormat, width, height,
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
  }
  for (Image& image : lumas_quarter_) {
    ok = ok && CreateImage(image, VK_FORMAT_R16_SFLOAT, quarter_width, quarter_height,
                           VK_IMAGE_USAGE_STORAGE_BIT);
  }
  ok = ok && CreateImage(coarse_motion_, VK_FORMAT_R16G16_SFLOAT,
                         DivideRoundUp(quarter_width, kCoarseBlock),
                         DivideRoundUp(quarter_height, kCoarseBlock), VK_IMAGE_USAGE_STORAGE_BIT);
  ok = ok && CreateImage(motion_, VK_FORMAT_R16G16_SFLOAT, width, height,
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  ok = ok && CreateImage(depth_, VK_FORMAT_R32_SFLOAT, width, height,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
  if (!ok) {
    REXLOG_ERROR("VulkanTemporalInputs: Failed to create {}x{} images", width, height);
    DestroySizedResources();
    return false;
  }

  for (uint32_t set = 0; set < descriptor_sets_.size(); set++) {
    const uint32_t other = set ^ 1;
    std::array<VkDescriptorImageInfo, kBindingCount> image_infos = {};
    image_infos[kBindingCurrentColor] = {VK_NULL_HANDLE, colors_[set].view,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    image_infos[kBindingPreviousColor] = {VK_NULL_HANDLE, colors_[other].view,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    image_infos[kBindingCurrentLumaQuarter] = {VK_NULL_HANDLE, lumas_quarter_[set].view,
                                               VK_IMAGE_LAYOUT_GENERAL};
    image_infos[kBindingPreviousLumaQuarter] = {VK_NULL_HANDLE, lumas_quarter_[other].view,
                                                VK_IMAGE_LAYOUT_GENERAL};
    image_infos[kBindingCoarseMotion] = {VK_NULL_HANDLE, coarse_motion_.view,
                                         VK_IMAGE_LAYOUT_GENERAL};
    image_infos[kBindingMotion] = {VK_NULL_HANDLE, motion_.view, VK_IMAGE_LAYOUT_GENERAL};
    std::array<VkWriteDescriptorSet, kBindingCount> writes = {};
    for (uint32_t i = 0; i < kBindingCount; i++) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = descriptor_sets_[set];
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = i <= kBindingPreviousColor ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                                            : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      writes[i].pImageInfo = &image_infos[i];
    }
    vulkan_device_->functions().vkUpdateDescriptorSets(
        vulkan_device_->device(), uint32_t(writes.size()), writes.data(), 0, nullptr);
  }

  width_ = width;
  height_ = height;
  parity_ = 0;
  frames_recorded_ = 0;
  return true;
}

bool VulkanTemporalInputs::Record(VkCommandBuffer command_buffer, VkImage color, uint32_t width,
                                  uint32_t height, bool& has_history_out) {
  has_history_out = false;
  if (width != width_ || height != height_) {
    if (!CreateSizedResources(width, height)) {
      return false;
    }
  }
  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const Image& current = colors_[parity_];
  const Image& previous = colors_[parity_ ^ 1];

  if (frames_recorded_ == 0) {
    // First use of these images: put everything in its steady-state layout, and fill the depth.
    std::array<VkImageMemoryBarrier, 6> barriers = {
        ImageBarrier(previous.image, 0, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        ImageBarrier(lumas_quarter_[0].image, 0, VK_ACCESS_SHADER_READ_BIT |
                     VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
        ImageBarrier(lumas_quarter_[1].image, 0, VK_ACCESS_SHADER_READ_BIT |
                     VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
        ImageBarrier(coarse_motion_.image, 0, VK_ACCESS_SHADER_READ_BIT |
                     VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
        ImageBarrier(motion_.image, 0, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        ImageBarrier(depth_.image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
    };
    dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, uint32_t(barriers.size()),
                             barriers.data());
    VkClearColorValue depth_value = {};
    depth_value.float32[0] = kConstantDepth;
    VkImageSubresourceRange range = util::InitializeSubresourceRange();
    dfn.vkCmdClearColorImage(command_buffer, depth_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &depth_value, 1, &range);
    VkImageMemoryBarrier depth_to_read =
        ImageBarrier(depth_.image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &depth_to_read);
  }

  // Copy the guest output into this frame's color image.
  {
    std::array<VkImageMemoryBarrier, 2> barriers = {
        ImageBarrier(color, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
        // Last read as the previous-but-one frame: contents are replaced.
        ImageBarrier(current.image, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
    };
    dfn.vkCmdPipelineBarrier(command_buffer,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             uint32_t(barriers.size()), barriers.data());
    VkImageCopy region = {};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = {width, height, 1};
    dfn.vkCmdCopyImage(command_buffer, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, current.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barriers = {
        ImageBarrier(color, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        ImageBarrier(current.image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
    };
    // The motion image was last read by the upscaler; it's fully rewritten below.
    VkImageMemoryBarrier motion_to_write =
        ImageBarrier(motion_.image, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    std::array<VkImageMemoryBarrier, 3> all = {barriers[0], barriers[1], motion_to_write};
    dfn.vkCmdPipelineBarrier(command_buffer,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, uint32_t(all.size()), all.data());
  }

  dfn.vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
  dfn.vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_, 0,
                              1, &descriptor_sets_[parity_], 0, nullptr);
  const uint32_t quarter_width = std::max(width / 4, 1u);
  const uint32_t quarter_height = std::max(height / 4, 1u);
  const std::array<std::pair<uint32_t, uint32_t>, 3> pass_sizes = {{
      {quarter_width, quarter_height},
      {DivideRoundUp(quarter_width, kCoarseBlock), DivideRoundUp(quarter_height, kCoarseBlock)},
      {DivideRoundUp(width, kFineBlock), DivideRoundUp(height, kFineBlock)},
  }};
  // Without history, only the luma pass runs; the motion image is just cleared below.
  const uint32_t pass_count = frames_recorded_ ? 3 : 1;
  for (uint32_t pass = 0; pass < pass_count; pass++) {
    if (pass) {
      VkMemoryBarrier barrier = {};
      barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr,
                               0, nullptr);
    }
    PushConstants constants = {width, height, pass};
    dfn.vkCmdPushConstants(command_buffer, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(constants), &constants);
    dfn.vkCmdDispatch(command_buffer, DivideRoundUp(pass_sizes[pass].first, kGroupSize),
                      DivideRoundUp(pass_sizes[pass].second, kGroupSize), 1);
  }
  if (!frames_recorded_) {
    VkClearColorValue zero = {};
    VkImageSubresourceRange range = util::InitializeSubresourceRange();
    dfn.vkCmdClearColorImage(command_buffer, motion_.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1,
                             &range);
  }

  // Luma written by this frame is read as the previous one next frame; motion goes to the upscaler.
  VkImageMemoryBarrier motion_to_read =
      ImageBarrier(motion_.image, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  VkMemoryBarrier luma_barrier = {};
  luma_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  luma_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  luma_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  dfn.vkCmdPipelineBarrier(command_buffer,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &luma_barrier, 0, nullptr,
                           1, &motion_to_read);

  has_history_out = frames_recorded_ != 0;
  if (frames_recorded_ < UINT32_MAX) {
    frames_recorded_++;
  }
  parity_ ^= 1;
  return true;
}

}  // namespace rex::ui::vulkan
