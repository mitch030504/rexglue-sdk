/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>

#include <rex/assert.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/deferred_command_buffer.h>
#include <rex/graphics/vulkan/primitive_processor.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_BOOL(vulkan_force_expand_point_sprites_in_vs, false, "GPU/Vulkan",
                    "Force Vulkan point sprite expansion in the vertex shader, even when geometry "
                    "shaders are available")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    vulkan_force_expand_rectangle_lists_in_vs, false, "GPU/Vulkan",
    "Force Vulkan rectangle list expansion in the vertex shader, even when geometry "
    "shaders are available")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(vulkan_force_convert_quad_lists_to_triangle_lists, false, "GPU/Vulkan",
                    "Force Vulkan quad list conversion to triangle lists in primitive processing, "
                    "even when geometry shaders are available")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::graphics::vulkan {

namespace {

// FNV-1a over the index words in stream order. The identity of a draw's index
// set must be order-sensitive: two draws can read the same words in a different
// order and map them to different vertices.
constexpr uint64_t kMotionIndexHashOffset = 14695981039346656037ull;
constexpr uint64_t kMotionIndexHashPrime = 1099511628211ull;

}  // namespace

VulkanPrimitiveProcessor::~VulkanPrimitiveProcessor() {
  Shutdown(true);
}

bool VulkanPrimitiveProcessor::Initialize() {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->properties();
  // Keep triangle fan handling aligned with D3D12 by always converting fans to
  // lists in the shared primitive processor path.
  constexpr bool triangle_fans_supported_without_conversion = false;

  // Default to D3D12-like geometry-shader-based handling when available, but
  // allow opting into Vulkan fallback paths for debugging and downlevel
  // compatibility testing.
  bool geometry_shader_primitive_emulation_allowed = device_properties.geometryShader;
  bool quad_lists_supported_without_conversion =
      geometry_shader_primitive_emulation_allowed &&
      !REXCVAR_GET(vulkan_force_convert_quad_lists_to_triangle_lists);
  bool point_sprites_supported_without_vs_expansion =
      geometry_shader_primitive_emulation_allowed &&
      !REXCVAR_GET(vulkan_force_expand_point_sprites_in_vs);
  bool rectangle_lists_supported_without_vs_expansion =
      geometry_shader_primitive_emulation_allowed &&
      !REXCVAR_GET(vulkan_force_expand_rectangle_lists_in_vs);

  if (!InitializeCommon(
          device_properties.fullDrawIndexUint32, triangle_fans_supported_without_conversion, false,
          quad_lists_supported_without_conversion, point_sprites_supported_without_vs_expansion,
          rectangle_lists_supported_without_vs_expansion)) {
    Shutdown();
    return false;
  }
  frame_index_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      vulkan_device, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      std::max(size_t(kMinRequiredConvertedIndexBufferSize),
               ui::GraphicsUploadBufferPool::kDefaultPageSize));
  return true;
}

void VulkanPrimitiveProcessor::Shutdown(bool from_destructor) {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  frame_index_buffers_.clear();
  frame_index_buffer_pool_.reset();
  builtin_index_buffer_host_.clear();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, builtin_index_buffer_upload_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         builtin_index_buffer_upload_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, builtin_index_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, builtin_index_buffer_memory_);

  if (!from_destructor) {
    ShutdownCommon();
  }
}

void VulkanPrimitiveProcessor::CompletedSubmissionUpdated() {
  if (builtin_index_buffer_upload_ != VK_NULL_HANDLE &&
      command_processor_.GetCompletedSubmission() >= builtin_index_buffer_upload_submission_) {
    const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                           builtin_index_buffer_upload_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                           builtin_index_buffer_upload_memory_);
  }
}

void VulkanPrimitiveProcessor::BeginSubmission() {
  if (builtin_index_buffer_upload_ != VK_NULL_HANDLE &&
      builtin_index_buffer_upload_submission_ == UINT64_MAX) {
    // No need to submit deferred barriers - builtin_index_buffer_ has never
    // been used yet, and builtin_index_buffer_upload_ is written before
    // submitting commands reading it.

    command_processor_.EndRenderPass();

    DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();

    VkBufferCopy* copy_region =
        command_buffer.CmdCopyBufferEmplace(builtin_index_buffer_upload_, builtin_index_buffer_, 1);
    copy_region->srcOffset = 0;
    copy_region->dstOffset = 0;
    copy_region->size = builtin_index_buffer_size_;

    command_processor_.PushBufferMemoryBarrier(
        builtin_index_buffer_, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_INDEX_READ_BIT);

    builtin_index_buffer_upload_submission_ = command_processor_.GetCurrentSubmission();
  }
}

void VulkanPrimitiveProcessor::BeginFrame() {
  frame_index_buffer_pool_->Reclaim(command_processor_.GetCompletedFrame());
}

void VulkanPrimitiveProcessor::EndSubmission() {
  frame_index_buffer_pool_->FlushWrites();
}

void VulkanPrimitiveProcessor::EndFrame() {
  ClearPerFrameCache();
  frame_index_buffers_.clear();
}

bool VulkanPrimitiveProcessor::Process(ProcessingResult& result_out) {
  if (!PrimitiveProcessor::Process(result_out)) {
    return false;
  }
  // Reset first: a draw that cannot be scanned must never inherit the previous
  // draw's bounds, because the command processor would key clip history on them.
  motion_index_bounds_valid_ = false;
  motion_index_min_ = 0;
  motion_index_max_ = 0;
  motion_index_fingerprint_ = 0;
  // Only while the motion guide is active: this is a second walk of the index
  // stream on top of the conversion the base processor already did, so a
  // guide-off run pays nothing for it.
  if (command_processor_.motion_guide_enabled() && result_out.host_draw_vertex_count) {
    ScanMotionIndexBounds(result_out);
  }
  return true;
}

void VulkanPrimitiveProcessor::ScanMotionIndexBounds(const ProcessingResult& result_out) {
  const uint32_t count = result_out.host_draw_vertex_count;
  const bool is_16bit = result_out.host_index_format == xenos::IndexFormat::kInt16;
  const uint32_t reset_index = is_16bit ? UINT32_C(0xFFFF) : UINT32_C(0xFFFFFFFF);
  const bool is_dma = result_out.index_buffer_type == ProcessedIndexBufferType::kGuestDMA;
  // The stream the draw will actually read, not the guest source: converted
  // buffers are addressed with gl_VertexIndex, and the built-in ones are the
  // expansion ordinals the vertex shader receives.
  const void* stream = nullptr;
  switch (result_out.index_buffer_type) {
    case ProcessedIndexBufferType::kNone:
      // Auto-indexed: Vulkan's gl_VertexIndex is the vertex ordinal, no buffer.
      break;
    case ProcessedIndexBufferType::kHostConverted:
      if (result_out.host_index_buffer_handle < frame_index_buffers_.size()) {
        stream = frame_index_buffers_[result_out.host_index_buffer_handle].host_data;
      }
      break;
    case ProcessedIndexBufferType::kHostBuiltinForAuto:
    case ProcessedIndexBufferType::kHostBuiltinForDMA:
      // The handle is a byte offset into the immutable built-in index buffer.
      if (result_out.host_index_buffer_handle <= builtin_index_buffer_host_.size() &&
          uint64_t(count) * (is_16bit ? sizeof(uint16_t) : sizeof(uint32_t)) <=
              builtin_index_buffer_host_.size() - result_out.host_index_buffer_handle) {
        stream = builtin_index_buffer_host_.data() + result_out.host_index_buffer_handle;
      }
      break;
    case ProcessedIndexBufferType::kGuestDMA:
      stream = memory().TranslatePhysical(result_out.guest_index_base);
      break;
  }
  if (!stream && result_out.index_buffer_type != ProcessedIndexBufferType::kNone) {
    if (!motion_index_scan_failure_logged_) {
      motion_index_scan_failure_logged_ = true;
      REXGPU_WARN(
          "motion_guide: the index stream for a draw is not CPU-readable "
          "(type {}, handle {}, guest base 0x{:08X}); that draw records no clip history",
          uint32_t(result_out.index_buffer_type), result_out.host_index_buffer_handle,
          result_out.guest_index_base);
    }
    return;
  }

  // Usually gl_VertexIndex is the index value from the bound buffer. It is the
  // ordinal instead for a non-indexed draw, and for the shader-side 32-bit DMA
  // load (no fullDrawIndexUint32: the draw is non-indexed and the vertex shader
  // reads the guest index at that ordinal). The block must follow whatever the
  // motion store actually sees.
  const bool ordinal_vertex_index =
      result_out.index_buffer_type == ProcessedIndexBufferType::kNone ||
      (is_dma && result_out.host_index_format == xenos::IndexFormat::kInt32 &&
       !command_processor_.GetVulkanDevice()->properties().fullDrawIndexUint32 &&
       result_out.host_vertex_shader_type == Shader::HostVertexShaderType::kVertex);

  uint64_t hash = kMotionIndexHashOffset;
  auto hash_value = [&hash](uint64_t value) {
    hash ^= value;
    hash *= kMotionIndexHashPrime;
  };
  // Everything that selects which vertices this draw reads. The stream hash
  // below catches contents changing behind an unchanged address, which is the
  // case no pointer or generation is exposed for.
  hash_value(uint64_t(result_out.index_buffer_type));
  // The host handle is a per-frame upload-pool cursor for converted buffers, so
  // it shifts when earlier conversions in the frame change; it is an identity
  // only for the immutable built-in streams, where it is a fixed byte offset.
  if (result_out.index_buffer_type == ProcessedIndexBufferType::kHostBuiltinForAuto ||
      result_out.index_buffer_type == ProcessedIndexBufferType::kHostBuiltinForDMA) {
    hash_value(uint64_t(result_out.host_index_buffer_handle));
  }
  hash_value(uint64_t(result_out.guest_index_base));
  hash_value(uint64_t(result_out.guest_draw_vertex_count));
  hash_value(uint64_t(count));
  hash_value(uint64_t(result_out.host_index_format) |
             (uint64_t(result_out.host_shader_index_endian) << 8) |
             (uint64_t(result_out.host_primitive_reset_enabled) << 16));
  // The base vertex changes the fetched vertex without changing the stream.
  hash_value(register_file()[XE_GPU_REG_VGT_INDX_OFFSET]);

  // Built-in DMA adapter: gl_VertexIndex is the strip ordinal, but the vertex each
  // ordinal fetches comes from the guest index buffer loaded in the vertex shader,
  // so its contents are part of the identity even though they do not bound the
  // block. No primitive reset skip here: the non-indexed expansion still invokes
  // the shader for every ordinal.
  if (result_out.index_buffer_type == ProcessedIndexBufferType::kHostBuiltinForDMA) {
    const xenos::IndexFormat guest_format =
        register_file().Get<reg::VGT_DRAW_INITIATOR>().index_size;
    const uint32_t guest_count = result_out.guest_draw_vertex_count;
    const void* guest_stream = memory().TranslatePhysical(result_out.guest_index_base);
    if (!guest_stream) {
      if (!motion_index_scan_failure_logged_) {
        motion_index_scan_failure_logged_ = true;
        REXGPU_WARN(
            "motion_guide: the guest index stream behind a DMA expansion at 0x{:08X} is not "
            "CPU-readable; that draw records no clip history",
            result_out.guest_index_base);
      }
      return;
    }
    if (guest_format == xenos::IndexFormat::kInt16) {
      const uint16_t* guest_indices = reinterpret_cast<const uint16_t*>(guest_stream);
      for (uint32_t i = 0; i < guest_count; ++i) {
        hash_value(uint64_t(guest_indices[i]));
      }
    } else {
      const uint32_t* guest_indices = reinterpret_cast<const uint32_t*>(guest_stream);
      for (uint32_t i = 0; i < guest_count; ++i) {
        hash_value(uint64_t(guest_indices[i]));
      }
    }
  }

  bool has_index = false;
  uint32_t index_min = 0;
  uint32_t index_max = 0;
  for (uint32_t i = 0; i < count; ++i) {
    // Raw values, not the endian-parsed fetch index: the motion shader keys its
    // slots on the raw gl_VertexIndex built-in, so the block must cover exactly
    // that stream. It is stable across frames even when it is byte-reversed.
    uint32_t value;
    if (result_out.index_buffer_type == ProcessedIndexBufferType::kNone) {
      value = i;
    } else if (is_dma) {
      // Guest DMA is read from big-endian guest memory by the shader's index
      // fetch, so gl_VertexIndex is the decoded value. Reading the stream raw
      // here byte-reverses every 16-bit index and widens a compact mesh to a
      // ~64K span, which fills the arena and leaves the shader's slots outside
      // the block. Decode exactly like the fetch does.
      const uint8_t* bytes = reinterpret_cast<const uint8_t*>(stream);
      value = is_16bit ? uint32_t(memory::load_and_swap<uint16_t>(bytes + size_t(i) * sizeof(uint16_t)))
                       : memory::load_and_swap<uint32_t>(bytes + size_t(i) * sizeof(uint32_t));
    } else if (is_16bit) {
      value = uint32_t(reinterpret_cast<const uint16_t*>(stream)[i]);
    } else {
      value = reinterpret_cast<const uint32_t*>(stream)[i];
    }
    hash_value(value);
    // Primitive reset separators never invoke the vertex stage, so they must not
    // widen the block; they stay in the hash as part of the index-set identity.
    if (!ordinal_vertex_index && result_out.host_primitive_reset_enabled && value == reset_index) {
      continue;
    }
    // The motion shader keys its slots on gl_VertexIndex & 0xFFFFFF, which is the
    // ordinal for the non-indexed paths.
    value = ordinal_vertex_index ? i : (value & xenos::kVertexIndexMask);
    if (!has_index) {
      index_min = index_max = value;
      has_index = true;
    } else {
      index_min = std::min(index_min, value);
      index_max = std::max(index_max, value);
    }
  }
  if (!has_index) {
    // Only resets: no vertex invocation, so no slot can be written this frame.
    return;
  }
  motion_index_min_ = index_min;
  motion_index_max_ = index_max;
  motion_index_fingerprint_ = hash;
  motion_index_bounds_valid_ = true;
}

bool VulkanPrimitiveProcessor::InitializeBuiltinIndexBuffer(
    size_t size_bytes, std::function<void(void*)> fill_callback) {
  assert_not_zero(size_bytes);
  assert_true(builtin_index_buffer_ == VK_NULL_HANDLE);
  assert_true(builtin_index_buffer_memory_ == VK_NULL_HANDLE);
  assert_true(builtin_index_buffer_upload_ == VK_NULL_HANDLE);
  assert_true(builtin_index_buffer_upload_memory_ == VK_NULL_HANDLE);

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  builtin_index_buffer_size_ = VkDeviceSize(size_bytes);
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, builtin_index_buffer_size_,
          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, builtin_index_buffer_,
          builtin_index_buffer_memory_)) {
    REXGPU_ERROR(
        "Vulkan primitive processor: Failed to create the built-in index "
        "buffer GPU resource with {} bytes",
        size_bytes);
    return false;
  }
  uint32_t upload_memory_type;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, builtin_index_buffer_size_, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          ui::vulkan::util::MemoryPurpose::kUpload, builtin_index_buffer_upload_,
          builtin_index_buffer_upload_memory_, &upload_memory_type)) {
    REXGPU_ERROR(
        "Vulkan primitive processor: Failed to create the built-in index "
        "buffer upload resource with {} bytes",
        size_bytes);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, builtin_index_buffer_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, builtin_index_buffer_memory_);
    return false;
  }

  void* mapping;
  if (dfn.vkMapMemory(device, builtin_index_buffer_upload_memory_, 0, VK_WHOLE_SIZE, 0, &mapping) !=
      VK_SUCCESS) {
    REXGPU_ERROR(
        "Vulkan primitive processor: Failed to map the built-in index buffer "
        "upload resource with {} bytes",
        size_bytes);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                           builtin_index_buffer_upload_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                           builtin_index_buffer_upload_memory_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, builtin_index_buffer_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, builtin_index_buffer_memory_);
    return false;
  }
  fill_callback(mapping);
  // The built-in streams never change after creation, so a host copy is the
  // exact stream every expansion draw reads - the motion guide's index scan needs
  // it after the upload mapping is gone.
  builtin_index_buffer_host_.resize(size_bytes);
  std::memcpy(builtin_index_buffer_host_.data(), mapping, size_bytes);
  ui::vulkan::util::FlushMappedMemoryRange(vulkan_device, builtin_index_buffer_memory_,
                                           upload_memory_type);
  dfn.vkUnmapMemory(device, builtin_index_buffer_upload_memory_);

  // Schedule uploading in the first submission.
  builtin_index_buffer_upload_submission_ = UINT64_MAX;
  return true;
}

void* VulkanPrimitiveProcessor::RequestHostConvertedIndexBufferForCurrentFrame(
    xenos::IndexFormat format, uint32_t index_count, bool coalign_for_simd,
    uint32_t coalignment_original_address, size_t& backend_handle_out) {
  size_t index_size = format == xenos::IndexFormat::kInt16 ? sizeof(uint16_t) : sizeof(uint32_t);
  VkBuffer buffer;
  VkDeviceSize offset;
  uint8_t* mapping = frame_index_buffer_pool_->Request(
      command_processor_.GetCurrentFrame(),
      index_size * index_count + (coalign_for_simd ? XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE : 0),
      index_size, buffer, offset);
  if (!mapping) {
    return nullptr;
  }
  if (coalign_for_simd) {
    ptrdiff_t coalignment_offset = GetSimdCoalignmentOffset(mapping, coalignment_original_address);
    mapping += coalignment_offset;
    offset = VkDeviceSize(offset + coalignment_offset);
  }
  backend_handle_out = frame_index_buffers_.size();
  frame_index_buffers_.push_back(FrameIndexBuffer{buffer, offset, mapping});
  return mapping;
}

}  // namespace rex::graphics::vulkan
