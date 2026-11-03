#pragma once
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

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/render_target/cache.h>
#include <rex/graphics/vulkan/shared_memory.h>
#include <rex/graphics/vulkan/texture_cache.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/ui/vulkan/single_layout_descriptor_set_pool.h>
#include <rex/ui/vulkan/upload_buffer_pool.h>

namespace rex::graphics::vulkan {

class VulkanCommandProcessor;

class VulkanRenderTargetCache final : public RenderTargetCache {
 public:
  union RenderPassKey {
    struct {
      // If emulating 2x as 4x, this is still 2x for simplicity of using this
      // field to make guest-related decisions. Render pass objects are not very
      // expensive, and their dependencies can't be shared between 2x-as-4x and
      // true 4x MSAA passes (framebuffers because render target cache render
      // targets are different for 2x and 4x guest MSAA, pipelines because the
      // sample mask will have 2 samples excluded for 2x-as-4x).
      // This has effect only on the attachments, but even in cases when there
      // are no attachments, it can be used to pass the sample count between
      // subsystems, for instance, to specify the desired number of samples to
      // use when there are no attachments in pipelines.
      // Also, without attachments, using separate render passes for different
      // sample counts ensures that if the variableMultisampleRate feature is
      // not supported, no draws with different rasterization sample counts end
      // up in one render pass.
      xenos::MsaaSamples msaa_samples : xenos::kMsaaSamplesBits;  // 2
      // << 0 is depth, << 1...4 is color.
      uint32_t depth_and_color_used : 1 + xenos::kMaxColorRenderTargets;  // 7
      // 0 for unused attachments.
      // If VK_FORMAT_D24_UNORM_S8_UINT is not supported, this must be kD24FS8
      // even for kD24S8.
      xenos::DepthRenderTargetFormat depth_format : xenos::kDepthRenderTargetFormatBits;  // 8
      // Linear or sRGB included if host sRGB is used.
      xenos::ColorRenderTargetFormat color_0_view_format
          : xenos::kColorRenderTargetFormatBits;  // 12
      xenos::ColorRenderTargetFormat color_1_view_format
          : xenos::kColorRenderTargetFormatBits;  // 16
      xenos::ColorRenderTargetFormat color_2_view_format
          : xenos::kColorRenderTargetFormatBits;  // 20
      xenos::ColorRenderTargetFormat color_3_view_format
          : xenos::kColorRenderTargetFormatBits;    // 24
      uint32_t color_rts_use_transfer_formats : 1;  // 25
      // Phase 2 motion guide: this pass includes the guide attachment, so it needs
      // its own render pass and framebuffer objects. The attachment exists only
      // while the guide is active, which keeps every guide-off key identical.
      uint32_t motion_attachment_used : 1;  // 26
    };
    uint32_t key = 0;
    struct Hasher {
      size_t operator()(const RenderPassKey& key) const { return std::hash<uint32_t>{}(key.key); }
    };
    bool operator==(const RenderPassKey& other_key) const { return key == other_key.key; }
    bool operator!=(const RenderPassKey& other_key) const { return !(*this == other_key); }
    bool operator<(const RenderPassKey& other_key) const { return key < other_key.key; }
  };
  static_assert_size(RenderPassKey, sizeof(uint32_t));

  // Phase 2 motion guide attachment: `xy` is the final NDC motion (current minus
  // previous, jitter removed), `z` is 0, `w` is 1 where the draw had a complete
  // previous-frame index set and the fragment wrote a vector, 0 elsewhere
  // (including the per-frame clear). Single-sample by the time NR sees it.
  static constexpr VkFormat kMotionGuideAttachmentFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
  // The guide is appended after every attachment the pass already uses, and the
  // guest attachments are compacted (depth first if used, then the used colour
  // targets in order), so its index is a function of the pass rather than a
  // constant. A fixed index would be dead on the scene pass, which uses one
  // colour target (depth_and_color_used 0x02 or 0x03), not four. Both formulas
  // below are the only ways the guide may be named; every site that builds a
  // pass, framebuffer, blend state, attachment array or clear uses them, so the
  // index cannot drift from the objects that were created with the key.
  //
  // Subpass colour attachment reference index: the number of used guest colour
  // targets, because the guide is referenced right after the compacted guest
  // colours. This is also the guide's fragment output location and the
  // vkCmdClearAttachments.colorAttachment index. 0x02 -> 1, 0x03 -> 1,
  // 0x1E -> 4 (the old constant, now just one case of the formula).
  static constexpr uint32_t GetMotionGuideSubpassIndex(RenderPassKey key) {
    return rex::bit_count(uint32_t(key.depth_and_color_used >> 1));
  }
  // Render pass and framebuffer attachment description index: after all used
  // guest attachments, depth included. 0x02 -> 1, 0x03 -> 2, 0x1E -> 5.
  static constexpr uint32_t GetMotionGuideDescriptionIndex(RenderPassKey key) {
    return rex::bit_count(uint32_t(key.depth_and_color_used));
  }
  // Capacity only, never an index: the most colour attachments any pass can have
  // (four guest targets plus the guide), used for the dynamic rendering array and
  // the subpass reference array. The guide's own index comes from the key.
  static constexpr uint32_t kMotionGuideColorAttachmentCount = xenos::kMaxColorRenderTargets + 1;

  struct Framebuffer {
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkExtent2D host_extent{};
    // Phase 2 motion guide attachment. Created with the framebuffer, at exactly
    // this framebuffer's host_extent and the render pass's sample count, and
    // destroyed with it - the framebuffer is the only object that knows both
    // values exactly, so nothing can recompute them differently. All three are
    // VK_NULL_HANDLE unless the render pass key had motion_attachment_used, which
    // keeps guide-off framebuffers and their memory footprint unchanged.
    //
    // The image is not a guest render target: no EDRAM range owns it, no transfer
    // or resolve sees it, and it is not in the render target cache at all.
    VkImage motion_image = VK_NULL_HANDLE;
    VkDeviceMemory motion_memory = VK_NULL_HANDLE;
    VkImageView motion_view = VK_NULL_HANDLE;
    // Subpass colour attachment index of the guide in this framebuffer's render
    // pass (GetMotionGuideSubpassIndex of the key the pass was built from). It is
    // also the fragment output location and the index vkCmdClearAttachments
    // takes, so it is captured with the pass instead of recomputed at the clear.
    uint32_t motion_subpass_index = 0;
    // Host sample count the image was created with - the same value the render
    // pass was built with, recorded here because the resolve can only bind the
    // image to a sampler2DMS when it really is multisampled, and the key it came
    // from is not available when the frame-end composite reads it.
    VkSampleCountFlagBits motion_samples = VK_SAMPLE_COUNT_1_BIT;
    // Guest frame number this framebuffer was recorded for in the frame's
    // composite list, or UINT64_MAX if it has not received a motion-writing draw
    // yet. The first such draw in a frame stamps it and adds the framebuffer to
    // VulkanRenderTargetCache::motion_frame_framebuffers_; a second draw into the
    // same framebuffer in the same frame must not add it twice, and the stamp is
    // how the frame boundary (a new GetCurrentFrame) makes the next frame's first
    // draw add it again. mutable: recording a draw updates frame bookkeeping, not
    // the framebuffer's identity.
    mutable uint64_t motion_draw_frame = UINT64_MAX;
    // Draw order of this framebuffer's most recent motion-writing draw this frame.
    // The composite dispatches in this order, not first-touch order: the shader's
    // keep_existing is last-dispatch-wins, so a framebuffer re-entered after another
    // one must not keep the precedence of its earlier draw. mutable: bookkeeping for
    // the frame, not the framebuffer's identity.
    mutable uint64_t motion_last_draw_serial = 0;
    // The resolve's descriptor set for this image view, allocated from the cache's
    // pool the first time this framebuffer is resolved and never rewritten: a
    // descriptor set still bound by an earlier submission may be executing, so a
    // per-view set is the only safe way to bind a view that changes with the
    // framebuffer. Invalidated with the pool (Shutdown, ClearCache), which also
    // destroys every framebuffer. mutable: it caches a GPU binding for a view that
    // is already part of this framebuffer, not the framebuffer's identity.
    mutable VkDescriptorSet motion_descriptor_set = VK_NULL_HANDLE;
    // Guest frame number the guide was last cleared on entry to its pass. A pass
    // may be ended and resumed within one frame (transfers, texture loads), and a
    // resume must not erase the motion written before the interruption, so the
    // clear is a once-per-frame stamp rather than a per-entry action. mutable:
    // clearing mutates the attachment, not the framebuffer's identity.
    mutable uint64_t motion_cleared_frame = UINT64_MAX;
    // For ~Framebuffer: destroying the guide needs the Vulkan device, which the
    // framebuffer does not hold. Nested in the cache, it can reach it.
    VulkanRenderTargetCache* render_target_cache = nullptr;
    Framebuffer() = default;
    Framebuffer(VkFramebuffer framebuffer, const VkExtent2D& host_extent,
                VulkanRenderTargetCache* render_target_cache)
        : framebuffer(framebuffer),
          host_extent(host_extent),
          render_target_cache(render_target_cache) {}
    ~Framebuffer();
    // The guide's handles are owned by exactly one framebuffer; a copy would
    // double-destroy them. Framebuffers are only ever emplaced in place.
    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;
  };

  VulkanRenderTargetCache(const RegisterFile& register_file, const memory::Memory& memory,
                          uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y,
                          VulkanCommandProcessor& command_processor);
  ~VulkanRenderTargetCache();

  // Transient descriptor set layouts must be initialized in the command
  // processor.
  bool Initialize(uint32_t shared_memory_binding_count);
  void Shutdown(bool from_destructor = false);
  void ClearCache() override;

  void CompletedSubmissionUpdated();
  void EndSubmission();

  Path GetPath() const override { return path_; }

  VkBuffer edram_buffer() const { return edram_buffer_; }

  // Performs the resolve to a shared memory area according to the current
  // register values, and also clears the render targets if needed. Must be in a
  // frame for calling.
  bool Resolve(const memory::Memory& memory, VulkanSharedMemory& shared_memory,
               VulkanTextureCache& texture_cache, uint32_t& written_address_out,
               uint32_t& written_length_out);

  bool Update(bool is_rasterization_done, reg::RB_DEPTHCONTROL normalized_depth_control,
              uint32_t normalized_color_mask, const Shader& vertex_shader) override;
  // Binding information for the last successful update.
  RenderPassKey last_update_render_pass_key() const { return last_update_render_pass_key_; }
  VkRenderPass last_update_render_pass() const { return last_update_render_pass_; }
  const Framebuffer* last_update_framebuffer() const { return last_update_framebuffer_; }
  // Colour attachments of the last successful update, for dynamic rendering.
  // The used guest colour targets are written compacted from index 0 - the same
  // order the render pass's colour references and blend states use - and a pass
  // with motion_attachment_used appends the motion guide at
  // GetMotionGuideSubpassIndex, making the count colour_count + 1 (at most
  // kMotionGuideColorAttachmentCount). The caller must provide capacity for
  // kMotionGuideColorAttachmentCount entries, and color_attachment_capacity says
  // how many it has; a caller with less room gets the guide omitted rather than
  // an out-of-bounds write.
  void GetLastUpdateRenderingAttachments(VkRenderingAttachmentInfo* color_attachments,
                                         uint32_t color_attachment_capacity,
                                         uint32_t* color_attachment_count_out,
                                         VkRenderingAttachmentInfo* depth_attachment,
                                         VkRenderingAttachmentInfo* stencil_attachment) const;

  // Using R16G16[B16A16]_SNORM, which are -1...1, not the needed -32...32.
  // Persistent data doesn't depend on this, so can be overriden by per-game
  // configuration.
  bool IsFixedRG16TruncatedToMinus1To1() const {
    return GetPath() == Path::kHostRenderTargets && !color_rg16_draw_format_fallback_to_float_ &&
           !REXCVAR_GET(snorm16_render_target_full_range);
  }
  bool IsFixedRGBA16TruncatedToMinus1To1() const {
    return GetPath() == Path::kHostRenderTargets && !color_rgba16_draw_format_fallback_to_float_ &&
           !REXCVAR_GET(snorm16_render_target_full_range);
  }
  bool gamma_render_target_as_unorm16() const { return gamma_render_target_as_unorm16_; }

  bool depth_unorm24_vulkan_format_supported() const {
    return depth_unorm24_vulkan_format_supported_;
  }
  bool depth_float24_round() const { return depth_float24_round_; }
  bool depth_float24_convert_in_pixel_shader() const {
    return depth_float24_convert_in_pixel_shader_;
  }

  bool msaa_2x_attachments_supported() const { return msaa_2x_attachments_supported_; }
  bool msaa_2x_no_attachments_supported() const { return msaa_2x_no_attachments_supported_; }
  bool IsMsaa2xSupported(bool subpass_has_attachments) const {
    return subpass_has_attachments ? msaa_2x_attachments_supported_
                                   : msaa_2x_no_attachments_supported_;
  }

  // Returns the render pass object, or VK_NULL_HANDLE if failed to create.
  // A render pass managed by the render target cache may be ended and resumed
  // at any time (to allow for things like copying and texture loading).
  VkRenderPass GetHostRenderTargetsRenderPass(RenderPassKey key);
  VkRenderPass GetFragmentShaderInterlockRenderPass() const {
    assert_true(GetPath() == Path::kPixelShaderInterlock);
    return fsi_render_pass_;
  }

  // Phase 2 motion guide
  // --------------------
  // A fifth host render target that the scene-phase draw writes and NR consumes,
  // owned here and by the command processor only: the render pass and the
  // per-framebuffer image live in this cache, the descriptor binding (2) and the
  // 64 MiB clip arena live in the command processor. Nothing about the guide is
  // visible to the guest or to EDRAM.

  // Turns the guide on or off. The command processor calls this once after device
  // initialization, from the same state that makes it report the guide as active
  // (motion_guide cvar plus VulkanDevice::SupportsMotionGuide). Returns whether
  // the guide is actually active; if it returns false, the command processor must
  // keep the guide inactive and report its own reason, because this cache will
  // never set motion_attachment_used. False here means the host render target
  // path is not the active one, or this device cannot use the attachment format
  // as a color attachment that can also be sampled. Requesting the guide while it
  // is not active creates no image and changes no key, so a guide-off run allocates
  // nothing.
  bool SetMotionGuideEnabled(bool enabled);
  bool motion_guide_enabled() const {
    return motion_guide_requested_ && path_ == Path::kHostRenderTargets &&
           motion_guide_format_supported_;
  }

  // Whether the draw being updated belongs to the scene phase, and thus must
  // write the guide. The cache cannot infer this: the guest's register state and
  // the shaders have the same shape for the scene, the HUD and full-screen
  // effects, so only the command processor, which owns the frame's phase, knows.
  // Sticky until changed, so it can be set once when the scene phase begins and
  // cleared once when it ends. Only takes effect while motion_guide_enabled() and
  // the pass has at least one guest attachment (with none, the framebuffer extent
  // is the no-attachment fallback and a guide image would be huge and unwritable).
  void SetMotionGuideSceneDraw(bool scene_draw);
  bool motion_guide_scene_draw() const { return motion_guide_scene_draw_; }

  // Clears a framebuffer's guide attachment at most once per guest frame, so the
  // guide starts each frame with w = 0 (a pixel no draw writes must be invalid,
  // not carry a previous frame's motion) while a pass that is ended and resumed
  // within the frame keeps what was written before the interruption. Call it
  // immediately after entering the render pass - or after vkCmdBeginRendering on
  // the dynamic rendering path - for the framebuffer just bound, inside the render
  // pass instance: the clear is recorded with vkCmdClearAttachments, which is the
  // only clear allowed inside a pass. A framebuffer without a guide, and a second
  // entry into the same framebuffer in the same frame, do nothing.
  void MaybeClearMotionAttachment(const Framebuffer* framebuffer);
  // Called by the command processor for every draw that will write the guide, so
  // the frame-end composite knows which framebuffers hold this frame's motion.
  void NotifyMotionDraw();
  // Whether any framebuffer received a motion-writing draw this frame, and thus
  // whether ResolveMotionGuideFrame has anything to composite. Lets the command
  // processor avoid opening a submission for an empty frame; false while the guide
  // is off, because NotifyMotionDraw records nothing then.
  bool motion_guide_frame_pending() const { return !motion_frame_framebuffers_.empty(); }

  // Phase 2 motion guide resolve - the guide's single-sample, 1000x600 form.
  // A frame's scene is split across render pass keys, so it is written into
  // several framebuffers, each with its own guide attachment at the host pass
  // extent (wider than the scene, and multisampled). NR needs one single-sample
  // 1000x600 RGBA16F image, so this composites every framebuffer that received a
  // motion draw this frame (`motion_frame_framebuffers_`, in first-draw order) at
  // the frame boundary - the swap, after every pass has ended and before the next
  // frame clears the attachments. One compute dispatch per framebuffer fetches
  // sample 0 of exactly the scene rectangle (x, y) - a crop of the attachment's
  // top-left corner, never a scale - and packs it as 1000*600 RGBA16F into a
  // device-local buffer. Sample 0, not a resolve: there is no image resolve or
  // copy command anywhere in this path, and the depth guide reads the same
  // sample.
  // The first dispatch overwrites the whole output, so its invalid pixels are
  // invalid; every later dispatch writes only where its own sample is valid, so
  // a later pass fills holes without erasing earlier motion. A buffer memory
  // barrier between dispatches makes the previous write visible/ordered.
  // The resolve must not run at the colour seam: there the guide may still be
  // the pre-scene clear, or only part of the scene, which is what made the dump
  // alternate between empty and partial.
  // Each framebuffer's guide image is transitioned
  // COLOR_ATTACHMENT_OPTIMAL -> SHADER_READ_ONLY -> COLOR_ATTACHMENT_OPTIMAL
  // around its dispatch, which is the layout every render pass with the guide
  // expects at both ends; the attachment is still valid at frame end and is
  // cleared at the next frame's pass entry. The command processor must have an
  // open submission and no open render pass when calling this - IssueSwap does
  // both.
  void ResolveMotionGuideFrame();

  VkFormat GetDepthVulkanFormat(xenos::DepthRenderTargetFormat format) const;
  VkFormat GetColorVulkanFormat(xenos::ColorRenderTargetFormat format) const;
  VkFormat GetColorOwnershipTransferVulkanFormat(
      xenos::ColorRenderTargetFormat format,
      xenos::MsaaSamples msaa_samples = xenos::MsaaSamples::k1X,
      bool* is_integer_out = nullptr) const;

 protected:
  uint32_t GetMaxRenderTargetWidth() const override;
  uint32_t GetMaxRenderTargetHeight() const override;

  RenderTarget* CreateRenderTarget(RenderTargetKey key) override;

  bool IsHostDepthEncodingDifferent(xenos::DepthRenderTargetFormat format) const override;

  bool IsGammaFormatHostStorageSeparate() const override;

  void RequestPixelShaderInterlockBarrier() override;

 private:
  enum class EdramBufferUsage {
    // There's no need for combined fragment and compute usages.
    // With host render targets, the usual usage sequence is as follows:
    // - Optionally compute writes - host depth copy storing for EDRAM range
    //   ownership transfers.
    // - Optionally fragment reads - host depth copy storing for EDRAM range
    //   ownership transfers.
    // - Compute writes - copying from host render targets during resolving.
    // - Compute reads - writing to the shared memory during resolving.
    // With the render backend implementation based on fragment shader
    // interlocks, it's:
    // - Fragment reads and writes - depth / stencil and color operations.
    // - Compute reads - writing to the shared memory during resolving.
    // So, fragment reads and compute reads normally don't follow each other,
    // and there's no need to amortize the cost of a read > read barrier in an
    // exceptional situation by using a wider barrier in the normal scenario.

    // Host depth copy storing.
    kFragmentRead,
    // Fragment shader interlock depth / stencil and color operations.
    kFragmentReadWrite,
    // Resolve - copying to the shared memory.
    kComputeRead,
    // Resolve - copying from host render targets.
    kComputeWrite,
    kTransferRead,
    kTransferWrite,
  };

  enum class EdramBufferModificationStatus {
    // The values are ordered by how strong the barrier conditions are.
    // No uncommitted shader writes.
    kUnmodified,
    // Need to commit before the next fragment shader interlock usage with
    // overlap.
    kViaFragmentShaderInterlock,
    // Need to commit before any next fragment shader interlock usage.
    kViaUnordered,
  };

  enum ResolveCopyDescriptorSet : uint32_t {
    // Never changes.
    kResolveCopyDescriptorSetEdram,
    // Shared memory or a region in it.
    kResolveCopyDescriptorSetDest,

    kResolveCopyDescriptorSetCount,
  };

  struct ResolveCopyShaderCode {
    const uint32_t* unscaled;
    size_t unscaled_size_bytes;
    const uint32_t* scaled;
    size_t scaled_size_bytes;
  };

  static void GetEdramBufferUsageMasks(EdramBufferUsage usage, VkPipelineStageFlags& stage_mask_out,
                                       VkAccessFlags& access_mask_out);
  bool IsColor16FormatFloatLike(xenos::ColorRenderTargetFormat format) const;
  void UseEdramBuffer(EdramBufferUsage new_usage);
  void MarkEdramBufferModified(EdramBufferModificationStatus modification_status =
                                   EdramBufferModificationStatus::kViaUnordered);
  void CommitEdramBufferShaderWrites(
      EdramBufferModificationStatus commit_status =
          EdramBufferModificationStatus::kViaFragmentShaderInterlock);

  VulkanCommandProcessor& command_processor_;

  Path path_ = Path::kHostRenderTargets;

  // Accessible in fragment and compute shaders.
  VkDescriptorSetLayout descriptor_set_layout_storage_buffer_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout_sampled_image_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout_sampled_image_x2_ = VK_NULL_HANDLE;

  std::unique_ptr<ui::vulkan::SingleLayoutDescriptorSetPool> descriptor_set_pool_sampled_image_;
  std::unique_ptr<ui::vulkan::SingleLayoutDescriptorSetPool> descriptor_set_pool_sampled_image_x2_;

  VkDeviceMemory edram_buffer_memory_ = VK_NULL_HANDLE;
  VkBuffer edram_buffer_ = VK_NULL_HANDLE;
  EdramBufferUsage edram_buffer_usage_;
  EdramBufferModificationStatus edram_buffer_modification_status_ =
      EdramBufferModificationStatus::kUnmodified;
  VkDescriptorPool edram_storage_buffer_descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet edram_storage_buffer_descriptor_set_;

  VkPipelineLayout resolve_copy_pipeline_layout_ = VK_NULL_HANDLE;
  static const ResolveCopyShaderCode
      kResolveCopyShaders[size_t(draw_util::ResolveCopyShaderIndex::kCount)];
  std::array<VkPipeline, size_t(draw_util::ResolveCopyShaderIndex::kCount)>
      resolve_copy_pipelines_{};

  // On the fragment shader interlock path, the render pass key is used purely
  // for passing parameters to pipeline setup - there's always only one render
  // pass.
  RenderPassKey last_update_render_pass_key_;
  VkRenderPass last_update_render_pass_ = VK_NULL_HANDLE;
  // The pitch is not used on the fragment shader interlock path.
  uint32_t last_update_framebuffer_pitch_tiles_at_32bpp_ = 0;
  // The attachments are not used on the fragment shader interlock path.
  const RenderTarget* const*
      last_update_framebuffer_attachments_[1 + xenos::kMaxColorRenderTargets] = {};
  const Framebuffer* last_update_framebuffer_ = VK_NULL_HANDLE;

  // For host render targets.

  // Can only be destroyed when framebuffers referencing it are destroyed!
  class VulkanRenderTarget final : public RenderTarget {
   public:
    static constexpr VkPipelineStageFlags kColorDrawStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    static constexpr VkAccessFlags kColorDrawAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    static constexpr VkImageLayout kColorDrawLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    static constexpr VkPipelineStageFlags kDepthDrawStageMask =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    static constexpr VkAccessFlags kDepthDrawAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    static constexpr VkImageLayout kDepthDrawLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    // Takes ownership of the Vulkan objects passed to the constructor.
    VulkanRenderTarget(RenderTargetKey key, VulkanRenderTargetCache& render_target_cache,
                       VkImage image, VkDeviceMemory memory, VkImageView view_depth_color,
                       VkImageView view_depth_stencil, VkImageView view_stencil,
                       VkImageView view_srgb, VkImageView view_color_transfer_separate,
                       size_t descriptor_set_index_transfer_source)
        : RenderTarget(key),
          render_target_cache_(render_target_cache),
          image_(image),
          memory_(memory),
          view_depth_color_(view_depth_color),
          view_depth_stencil_(view_depth_stencil),
          view_stencil_(view_stencil),
          view_srgb_(view_srgb),
          view_color_transfer_separate_(view_color_transfer_separate),
          descriptor_set_index_transfer_source_(descriptor_set_index_transfer_source) {}
    ~VulkanRenderTarget();

    VkImage image() const { return image_; }

    VkImageView view_depth_color() const { return view_depth_color_; }
    VkImageView view_depth_stencil() const { return view_depth_stencil_; }
    VkImageView view_color_transfer_separate() const { return view_color_transfer_separate_; }
    VkImageView view_color_transfer() const {
      return view_color_transfer_separate_ != VK_NULL_HANDLE ? view_color_transfer_separate_
                                                             : view_depth_color_;
    }
    VkDescriptorSet GetDescriptorSetTransferSource() const {
      ui::vulkan::SingleLayoutDescriptorSetPool& descriptor_set_pool =
          key().is_depth ? *render_target_cache_.descriptor_set_pool_sampled_image_x2_
                         : *render_target_cache_.descriptor_set_pool_sampled_image_;
      return descriptor_set_pool.Get(descriptor_set_index_transfer_source_);
    }

    static void GetDrawUsage(bool is_depth, VkPipelineStageFlags* stage_mask_out,
                             VkAccessFlags* access_mask_out, VkImageLayout* layout_out) {
      if (stage_mask_out) {
        *stage_mask_out = is_depth ? kDepthDrawStageMask : kColorDrawStageMask;
      }
      if (access_mask_out) {
        *access_mask_out = is_depth ? kDepthDrawAccessMask : kColorDrawAccessMask;
      }
      if (layout_out) {
        *layout_out = is_depth ? kDepthDrawLayout : kColorDrawLayout;
      }
    }
    void GetDrawUsage(VkPipelineStageFlags* stage_mask_out, VkAccessFlags* access_mask_out,
                      VkImageLayout* layout_out) const {
      GetDrawUsage(key().is_depth, stage_mask_out, access_mask_out, layout_out);
    }
    VkPipelineStageFlags current_stage_mask() const { return current_stage_mask_; }
    VkAccessFlags current_access_mask() const { return current_access_mask_; }
    VkImageLayout current_layout() const { return current_layout_; }
    void SetUsage(VkPipelineStageFlags stage_mask, VkAccessFlags access_mask,
                  VkImageLayout layout) {
      current_stage_mask_ = stage_mask;
      current_access_mask_ = access_mask;
      current_layout_ = layout;
    }

    uint32_t temporary_sort_index() const { return temporary_sort_index_; }
    void SetTemporarySortIndex(uint32_t index) { temporary_sort_index_ = index; }

   private:
    VulkanRenderTargetCache& render_target_cache_;

    VkImage image_;
    VkDeviceMemory memory_;

    // TODO(Triang3l): Per-format drawing views for mutable formats with EDRAM
    // aliasing without transfers.
    VkImageView view_depth_color_;
    // Optional views.
    VkImageView view_depth_stencil_;
    VkImageView view_stencil_;
    VkImageView view_srgb_;
    VkImageView view_color_transfer_separate_;

    // 2 sampled images for depth / stencil, 1 sampled image for color.
    size_t descriptor_set_index_transfer_source_;

    VkPipelineStageFlags current_stage_mask_ = 0;
    VkAccessFlags current_access_mask_ = 0;
    VkImageLayout current_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    // Temporary storage for indices in operations like transfers and dumps.
    uint32_t temporary_sort_index_ = 0;
  };

  struct FramebufferKey {
    RenderPassKey render_pass_key;

    // Same as RenderTargetKey::pitch_tiles_at_32bpp.
    uint32_t pitch_tiles_at_32bpp : 8;  // 8
    // [0, 2047].
    uint32_t depth_base_tiles : xenos::kEdramBaseTilesBits;    // 19
    uint32_t color_0_base_tiles : xenos::kEdramBaseTilesBits;  // 30

    uint32_t color_1_base_tiles : xenos::kEdramBaseTilesBits;  // 43
    uint32_t color_2_base_tiles : xenos::kEdramBaseTilesBits;  // 54

    uint32_t color_3_base_tiles : xenos::kEdramBaseTilesBits;  // 75

    // Including all the padding, for a stable hash.
    FramebufferKey() { Reset(); }
    FramebufferKey(const FramebufferKey& key) { std::memcpy(this, &key, sizeof(*this)); }
    FramebufferKey& operator=(const FramebufferKey& key) {
      std::memcpy(this, &key, sizeof(*this));
      return *this;
    }
    bool operator==(const FramebufferKey& key) const {
      return std::memcmp(this, &key, sizeof(*this)) == 0;
    }
    using Hasher = rex::XXHasher<FramebufferKey>;
    void Reset() { std::memset(this, 0, sizeof(*this)); }
  };

  enum TransferUsedDescriptorSet : uint32_t {
    // Ordered from the least to the most frequently changed.
    kTransferUsedDescriptorSetHostDepthBuffer,
    kTransferUsedDescriptorSetHostDepthStencilTextures,
    kTransferUsedDescriptorSetDepthStencilTextures,
    // Mutually exclusive with kTransferUsedDescriptorSetDepthStencilTextures.
    kTransferUsedDescriptorSetColorTexture,

    kTransferUsedDescriptorSetCount,

    kTransferUsedDescriptorSetHostDepthBufferBit = uint32_t(1)
                                                   << kTransferUsedDescriptorSetHostDepthBuffer,
    kTransferUsedDescriptorSetHostDepthStencilTexturesBit =
        uint32_t(1) << kTransferUsedDescriptorSetHostDepthStencilTextures,
    kTransferUsedDescriptorSetDepthStencilTexturesBit =
        uint32_t(1) << kTransferUsedDescriptorSetDepthStencilTextures,
    kTransferUsedDescriptorSetColorTextureBit = uint32_t(1)
                                                << kTransferUsedDescriptorSetColorTexture,
  };

  // 32-bit push constants (for simplicity of size calculation and to avoid
  // std140 packing issues).
  enum TransferUsedPushConstantDword : uint32_t {
    kTransferUsedPushConstantDwordHostDepthAddress,
    kTransferUsedPushConstantDwordAddress,
    // Changed 8 times per transfer.
    kTransferUsedPushConstantDwordStencilMask,

    kTransferUsedPushConstantDwordCount,

    kTransferUsedPushConstantDwordHostDepthAddressBit =
        uint32_t(1) << kTransferUsedPushConstantDwordHostDepthAddress,
    kTransferUsedPushConstantDwordAddressBit = uint32_t(1) << kTransferUsedPushConstantDwordAddress,
    kTransferUsedPushConstantDwordStencilMaskBit = uint32_t(1)
                                                   << kTransferUsedPushConstantDwordStencilMask,
  };

  enum class TransferPipelineLayoutIndex {
    kColor,
    kDepth,
    kColorToStencilBit,
    kDepthToStencilBit,
    kColorAndHostDepthTexture,
    kColorAndHostDepthBuffer,
    kDepthAndHostDepthTexture,
    kDepthAndHostDepthBuffer,

    kCount,
  };

  struct TransferPipelineLayoutInfo {
    uint32_t used_descriptor_sets;
    uint32_t used_push_constant_dwords;
  };

  static const TransferPipelineLayoutInfo
      kTransferPipelineLayoutInfos[size_t(TransferPipelineLayoutIndex::kCount)];

  enum class TransferMode : uint32_t {
    kColorToDepth,
    kColorToColor,

    kDepthToDepth,
    kDepthToColor,

    kColorToStencilBit,
    kDepthToStencilBit,

    // Two-source modes, using the host depth if it, when converted to the guest
    // format, matches what's in the owner source (not modified, keep host
    // precision), or the guest data otherwise (significantly modified, possibly
    // cleared). Stencil for FragStencilRef is always taken from the guest
    // source.

    kColorAndHostDepthToDepth,
    // When using different source and destination depth formats.
    kDepthAndHostDepthToDepth,

    // If host depth is fetched, but it's the same image as the destination,
    // it's copied to the EDRAM buffer (but since it's just a scratch buffer,
    // with tiles laid out linearly with the same pitch as in the original
    // render target; also no swapping of 40-sample columns as opposed to the
    // host render target - this is done only for the color source) and fetched
    // from there instead of the host depth texture.
    kColorAndHostDepthCopyToDepth,
    kDepthAndHostDepthCopyToDepth,

    kCount,
  };

  enum class TransferOutput {
    kColor,
    kDepth,
    kStencilBit,
  };

  struct TransferModeInfo {
    TransferOutput output;
    TransferPipelineLayoutIndex pipeline_layout;
  };

  static const TransferModeInfo kTransferModes[size_t(TransferMode::kCount)];

  union TransferShaderKey {
    uint32_t key;
    struct {
      xenos::MsaaSamples dest_msaa_samples : xenos::kMsaaSamplesBits;
      uint32_t dest_color_rt_index : xenos::kColorRenderTargetIndexBits;
      uint32_t dest_resource_format : xenos::kRenderTargetFormatBits;
      xenos::MsaaSamples source_msaa_samples : xenos::kMsaaSamplesBits;
      // Always 1x when the host depth is a copy from a buffer rather than an
      // image, not to create the same pipeline for different MSAA sample counts
      // as it doesn't matter in this case.
      xenos::MsaaSamples host_depth_source_msaa_samples : xenos::kMsaaSamplesBits;
      uint32_t source_resource_format : xenos::kRenderTargetFormatBits;

      // Last bits because this affects the pipeline layout - after sorting,
      // only change it as fewer times as possible. Depth buffers have an
      // additional stencil texture.
      static_assert(size_t(TransferMode::kCount) <= (size_t(1) << 4));
      TransferMode mode : 4;
    };

    TransferShaderKey() : key(0) { static_assert_size(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const TransferShaderKey& key) const {
        return std::hash<uint32_t>{}(key.key);
      }
    };
    bool operator==(const TransferShaderKey& other_key) const { return key == other_key.key; }
    bool operator!=(const TransferShaderKey& other_key) const { return !(*this == other_key); }
    bool operator<(const TransferShaderKey& other_key) const { return key < other_key.key; }
  };

  struct TransferPipelineKey {
    RenderPassKey render_pass_key;
    TransferShaderKey shader_key;

    TransferPipelineKey(RenderPassKey render_pass_key, TransferShaderKey shader_key)
        : render_pass_key(render_pass_key), shader_key(shader_key) {}

    struct Hasher {
      size_t operator()(const TransferPipelineKey& key) const {
        XXH3_state_t hash_state;
        XXH3_64bits_reset(&hash_state);
        XXH3_64bits_update(&hash_state, &key.render_pass_key, sizeof(key.render_pass_key));
        XXH3_64bits_update(&hash_state, &key.shader_key, sizeof(key.shader_key));
        return static_cast<size_t>(XXH3_64bits_digest(&hash_state));
      }
    };
    bool operator==(const TransferPipelineKey& other_key) const {
      return render_pass_key == other_key.render_pass_key && shader_key == other_key.shader_key;
    }
    bool operator!=(const TransferPipelineKey& other_key) const { return !(*this == other_key); }
    bool operator<(const TransferPipelineKey& other_key) const {
      if (render_pass_key != other_key.render_pass_key) {
        return render_pass_key < other_key.render_pass_key;
      }
      return shader_key < other_key.shader_key;
    }
  };

  union TransferAddressConstant {
    uint32_t constant;
    struct {
      // All in tiles.
      uint32_t dest_pitch : xenos::kEdramPitchTilesBits;
      uint32_t source_pitch : xenos::kEdramPitchTilesBits;
      // Destination base in tiles minus source base in tiles (not vice versa
      // because this is a transform of the coordinate system, not addresses
      // themselves).
      // + 1 bit because this is a signed difference between two EDRAM bases.
      // 0 for host_depth_source_is_copy (ignored in this case anyway as
      // destination == source anyway).
      int32_t source_to_dest : xenos::kEdramBaseTilesBits + 1;
    };
    TransferAddressConstant() : constant(0) { static_assert_size(*this, sizeof(constant)); }
    bool operator==(const TransferAddressConstant& other_constant) const {
      return constant == other_constant.constant;
    }
    bool operator!=(const TransferAddressConstant& other_constant) const {
      return !(*this == other_constant);
    }
  };

  struct TransferInvocation {
    Transfer transfer;
    TransferShaderKey shader_key;
    TransferInvocation(const Transfer& transfer, const TransferShaderKey& shader_key)
        : transfer(transfer), shader_key(shader_key) {}
    bool operator<(const TransferInvocation& other_invocation) const {
      // TODO(Triang3l): See if it may be better to sort by the source in the
      // first place, especially when reading the same data multiple times (like
      // to write the stencil bits after depth) for better read locality.
      // Sort by the shader key primarily to reduce pipeline state (context)
      // switches.
      if (shader_key != other_invocation.shader_key) {
        return shader_key < other_invocation.shader_key;
      }
      // Host depth render targets are changed rarely if they exist, won't save
      // many binding changes, ignore them for simplicity (their existence is
      // caught by the shader key change).
      assert_not_null(transfer.source);
      assert_not_null(other_invocation.transfer.source);
      uint32_t source_index =
          static_cast<const VulkanRenderTarget*>(transfer.source)->temporary_sort_index();
      uint32_t other_source_index =
          static_cast<const VulkanRenderTarget*>(other_invocation.transfer.source)
              ->temporary_sort_index();
      if (source_index != other_source_index) {
        return source_index < other_source_index;
      }
      return transfer.start_tiles < other_invocation.transfer.start_tiles;
    }
    bool CanBeMergedIntoOneDraw(const TransferInvocation& other_invocation) const {
      return shader_key == other_invocation.shader_key &&
             transfer.AreSourcesSame(other_invocation.transfer);
    }
  };

  union DumpPipelineKey {
    uint32_t key;
    struct {
      xenos::MsaaSamples msaa_samples : 2;
      uint32_t resource_format : 4;
      // Last bit because this affects the pipeline - after sorting, only change
      // it at most once. Depth buffers have an additional stencil SRV.
      uint32_t is_depth : 1;
    };

    DumpPipelineKey() : key(0) { static_assert_size(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const DumpPipelineKey& key) const { return std::hash<uint32_t>{}(key.key); }
    };
    bool operator==(const DumpPipelineKey& other_key) const { return key == other_key.key; }
    bool operator!=(const DumpPipelineKey& other_key) const { return !(*this == other_key); }
    bool operator<(const DumpPipelineKey& other_key) const { return key < other_key.key; }

    xenos::ColorRenderTargetFormat GetColorFormat() const {
      assert_false(is_depth);
      return xenos::ColorRenderTargetFormat(resource_format);
    }
    xenos::DepthRenderTargetFormat GetDepthFormat() const {
      assert_true(is_depth);
      return xenos::DepthRenderTargetFormat(resource_format);
    }
  };

  // There's no strict dependency on the group size in dumping, for simplicity
  // calculations especially with resolution scaling, dividing manually (as the
  // group size is not unlimited). The only restriction is that an integer
  // multiple of it must be 80x16 samples (and no larger than that) for 32bpp,
  // or 40x16 samples for 64bpp (because only a half of the pair of tiles may
  // need to be dumped). Using 8x16 since that's 128 - the minimum required
  // group size on Vulkan, and the maximum number of lanes in a subgroup on
  // Vulkan.
  static constexpr uint32_t kDumpSamplesPerGroupX = 8;
  static constexpr uint32_t kDumpSamplesPerGroupY = 16;

  union DumpPitches {
    uint32_t pitches;
    struct {
      // Both in tiles.
      uint32_t dest_pitch : xenos::kEdramPitchTilesBits;
      uint32_t source_pitch : xenos::kEdramPitchTilesBits;
    };
    DumpPitches() : pitches(0) { static_assert_size(*this, sizeof(pitches)); }
    bool operator==(const DumpPitches& other_pitches) const {
      return pitches == other_pitches.pitches;
    }
    bool operator!=(const DumpPitches& other_pitches) const { return !(*this == other_pitches); }
  };

  union DumpOffsets {
    uint32_t offsets;
    struct {
      // May be beyond the EDRAM tile count in case of EDRAM addressing
      // wrapping, thus + 1 bit.
      uint32_t dispatch_first_tile : xenos::kEdramBaseTilesBits + 1;
      uint32_t source_base_tiles : xenos::kEdramBaseTilesBits;
    };
    DumpOffsets() : offsets(0) { static_assert_size(*this, sizeof(offsets)); }
    bool operator==(const DumpOffsets& other_offsets) const {
      return offsets == other_offsets.offsets;
    }
    bool operator!=(const DumpOffsets& other_offsets) const { return !(*this == other_offsets); }
  };

  enum DumpDescriptorSet : uint32_t {
    // Never changes. Same in both color and depth pipeline layouts, keep the
    // first for pipeline layout compatibility, to only have to set it once.
    kDumpDescriptorSetEdram,
    // One resolve may need multiple sources. Different descriptor set layouts
    // for color and depth.
    kDumpDescriptorSetSource,

    kDumpDescriptorSetCount,
  };

  enum DumpPushConstant : uint32_t {
    // May be different for different sources.
    kDumpPushConstantPitches,
    // May be changed multiple times for the same source.
    kDumpPushConstantOffsets,

    kDumpPushConstantCount,
  };

  struct DumpInvocation {
    ResolveCopyDumpRectangle rectangle;
    DumpPipelineKey pipeline_key;
    DumpInvocation(const ResolveCopyDumpRectangle& rectangle, const DumpPipelineKey& pipeline_key)
        : rectangle(rectangle), pipeline_key(pipeline_key) {}
    bool operator<(const DumpInvocation& other_invocation) const {
      // Sort by the pipeline key primarily to reduce pipeline state (context)
      // switches.
      if (pipeline_key != other_invocation.pipeline_key) {
        return pipeline_key < other_invocation.pipeline_key;
      }
      assert_not_null(rectangle.render_target);
      uint32_t render_target_index =
          static_cast<const VulkanRenderTarget*>(rectangle.render_target)->temporary_sort_index();
      const ResolveCopyDumpRectangle& other_rectangle = other_invocation.rectangle;
      uint32_t other_render_target_index =
          static_cast<const VulkanRenderTarget*>(other_rectangle.render_target)
              ->temporary_sort_index();
      if (render_target_index != other_render_target_index) {
        return render_target_index < other_render_target_index;
      }
      if (rectangle.row_first != other_rectangle.row_first) {
        return rectangle.row_first < other_rectangle.row_first;
      }
      return rectangle.row_first_start < other_rectangle.row_first_start;
    }
  };

  struct DirectResolvePushConstants {
    draw_util::ResolveCopyShaderConstants resolve;
    uint32_t source_base_tiles;
    uint32_t source_pitch_tiles;
    uint32_t dispatch_first_tile;
  };

  struct DirectResolvePipelineKey {
    DumpPipelineKey dump_pipeline_key;
    draw_util::ResolveCopyShaderIndex copy_shader;
    bool draw_resolution_scaled;
    uint64_t packed() const {
      return uint64_t(dump_pipeline_key.key) | (uint64_t(size_t(copy_shader)) << 32) |
             (uint64_t(draw_resolution_scaled ? 1 : 0) << 40);
    }
    struct Hasher {
      size_t operator()(const DirectResolvePipelineKey& key) const {
        return std::hash<uint64_t>{}(key.packed());
      }
    };
    bool operator==(const DirectResolvePipelineKey& other_key) const {
      return packed() == other_key.packed();
    }
  };

  // Returns the framebuffer object, or VK_NULL_HANDLE if failed to create.
  const Framebuffer* GetHostRenderTargetsFramebuffer(
      RenderPassKey render_pass_key, uint32_t pitch_tiles_at_32bpp,
      const RenderTarget* const* depth_and_color_render_targets);

  // Host sample count of a pass key - the same value its render pass and its
  // framebuffer are built with, so the motion guide attachment can never disagree
  // with the pass it is attached to. 2x emulated as 4x is the only case where it
  // differs from the guest's own count. VK_SAMPLE_COUNT_FLAG_BITS_MAX_ENUM if the
  // guest count is not supported.
  VkSampleCountFlagBits GetHostRenderTargetsSampleCount(RenderPassKey key) const;
  // Destroys the guide image, memory and view of a framebuffer, if any. Called by
  // ~Framebuffer, so every path that destroys a framebuffer - ClearCache,
  // Shutdown, a failed creation - releases the guide too.
  void DestroyMotionGuideAttachment(Framebuffer& framebuffer);

  VkShaderModule GetTransferShader(TransferShaderKey key);
  // With sample-rate shading, returns a pointer to one pipeline. Without
  // sample-rate shading, returns a pointer to as many pipelines as there are
  // samples. If there was a failure to create a pipeline, returns nullptr.
  VkPipeline const* GetTransferPipelines(TransferPipelineKey key);

  // Do ownership transfers for render targets - each render target / vector may
  // be null / empty in case there's nothing to do for them.
  // resolve_clear_rectangle is expected to be provided by
  // PrepareHostRenderTargetsResolveClear which should do all the needed size
  // bound checks.
  void PerformTransfersAndResolveClears(
      uint32_t render_target_count, RenderTarget* const* render_targets,
      const std::vector<Transfer>* render_target_transfers,
      const uint64_t* render_target_resolve_clear_values = nullptr,
      const Transfer::Rectangle* resolve_clear_rectangle = nullptr);

  VkPipeline GetDumpPipeline(DumpPipelineKey key);
  VkPipeline GetDirectResolvePipeline(DirectResolvePipelineKey key);
  bool TryResolveCopyDirectly(const draw_util::ResolveInfo& resolve_info,
                              draw_util::ResolveCopyShaderIndex copy_shader,
                              bool draw_resolution_scaled);

  // Writes contents of host render targets within rectangles from
  // ResolveInfo::GetCopyEdramTileSpan to edram_buffer_.
  bool DumpRenderTargets(uint32_t dump_base, uint32_t dump_row_length_used, uint32_t dump_rows,
                         uint32_t dump_pitch);

  // Shared capture machinery for a range the guest resolved into shared memory,
  // used by both the depth probe's guest source and the colour probe. The range
  // is copied into a device-local staging buffer and then into a host-visible
  // readback buffer of the same size, and the CPU detiles it from there. One
  // instance per probe, so the two can never share - and overwrite - a buffer.
  struct GuestRangeCapture {
    VkBuffer staging_buffer = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    VkBuffer readback_buffer = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory = VK_NULL_HANDLE;
    void* readback_mapped = nullptr;
    VkDeviceSize size = 0;
    // A failed allocation is not retried: the range either fits or the device
    // cannot support the probe, and retrying inside every subsequent resolve
    // would churn the allocator for no benefit.
    bool alloc_failed = false;
  };

  // Development-only writeback probe (cvar wb_probe, off by default).
  // Copies a resolved guest colour range out to a scratch buffer and back twice,
  // then compares the three snapshots on the CPU after the submission completes.
  // In the default identity mode the scratch holds the resolved bytes, so a
  // byte-exact copy must leave the frame structurally identical. In visible mode
  // (wb_probe_mode=1) the scratch holds an opaque pattern instead, one colour per
  // (destination address, resolve sequence) of the frame, which must show up in
  // the final composited frame while the HUD drawn after the resolve must not.
  void MaybeRunWritebackProbe(VulkanSharedMemory& shared_memory, VulkanTextureCache& texture_cache,
                              uint32_t address, uint32_t length, uint32_t width_div_8,
                              uint32_t height_div_8, bool is_copying_depth,
                              bool draw_resolution_scaled, xenos::ColorFormat copy_dest_format);
  void VerifyWritebackProbe();
  bool EnsureWritebackProbeBuffers(VkDeviceSize slice_size);
  void DestroyWritebackProbeBuffers();

  // Development-only scene depth capture probe (cvar depth_probe, off by default).
  // depth_probe_source selects the surface; both sources produce the same
  // 1000x600 float32 layout, so the statistics, the hash and the dump file are
  // identical in format.
  // - 0 (default): the guest's own 1000x600 kD24S8 depth resolve. At the depth
  //   resolve the resolved range is copied out of guest memory into a device
  //   local staging buffer and then into a host-visible readback buffer, and the
  //   tiled 32-bit words are detiled and decoded on the CPU. This is the seam
  //   validated against an independent capture; the host path is not excluded by
  //   its size (the scene's 13-tile 2x depth target is 1040 px wide), only
  //   untested, because no host dump has been taken.
  // - 1: the host depth render target at the colour resolve, read with a compute
  //   shader (one MSAA sample). A candidate, not a proven source: it has never
  //   been dumped, and the only retained host dump was a half-cleared buffer.
  // Copy-out only for both: neither source writes the depth image, guest memory
  // or the EDRAM buffer; the host source restores its usage triple exactly.
  void MaybeRunDepthProbe(VulkanSharedMemory& shared_memory,
                          const draw_util::ResolveInfo& resolve_info,
                          bool draw_resolution_scaled);
  void VerifyDepthProbe();
  bool EnsureDepthProbeResources();
  // Development-only native pre-HUD scene colour capture (cvar colour_probe, off
  // by default). At the first 1000x600 colour resolve of a guest frame the
  // resolved range - a tiled guest RGBA8 texture holding the scene at the pre-HUD
  // instant - is copied out with the shared GuestRangeCapture machinery, detiled
  // on the CPU and dumped as a 1000x600 row-major RGBA8 rectangle. Read-only with
  // respect to guest memory: nothing is written back and no ownership is claimed.
  void MaybeRunColourProbe(VulkanSharedMemory& shared_memory,
                           const draw_util::ResolveInfo& resolve_info,
                           bool draw_resolution_scaled);
  void VerifyColourProbe();
  void DestroyColourProbeResources();
  // Shared by the guest depth source and the colour probe. Lazily created from the
  // first resolved range and refused rather than resized if a later range is
  // larger - this runs inside an open submission, where the queue cannot be
  // drained to replace a buffer safely. `label` names the capture in the
  // allocation error, so a failure still says which probe broke.
  bool EnsureGuestRangeCapture(GuestRangeCapture& capture, VkDeviceSize size, const char* label);
  // Records the shared-memory read barrier and the two copies that get `length`
  // bytes at `start` into the capture's host-visible readback buffer.
  void RecordGuestRangeCopyOut(GuestRangeCapture& capture, VulkanSharedMemory& shared_memory,
                               uint32_t start, VkDeviceSize length);
  // Invalidates the mapped readback and returns its bytes, or null.
  const uint8_t* MapGuestRangeCapture(GuestRangeCapture& capture);
  void DestroyGuestRangeCapture(GuestRangeCapture& capture);
  void DestroyDepthProbeResources();
  // host_sample_count_log2 is uint32_t(xenos::MsaaSamples) except for 2x emulated
  // as 4x, which is 2. Depth cannot be resolved, so sample count is part of the
  // pipeline key (a non-multisampled image has no sample operand to fetch from).
  VkPipeline GetDepthProbePipeline(uint32_t host_sample_count_log2);

  // Phase 2 motion guide dump.
  // Writes the pending dump as `<prefix>m<frame>.bin`, exactly 4800000 bytes of
  // RGBA16F little-endian, row-major [y][x], top-left origin, with no header - the
  // contract tools/motion_guide_analyze.py reads. The `m` is appended here, not
  // expected from the prefix, so the analyzer's file-name pattern always holds.
  // Called from CompletedSubmissionUpdated, so the mapped readback is only read
  // after the GPU finished writing it.
  void VerifyMotionGuideDump();
  // Lazily created on the first resolve, freed by Shutdown and ClearCache, and
  // recreated on the next resolve after that - which is also how a device loss is
  // recovered from, since the cache is shut down and initialized again. A failure
  // is remembered so a device that cannot support the resolve is not asked on
  // every frame.
  bool EnsureMotionGuideResources();
  // Allocates and writes the per-framebuffer descriptor set on first use. Only
  // the framebuffer's mutable cache field is modified.
  bool EnsureMotionGuideDescriptorSet(const Framebuffer& framebuffer);
  void DestroyMotionGuideResources();

  // Phase 2 motion guide state (see the public API). The format support is a
  // device fact from Initialize; the request is the command processor's; the
  // guide is only active when both hold and the host render target path is in
  // use, which is what keeps motion_attachment_used out of every other key.
  bool motion_guide_requested_ = false;
  bool motion_guide_format_supported_ = false;
  bool motion_guide_scene_draw_ = false;

  bool gamma_render_target_as_unorm16_ = false;

  bool depth_unorm24_vulkan_format_supported_ = false;
  bool depth_float24_round_ = false;
  bool depth_float24_convert_in_pixel_shader_ = false;

  bool msaa_2x_attachments_supported_ = false;
  bool msaa_2x_no_attachments_supported_ = false;
  bool color_16bit_transfer_uint_formats_supported_ = true;
  bool color_32bit_transfer_uint_formats_supported_ = true;
  bool color_rg16_draw_format_fallback_to_float_ = false;
  bool color_rgba16_draw_format_fallback_to_float_ = false;

  // VK_NULL_HANDLE if failed to create.
  std::unordered_map<RenderPassKey, VkRenderPass, RenderPassKey::Hasher> render_passes_;

  std::unordered_map<FramebufferKey, Framebuffer, FramebufferKey::Hasher> framebuffers_;

  // Set 0 - EDRAM storage buffer, set 1 - source depth sampled image (and
  // unused stencil from the transfer descriptor set), HostDepthStoreConstants
  // passed via push constants.
  VkPipelineLayout host_depth_store_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k4X) + 1] = {};

  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> transfer_vertex_buffer_pool_;
  VkShaderModule transfer_passthrough_vertex_shader_ = VK_NULL_HANDLE;
  VkPipelineLayout transfer_pipeline_layouts_[size_t(TransferPipelineLayoutIndex::kCount)] = {};
  // VK_NULL_HANDLE if failed to create.
  std::unordered_map<TransferShaderKey, VkShaderModule, TransferShaderKey::Hasher>
      transfer_shaders_;
  // With sample-rate shading, one pipeline per entry. Without sample-rate
  // shading, one pipeline per sample per entry. VK_NULL_HANDLE if failed to
  // create.
  std::unordered_map<TransferPipelineKey, std::array<VkPipeline, 4>, TransferPipelineKey::Hasher>
      transfer_pipelines_;

  VkPipelineLayout dump_pipeline_layout_color_ = VK_NULL_HANDLE;
  VkPipelineLayout dump_pipeline_layout_depth_ = VK_NULL_HANDLE;
  // Compute pipelines for copying host render target contents to the EDRAM
  // buffer. VK_NULL_HANDLE if failed to create.
  std::unordered_map<DumpPipelineKey, VkPipeline, DumpPipelineKey::Hasher> dump_pipelines_;
  VkPipelineLayout direct_resolve_pipeline_layout_color_ = VK_NULL_HANDLE;
  VkPipelineLayout direct_resolve_pipeline_layout_depth_ = VK_NULL_HANDLE;
  std::unordered_map<DirectResolvePipelineKey, VkPipeline, DirectResolvePipelineKey::Hasher>
      direct_resolve_pipelines_;

  // Temporary storage for Resolve.
  std::vector<Transfer> clear_transfers_[2];

  // Temporary storage for PerformTransfersAndResolveClears.
  std::vector<TransferInvocation> current_transfer_invocations_;

  // Temporary storage for DumpRenderTargets.
  std::vector<ResolveCopyDumpRectangle> dump_rectangles_;
  std::vector<DumpInvocation> dump_invocations_;
  std::vector<ResolveCopyDispatch> direct_resolve_dispatches_;
  uint64_t direct_resolve_attempt_count_ = 0;
  uint64_t direct_resolve_success_count_ = 0;
  uint64_t direct_resolve_fallback_count_ = 0;

  // Development-only identity writeback probe (cvar wb_probe).
  VkBuffer probe_scratch_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory probe_scratch_memory_ = VK_NULL_HANDLE;
  // Host-visible pattern source for wb_probe_mode=1, uploaded once: one fill per
  // palette entry (one per address slot and resolve sequence), as R8G8B8A8 bytes.
  VkBuffer probe_pattern_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory probe_pattern_memory_ = VK_NULL_HANDLE;
  void* probe_pattern_mapped_ = nullptr;
  // One-shot reporting so a refused format or the chosen fill is visible once
  // instead of per frame.
  bool probe_layout_logged_ = false;
  bool probe_visible_format_logged_ = false;
  // Host-visible buffer holding three consecutive snapshots of the same guest
  // range: before the round trip, after the first copy-back, after the second.
  VkBuffer probe_readback_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory probe_readback_memory_ = VK_NULL_HANDLE;
  void* probe_readback_mapped_ = nullptr;
  VkDeviceSize probe_readback_size_ = 0;
  VkDeviceSize probe_slice_size_ = 0;
  // One probe per guest frame, and none while a recorded probe still needs its
  // CPU-side comparison, so the single readback buffer is never overwritten
  // before the GPU has finished writing a previous snapshot into it.
  bool probe_pending_ = false;
  uint32_t probe_pending_frame_ = 0;
  uint32_t probe_pending_address_ = 0;
  uint32_t probe_pending_length_ = 0;
  uint32_t probe_last_frame_ = UINT32_MAX;
  uint32_t probe_frames_done_ = 0;
  // Visible mode paints one colour per (destination address, resolve sequence).
  // Four addresses and at most three resolves of each is the observed frame shape
  // (10 resolves, 4 addresses, 3/1/3/3), so the palette holds 12 entries and a
  // colour is address_slot * kWritebackProbeAddressSequences + sequence.
  static constexpr uint32_t kWritebackProbeAddressSlots = 4;
  static constexpr uint32_t kWritebackProbeAddressSequences = 3;
  // Destination address slots and their per-frame resolve sequence counters, used
  // to give every paint its own colour, plus where the pending probe's colour
  // starts in the pattern buffer.
  uint32_t probe_color_frame_ = UINT32_MAX;
  uint32_t probe_color_address_count_ = 0;
  uint32_t probe_color_addresses_[kWritebackProbeAddressSlots] = {};
  uint32_t probe_color_address_sequences_[kWritebackProbeAddressSlots] = {};
  VkDeviceSize probe_pending_pattern_offset_ = 0;
  // Bounded per-frame diagnostic for the probe window.
  uint32_t probe_count_frame_ = UINT32_MAX;
  uint32_t probe_frame_resolves_ = 0;
  // Bounded reporting of colour resolves the extent filter rejected, so a filter
  // that never matches is visible instead of silent.
  uint32_t probe_skipped_extent_logs_ = 0;
  uint32_t probe_skipped_unaligned_logs_ = 0;
  bool probe_alloc_failed_logged_ = false;

  // Development-only scene depth capture probe (cvar depth_probe).
  // The depth render target the current frame last drew into. Remembered from
  // Update() because at the resolve the most recent draw may have had no depth
  // attachment at all, so the framebuffer's depth pointer says nothing about
  // which depth image still holds the scene. Nulled whenever render targets are
  // destroyed - a dangling pointer here would read freed memory.
  RenderTarget* depth_probe_target_ = nullptr;
  // Guest frame `depth_probe_target_` was adopted on, so only the first depth
  // target of a frame is taken. Measured: adopting every update picks up a
  // later effect pass and reads a cleared surface instead of the scene.
  uint64_t depth_probe_target_frame_ = UINT64_MAX;
  // Bounded reporting of every distinct candidate adopted for a frame, so a run
  // shows how many depth targets a frame really has instead of guessing.
  uint32_t depth_probe_target_logs_ = 0;
  // RenderTargetKey::key values already reported, so the inventory is one line per
  // distinct depth target rather than one per draw.
  struct DepthProbeCandidateKey {
    uint32_t key;
  };
  DepthProbeCandidateKey depth_probe_candidates_[32] = {};
  uint32_t depth_probe_candidate_count_ = 0;
  // Linear float32 scene depth, row stride exactly the width: a pure copy-out of
  // one MSAA sample, never normalised, never written back. NaN payloads survive
  // because the shader bitcasts float to uint and the buffer is uint-typed.
  VkBuffer depth_probe_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory depth_probe_memory_ = VK_NULL_HANDLE;
  VkBuffer depth_probe_readback_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory depth_probe_readback_memory_ = VK_NULL_HANDLE;
  void* depth_probe_readback_mapped_ = nullptr;
  std::unique_ptr<ui::vulkan::SingleLayoutDescriptorSetPool> depth_probe_descriptor_set_pool_;
  VkDescriptorSet depth_probe_descriptor_set_ = VK_NULL_HANDLE;
  // Set 0 - depth destination storage buffer, set 1 - sampled depth image
  // (the stencil binding of the shared layout is declared but unused),
  // DepthProbePushConstants passed via push constants.
  VkPipelineLayout depth_probe_pipeline_layout_ = VK_NULL_HANDLE;
  std::unordered_map<uint32_t, VkPipeline> depth_probe_pipelines_;
  // Guest-seam source (depth_probe_source=0): the resolved guest range copied
  // into a device-local staging buffer and then into a host-visible readback
  // buffer of the same size, from which the tiled k_24_8 words are detiled.
  GuestRangeCapture depth_probe_guest_;
  // Decoded guest scene depth, in exactly the layout the host source writes, so
  // the statistics, the hash and the dump file do not depend on the source.
  std::vector<float> depth_probe_guest_linear_;
  // Whether the pending capture came from guest memory, and the destination
  // geometry and endianness needed to detile and decode it.
  bool depth_probe_pending_guest_ = false;
  uint32_t depth_probe_pending_pitch_ = 0;
  uint32_t depth_probe_pending_height_ = 0;
  uint32_t depth_probe_pending_origin_x_ = 0;
  uint32_t depth_probe_pending_origin_y_ = 0;
  uint32_t depth_probe_pending_endian_ = 0;
  // One-shot reporting of the guest range geometry and of the first decoded raw
  // words, so a refused range and the actual bit layout are both visible from a
  // run instead of being silent or assumed.
  bool depth_probe_guest_range_logged_ = false;
  bool depth_probe_guest_layout_logged_ = false;
  // One probe per guest frame, and none while a recorded probe still needs its
  // CPU-side statistics, so the single readback buffer is never overwritten
  // before the GPU has finished writing a previous capture into it.
  bool depth_probe_pending_ = false;
  uint32_t depth_probe_pending_frame_ = 0;
  uint32_t depth_probe_last_frame_ = UINT32_MAX;
  uint32_t depth_probe_frames_done_ = 0;
  // One-shot reporting so the guessed depth format and each refusal reason are
  // visible once instead of per frame, and so a probe that never fires is not
  // silent.
  bool depth_format_logged_ = false;
  uint32_t depth_probe_skipped_logs_ = 0;
  bool depth_probe_alloc_failed_logged_ = false;
  // Bounded inventory of guest depth resolves, so "depth has no guest-memory
  // seam" becomes a measured statement rather than an inference.
  uint32_t depth_resolve_logs_ = 0;

  // Development-only native pre-HUD scene colour capture (cvar colour_probe).
  // The resolved range is a tiled guest RGBA8 texture: the same staging and
  // readback machinery as the guest depth source, enlarged to 32 bpp colour.
  GuestRangeCapture colour_probe_guest_;
  // The detiled scene rectangle, row-major, four bytes per pixel - exactly the
  // bytes that are hashed and dumped.
  std::vector<uint8_t> colour_probe_rgba8_;
  // Destination geometry and endianness of the pending capture, recorded at the
  // resolve so the CPU decode does not depend on register state later.
  uint32_t colour_probe_pending_pitch_ = 0;
  uint32_t colour_probe_pending_height_ = 0;
  uint32_t colour_probe_pending_origin_x_ = 0;
  uint32_t colour_probe_pending_origin_y_ = 0;
  uint32_t colour_probe_pending_endian_ = 0;
  // One-shot reporting of the guest range, the destination format and the first
  // decoded words, so a refused range, a wrong channel order and a wrong offset
  // are all visible from a run instead of assumed.
  bool colour_probe_range_logged_ = false;
  bool colour_probe_format_logged_ = false;
  bool colour_probe_layout_logged_ = false;
  // One capture per guest frame, and none while a recorded capture still needs
  // its CPU-side decode, so the single readback buffer is never overwritten
  // before the GPU has finished writing a previous capture into it.
  bool colour_probe_pending_ = false;
  uint32_t colour_probe_pending_frame_ = 0;
  uint32_t colour_probe_last_frame_ = UINT32_MAX;
  // Which of the frame's 1000x600 colour resolves to capture, and how many
  // have been seen this frame. A frame resolves this extent several times at
  // several addresses; only some carry the finished scene.
  uint32_t colour_probe_resolve_frame_ = UINT32_MAX;
  uint32_t colour_probe_resolve_index_ = 0;
  uint32_t colour_probe_frames_done_ = 0;
  // Bounded reason-tagged refusals, so a probe that never fires is explainable
  // from the log instead of silent.
  uint32_t colour_probe_skipped_logs_ = 0;
  // Own budget for the per-resolve inventory, so it cannot be spent by boot-time
  // refusal logs before the probe window opens.
  uint32_t colour_probe_resolve_logs_ = 0;
  bool colour_probe_alloc_failed_logged_ = false;

  // Phase 2 motion guide resolve.
  // Set 0 - the sampled guide (combined image sampler: the shader is written as
  // sampler2DMS, and a bare sampled image cannot carry the multisample sample
  // operand through GLSL) and the packed output storage buffer.
  VkDescriptorSetLayout motion_guide_descriptor_set_layout_ = VK_NULL_HANDLE;
  // One sampler for every framebuffer: texelFetch ignores filtering, addressing
  // and LOD, so its state is never consulted - it exists only because a combined
  // image sampler binding requires one.
  VkSampler motion_guide_sampler_ = VK_NULL_HANDLE;
  // One descriptor set per framebuffer (Framebuffer::motion_descriptor_set), grown
  // on demand.
  std::unique_ptr<ui::vulkan::SingleLayoutDescriptorSetPool>
      motion_guide_descriptor_set_pool_;
  VkPipelineLayout motion_guide_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline motion_guide_pipeline_ = VK_NULL_HANDLE;
  // Device-local 1000*600 RGBA16F output of the resolve, read by NR later and
  // copied out for a dump; plus the host-visible copy the dump is written from,
  // which exists only when motion_dump was set at initialization.
  VkBuffer motion_guide_output_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory motion_guide_output_memory_ = VK_NULL_HANDLE;
  VkBuffer motion_guide_readback_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory motion_guide_readback_memory_ = VK_NULL_HANDLE;
  void* motion_guide_readback_mapped_ = nullptr;
  // A creation failure is not retried: retrying inside every resolve would churn
  // the allocator and log once per frame for an error that cannot resolve itself.
  bool motion_guide_resources_failed_ = false;
  // Framebuffers that received a motion-writing draw this frame, in first-draw
  // order (sorted by motion_last_draw_serial before the composite). This vector is
  // the frame's gate: the resolve composites exactly these and nothing else, and
  // ResolveMotionGuideFrame takes and clears it, so the next frame starts empty.
  // Cleared with the resolve resources as well, because a cache clear destroys the
  // framebuffers these point to.
  std::vector<const Framebuffer*> motion_frame_framebuffers_;
  // Monotonic across frames: only used to order one frame's list, so a reset is
  // unnecessary and wrap-around would take centuries.
  uint64_t motion_draw_serial_ = 0;
  // No dump while the previous one still needs its CPU write, so the single
  // readback buffer is never overwritten before the GPU has finished with it.
  bool motion_guide_pending_ = false;
  uint32_t motion_guide_pending_frame_ = 0;
  // Submission the copy-out was recorded in. The host read is only valid once that
  // submission completes; any earlier fence would read a buffer the GPU has not
  // finished writing.
  uint64_t motion_guide_pending_submission_ = 0;
  uint32_t motion_guide_dump_frames_done_ = 0;
  // Bounded logging: a few lines show how many framebuffers the composite
  // resolved and the first attachment's extent and sample count per run, and
  // every refusal reason is nameable at most once, so a resolve that never fires
  // is explainable from the log instead of silent.
  uint32_t motion_guide_resolve_logs_ = 0;
  uint32_t motion_guide_skipped_logs_ = 0;
  // Bounded one-shot log of the pass keys the guide sees, so an attachment that is
  // never created can be explained (which target shape the scene actually uses).
  uint32_t motion_guide_pass_logs_ = 0;

  // For pixel (fragment) shader interlock.

  VkRenderPass fsi_render_pass_ = VK_NULL_HANDLE;
  Framebuffer fsi_framebuffer_;

  VkPipelineLayout resolve_fsi_clear_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline resolve_fsi_clear_32bpp_pipeline_ = VK_NULL_HANDLE;
  VkPipeline resolve_fsi_clear_64bpp_pipeline_ = VK_NULL_HANDLE;
};

}  // namespace rex::graphics::vulkan
