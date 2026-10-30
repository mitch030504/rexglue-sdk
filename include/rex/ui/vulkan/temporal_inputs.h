#pragma once
/**
 * @file        rex/ui/vulkan/temporal_inputs.h
 * @brief       Color history, motion vectors and depth for the presenter's temporal upscaler.
 *
 * The guest gives the presenter only its final image, so motion is estimated from consecutive
 * images (shaders/temporal_motion_cs.glsl) and depth is a constant.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <cstdint>
#include <memory>

#include <rex/ui/vulkan/device.h>

namespace rex::ui::vulkan {

class VulkanTemporalInputs {
 public:
  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageCreateInfo info = {};
  };

  static std::unique_ptr<VulkanTemporalInputs> Create(const VulkanDevice* vulkan_device);
  ~VulkanTemporalInputs();

  VulkanTemporalInputs(const VulkanTemporalInputs&) = delete;
  VulkanTemporalInputs& operator=(const VulkanTemporalInputs&) = delete;

  // Records the inputs for a new frame. `color` must be a kGuestOutputFormat image in
  // SHADER_READ_ONLY_OPTIMAL and is left that way. Afterwards color(), motion() and depth() are in
  // SHADER_READ_ONLY_OPTIMAL. has_history_out is false when there is no previous frame to match
  // against (first frame, or the size changed), and the upscaler must reset. The caller must have
  // awaited the GPU before a size change, as the images are recreated.
  bool Record(VkCommandBuffer command_buffer, VkImage color, uint32_t width, uint32_t height,
              bool& has_history_out);

  const Image& color() const { return colors_[parity_ ^ 1]; }  // the frame just recorded
  const Image& motion() const { return motion_; }
  const Image& depth() const { return depth_; }

 private:
  explicit VulkanTemporalInputs(const VulkanDevice* vulkan_device)
      : vulkan_device_(vulkan_device) {}
  bool Initialize();
  bool CreateSizedResources(uint32_t width, uint32_t height);
  void DestroySizedResources();
  bool CreateImage(Image& image, VkFormat format, uint32_t width, uint32_t height,
                   VkImageUsageFlags usage);
  void DestroyImage(Image& image);

  const VulkanDevice* vulkan_device_;

  VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  // Index of the color/luma images the next frame is written to; the other pair is the previous.
  uint32_t parity_ = 0;
  uint32_t frames_recorded_ = 0;
  std::array<Image, 2> colors_;
  std::array<Image, 2> lumas_quarter_;
  Image coarse_motion_;
  Image motion_;
  Image depth_;
  // Set i reads colors_[i] / lumas_quarter_[i] as current and the other pair as previous.
  std::array<VkDescriptorSet, 2> descriptor_sets_ = {};
};

}  // namespace rex::ui::vulkan
