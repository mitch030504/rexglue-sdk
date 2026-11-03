#pragma once
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

#include <cstdint>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/graphics/primitive_processor.h>
#include <rex/ui/vulkan/upload_buffer_pool.h>

namespace rex::graphics::vulkan {

class VulkanCommandProcessor;

class VulkanPrimitiveProcessor final : public PrimitiveProcessor {
 public:
  VulkanPrimitiveProcessor(const RegisterFile& register_file, memory::Memory& memory,
                           SharedMemory& shared_memory, VulkanCommandProcessor& command_processor)
      : PrimitiveProcessor(register_file, memory, shared_memory),
        command_processor_(command_processor) {}
  ~VulkanPrimitiveProcessor();

  bool Initialize();
  void Shutdown(bool from_destructor = false);
  void ClearCache() { frame_index_buffer_pool_->ClearCache(); }

  void CompletedSubmissionUpdated();
  void BeginSubmission();
  void BeginFrame();
  void EndSubmission();
  void EndFrame();

  // Hides the non-virtual PrimitiveProcessor::Process on purpose: the motion
  // guide needs the draw's effective index range while the index stream is still
  // CPU-readable, which is exactly inside processing. The base processing is
  // called unchanged, and this class is only ever owned as a
  // VulkanPrimitiveProcessor by the Vulkan command processor.
  bool Process(ProcessingResult& result_out);

  // Phase 2 motion guide: the range of the draw's raw gl_VertexIndex built-in (the
  // value the motion shader actually keys its slots on, masked to 24 bits,
  // primitive reset indices excluded because they never invoke the vertex stage),
  // and an order-sensitive fingerprint of that stream plus every identity field
  // that selects it. Only computed while the guide is active; valid() is false
  // when the stream cannot be read or contains no drawable index, in which case
  // the draw must record no clip history at all.
  bool motion_index_bounds_valid() const { return motion_index_bounds_valid_; }
  uint32_t motion_index_min() const { return motion_index_min_; }
  uint32_t motion_index_max() const { return motion_index_max_; }
  uint64_t motion_index_fingerprint() const { return motion_index_fingerprint_; }

  std::pair<VkBuffer, VkDeviceSize> GetBuiltinIndexBuffer(size_t handle) const {
    assert_not_null(builtin_index_buffer_);
    return std::make_pair(builtin_index_buffer_,
                          VkDeviceSize(GetBuiltinIndexBufferOffsetBytes(handle)));
  }
  std::pair<VkBuffer, VkDeviceSize> GetConvertedIndexBuffer(size_t handle) const {
    return std::make_pair(frame_index_buffers_[handle].buffer, frame_index_buffers_[handle].offset);
  }

 protected:
  bool InitializeBuiltinIndexBuffer(size_t size_bytes,
                                    std::function<void(void*)> fill_callback) override;

  void* RequestHostConvertedIndexBufferForCurrentFrame(xenos::IndexFormat format,
                                                       uint32_t index_count, bool coalign_for_simd,
                                                       uint32_t coalignment_original_address,
                                                       size_t& backend_handle_out) override;

 private:
  // Index buffer handed to the draw for this frame, plus the CPU-side stream the
  // motion guide scans. `host_data` follows the buffer's lifetime (the upload
  // pool page is reclaimed only after the frame completes), so it stays valid
  // through every draw of the frame that requested it.
  struct FrameIndexBuffer {
    VkBuffer buffer;
    VkDeviceSize offset;
    const void* host_data;
  };

  // Fills the motion_index_* members from the index stream the draw will read.
  // Leaves bounds_valid() false (and logs once) when it cannot be read.
  void ScanMotionIndexBounds(const ProcessingResult& result_out);

  VulkanCommandProcessor& command_processor_;

  VkDeviceSize builtin_index_buffer_size_ = 0;
  VkBuffer builtin_index_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory builtin_index_buffer_memory_ = VK_NULL_HANDLE;
  // Host copy of builtin_index_buffer_, taken while it is mapped for filling.
  // The built-in streams are immutable after creation, so this is the exact
  // stream every auto-indexed expansion draw reads.
  std::vector<uint8_t> builtin_index_buffer_host_;
  // Temporary buffer copied in the beginning of the first submission for
  // uploading to builtin_index_buffer_, destroyed when the submission when it
  // was uploaded is completed.
  VkBuffer builtin_index_buffer_upload_ = VK_NULL_HANDLE;
  VkDeviceMemory builtin_index_buffer_upload_memory_ = VK_NULL_HANDLE;
  // UINT64_MAX means not uploaded yet and needs uploading in the first
  // submission (if the upload buffer exists at all).
  uint64_t builtin_index_buffer_upload_submission_ = UINT64_MAX;

  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> frame_index_buffer_pool_;
  // Indexed by the backend handles.
  std::deque<FrameIndexBuffer> frame_index_buffers_;

  bool motion_index_bounds_valid_ = false;
  uint32_t motion_index_min_ = 0;
  uint32_t motion_index_max_ = 0;
  uint64_t motion_index_fingerprint_ = 0;
  bool motion_index_scan_failure_logged_ = false;
};

}  // namespace rex::graphics::vulkan
