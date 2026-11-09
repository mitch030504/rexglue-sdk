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
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
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
#include <rex/math.h>
#include <rex/types.h>
#include <rex/ui/dlss5_amd.h>
#include <rex/ui/dlss5_amd_vulkan.h>
#include <rex/ui/fsr4.h>
#include <rex/ui/motion_contract.h>
#include <rex/ui/temporal_jitter.h>
#include <rex/ui/vulkan/single_layout_descriptor_set_pool.h>
#include <rex/ui/vulkan/util.h>

#include "ui/fsr4_runtime.h"

#if defined(REX_HAS_DLSS5_AMD) && REX_HAS_DLSS5_AMD
#include <rex/runtime.h>

#include "nr_runtime.hpp"

namespace shaders {
#include "shaders/vulkan_spirv/nr_motion_prep_cs.h"
}  // namespace shaders

#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
// The FSR 4 seam (dlss5_amd_mode=4) runs the user-supplied provider through the
// same ffx API the presenter uses. The provider .so is loaded by ffx_runtime;
// only the entry points and the upscale descriptors are needed here.
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>

#include "ui/ffx_runtime.h"
#endif  // REX_HAS_FIDELITYFX_RUNTIME
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
                     "2 = substitute an NR image, 3 = live per-frame NR recorded at the "
                     "composition seam (every frame; dlss5_amd_frame is ignored), 4 = live "
                     "per-frame FSR 4 pre-upscale at the same seam (needs fsr4=true and the "
                     "FSR 4 provider; the presenter temporal upscaler must be off, i.e. "
                     "present_effect != fsr3; the NR model is not used). Modes 1/2 require "
                     "dlss5_amd=true and dlss5_amd_image; modes 3/4 require dlss5_amd=true.")
    .range(0, 4)
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
                     "Development: view format used for the substitute image on UNSIGNED "
                     "fetches - 0 = R8G8B8A8_UNORM (domain-identical to the guest composition "
                     "fetch, which is UNORM end-to-end), 1 = R8G8B8A8_SRGB (legacy). The image "
                     "itself is always created UNORM so all views are legal reinterprets.")
    .range(0, 1)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(dlss5_amd_signed_view, 0, "GPU",
                     "Development: view format used for the substitute image on SIGNED fetches "
                     "of the composition source - 0 = R8G8B8A8_SNORM (domain-identical to the "
                     "guest signed host view of base page 0x11C6F), 1 = R8G8B8A8_UNORM, 2 = "
                     "R8G8B8A8_SRGB (A/B). The composition draw samples 0x11C6F twice per draw, "
                     "once unsigned and once signed; each fetch gets its own view.")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(dlss5_amd_draw_filter, 1, "GPU",
                     "Development: which draws fetching the composition source (base page "
                     "0x11C6F) get the substitute view - 0 = every such draw (legacy; leaks the "
                     "substitute into intermediate buffers that later feed the composition, "
                     "e.g. the seq-1 term 0x1212F), 1 = only composition draws, identified by "
                     "also fetching the seq-1 page 0x1212F in the same draw.")
    .range(0, 1)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(dlss5_live_bypass, false, "GPU",
                    "Development (dlss5_amd_mode=3/4): the live seam's identity control - the "
                    "pre-upscaled scene colour is copied into the substitute instead of running "
                    "NR/FSR 4, so the split, the pre-upscale and the transfer can be checked "
                    "against the control without the network's edit.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(dlss5_live_fsr4_timing, true, "GPU",
                    "Development (dlss5_amd_mode=4): bracket the FSR 4 seam dispatch with GPU "
                    "timestamps and log fsr4_seam timing frame=<n> gpu_ms=<ms> when an earlier "
                    "frame's pair is provably complete (no queue wait). Off: no timestamps.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(dlss5_live_trace, false, "GPU",
                    "Development: log each step of the live seam (split, guide resolve, live "
                    "record, submit, resume) so a stall can be localized to its call.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(dlss5_live_no_depth, false, "GPU",
                    "Development (dlss5_amd_mode=3): do not feed the guest scene depth resolve "
                    "to NR (A/B of the silhouette guide).")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(dlss5_live_no_substitute, false, "GPU",
                    "Development: record live NR but keep the guest composition source (isolates "
                    "the substitute descriptor from the split and the record).")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(dlss5_live_dump, "", "GPU",
                      "Development (dlss5_amd_mode=3): if non-empty, one live frame is read back "
                      "for a byte-for-byte determinism comparison. Writes <prefix>-f<frame>-"
                      "colour.rgba, -motion.rg16f, -depth.r32f and -answer.rgba (raw, no header, "
                      "1920x1080 RGBA8 / RG16F / R32F / RGBA8) and <prefix>-f<frame>.json "
                      "(extents, formats, guest_frame, per-file SHA-256). The copy is recorded "
                      "in the live command buffer and read on a later live frame, so the queue "
                      "is never waited on; the recorded frame itself is unchanged.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(dlss5_live_dump_frame, -1, "GPU",
                     "Development: guest frame the live determinism dump records; -1 = the first "
                     "frame live NR records. The files are written once, on the next live frame "
                     "whose descriptor slot proves the copy's submission completed.")
    .range(-1, 1000000000)
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
// All reinterprets of the one UNORM image, created together at upload; the
// active one is picked per fetch signedness in SubstituteViewForFetch() -
// dlss5_amd_view for unsigned fetches, dlss5_amd_signed_view for signed ones.
VkImageView g_substitute_view_unorm = VK_NULL_HANDLE;
VkImageView g_substitute_view_srgb = VK_NULL_HANDLE;
VkImageView g_substitute_view_snorm = VK_NULL_HANDLE;
VkDevice g_substitute_device = VK_NULL_HANDLE;
PFN_vkDestroyImageView g_substitute_destroy_view = nullptr;
PFN_vkDestroyImage g_substitute_destroy_image = nullptr;
PFN_vkFreeMemory g_substitute_free_memory = nullptr;
bool g_assets_initialized = false;

// --- Live per-frame NR (mode 3) ---
// Persistent resources created in InitializeRuntime when dlss5_amd_mode == 3
// and the runtime constructed, at the runtime's own extent (model_width/height,
// which is what record_engine validates against). g_live_target is the NR
// answer and the composition substitute; g_live_colour is the pre-upscaled
// scene colour NR consumes (the guest composition source is 1000x600, while
// record_engine requires the colour at the runtime extent); g_live_motion is
// the prep shader's R16G16_SFLOAT output. The prep pipeline is the one compute
// shader compiled from shaders/nr_motion_prep_cs.glsl. The descriptor sets are
// a small ring: the command processor hands RecordLiveFrame the slot whose
// previous submission has completed, so updating the set is never in-flight.
//
// The layouts are tracked per image because the runtime takes the caller's
// `before` as the barrier oldLayout: after the composition samples the answer
// it is in SHADER_READ_ONLY_OPTIMAL, not GENERAL, and handing the runtime a
// stale GENERAL would make the next frame's write an illegal no-op transition.
#if defined(REX_HAS_DLSS5_AMD) && REX_HAS_DLSS5_AMD
constexpr uint32_t kLiveGuideWidth = 1000;
constexpr uint32_t kLiveGuideHeight = 600;
constexpr VkDeviceSize kLiveGuideBytes =
    VkDeviceSize(kLiveGuideWidth) * VkDeviceSize(kLiveGuideHeight) * 8u;
constexpr uint32_t kLiveDescriptorSlots = 4;

const vulkan::VulkanDevice* g_live_device = nullptr;
uint32_t g_live_width = 0;
uint32_t g_live_height = 0;
VkImage g_live_target = VK_NULL_HANDLE;
VkDeviceMemory g_live_target_memory = VK_NULL_HANDLE;
VkImageView g_live_target_view = VK_NULL_HANDLE;
VkImage g_live_colour = VK_NULL_HANDLE;
VkDeviceMemory g_live_colour_memory = VK_NULL_HANDLE;
VkImageView g_live_colour_view = VK_NULL_HANDLE;
VkImage g_live_motion = VK_NULL_HANDLE;
VkDeviceMemory g_live_motion_memory = VK_NULL_HANDLE;
VkImageView g_live_motion_view = VK_NULL_HANDLE;
// The NR depth input (R32_SFLOAT at the runtime extent): the prep writes it by
// nearest-sampling the guest's own scene depth resolve, and record_engine
// samples it in place. The guest depth texture itself is SAMPLED|TRANSFER_DST
// only, so it cannot be blitted from - this image is why the prep exists.
VkImage g_live_depth = VK_NULL_HANDLE;
VkDeviceMemory g_live_depth_memory = VK_NULL_HANDLE;
VkImageView g_live_depth_view = VK_NULL_HANDLE;
// 1x1 R32_SFLOAT bound as the prep's depth source on frames without a guest
// depth resolve (never sampled then - depth_enabled is 0).
VkImage g_live_null_depth = VK_NULL_HANDLE;
VkDeviceMemory g_live_null_depth_memory = VK_NULL_HANDLE;
VkImageView g_live_null_depth_view = VK_NULL_HANDLE;
// Actual current layout of each live image, in this queue's submission order.
VkImageLayout g_live_target_layout = VK_IMAGE_LAYOUT_UNDEFINED;
VkImageLayout g_live_colour_layout = VK_IMAGE_LAYOUT_UNDEFINED;
VkImageLayout g_live_motion_layout = VK_IMAGE_LAYOUT_UNDEFINED;
VkImageLayout g_live_depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
// The view substituted for the composition source: the NR target when the
// runtime takes a separate target, else the in-place colour answer.
VkImageView g_live_substitute_view = VK_NULL_HANDLE;
// The image behind g_live_substitute_view, for the determinism dump's answer copy.
VkImage g_live_substitute_image = VK_NULL_HANDLE;
VkSampler g_live_sampler = VK_NULL_HANDLE;
VkDescriptorSetLayout g_live_set_layout = VK_NULL_HANDLE;
std::unique_ptr<vulkan::SingleLayoutDescriptorSetPool> g_live_pool;
VkDescriptorSet g_live_sets[kLiveDescriptorSlots] = {};
VkPipelineLayout g_live_pipeline_layout = VK_NULL_HANDLE;
VkPipeline g_live_pipeline = VK_NULL_HANDLE;
bool g_live_resources_failed = false;

// --- Live determinism dump (dlss5_live_dump) ---
// One host-visible buffer holding the four 1920x1080 images at fixed offsets
// (colour RGBA8, motion RG16F, depth R32F, answer RGBA8 - 4 bytes per pixel
// each). It is created and mapped once at setup. A matching frame records the
// four copies into it and sets pending; the next RecordLiveFrame call that
// reuses the same seam slot reads it back. That slot is only handed out after
// the submission following the live command buffer has completed, so the copy
// is provably done: the queue is never waited on and the host never reads a
// buffer that may still be in flight. One frame, one buffer, no per-frame
// allocation.
VkBuffer g_live_dump_buffer = VK_NULL_HANDLE;
VkDeviceMemory g_live_dump_memory = VK_NULL_HANDLE;
void* g_live_dump_mapped = nullptr;
VkDeviceSize g_live_dump_image_bytes = 0;
// Cached at setup: both cvars are kInitOnly, and the recorded-frame path must
// not allocate or look anything up per frame.
bool g_live_dump_enabled = false;
std::string g_live_dump_prefix;
int32_t g_live_dump_frame = -1;
bool g_live_dump_pending = false;
bool g_live_dump_done = false;
uint32_t g_live_dump_pending_slot = 0;
uint64_t g_live_dump_pending_frame = 0;
// The guest frame the recorded dump belongs to (see CurrentGuestFrameIndex);
// captured with the copies, because the counter has advanced by the time the
// pending dump is read back.
int64_t g_live_dump_pending_guest_frame = -1;
uint32_t g_live_dump_pending_scene_width = 0;
uint32_t g_live_dump_pending_scene_height = 0;
bool g_live_dump_pending_depth = false;
bool g_live_dump_pending_in_place = false;
// Per-kind extents/byte sizes: mode 3 dumps all four images at the runtime
// extent, mode 4 dumps its scene-extent inputs (colour/motion/depth) next to
// the runtime-extent answer.
uint32_t g_live_dump_pending_width[4] = {};
uint32_t g_live_dump_pending_height[4] = {};
VkDeviceSize g_live_dump_pending_bytes[4] = {};

// --- Live FSR 4 pre-upscale (dlss5_amd_mode=4) ---
// FSR 4 runs where NR runs in mode 3: at the composition seam, on the native
// 1000x600 scene, before the HUD. It consumes its own scene-extent copies of
// the same three inputs the mode-3 prep already produces (colour, guide motion,
// guest depth) and writes the 1920x1080 answer into the existing live target.
// The scene copies exist because FSR validates renderSize = the guest resolve
// (1000x600) while the mode-3 prep writes at the NR evaluation extent
// (1920x1080); everything else - the target, the descriptor ring, the prep
// pipeline, the dump buffer and the layout discipline - is shared.
#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
constexpr uint32_t kFsrSceneWidth = 1000;
constexpr uint32_t kFsrSceneHeight = 600;

ffxContext g_fsr_context = nullptr;
const ffx::Functions* g_fsr_functions = nullptr;
std::string g_fsr_provider;
bool g_fsr_ready = false;
VkImage g_fsr_colour = VK_NULL_HANDLE;
VkDeviceMemory g_fsr_colour_memory = VK_NULL_HANDLE;
VkImageView g_fsr_colour_view = VK_NULL_HANDLE;
VkImage g_fsr_motion = VK_NULL_HANDLE;
VkDeviceMemory g_fsr_motion_memory = VK_NULL_HANDLE;
VkImageView g_fsr_motion_view = VK_NULL_HANDLE;
VkImage g_fsr_depth = VK_NULL_HANDLE;
VkDeviceMemory g_fsr_depth_memory = VK_NULL_HANDLE;
VkImageView g_fsr_depth_view = VK_NULL_HANDLE;
VkImageLayout g_fsr_colour_layout = VK_IMAGE_LAYOUT_UNDEFINED;
VkImageLayout g_fsr_motion_layout = VK_IMAGE_LAYOUT_UNDEFINED;
VkImageLayout g_fsr_depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
// The last measured dispatch, read from a completed timestamp pair on the next
// reuse of its seam slot (never a queue wait). 0 until the second slot reuse.
double g_fsr_last_gpu_ms = 0.0;
// GPU timestamps: one pair per seam slot, reset and written inside the live
// command buffer. A slot is reused only after the submission that followed the
// live command buffer completed, so the pair is readable then.
VkQueryPool g_fsr_timing_pool = VK_NULL_HANDLE;
float g_fsr_timestamp_period_ns = 0.0f;
uint64_t g_fsr_timing_frame[kLiveDescriptorSlots] = {};
bool g_fsr_timing_recorded[kLiveDescriptorSlots] = {};
// Frame time for FSR's history (per frame, not a timestamp query).
std::chrono::steady_clock::time_point g_fsr_last_time;
bool g_fsr_have_last_time = false;

// The FSR 4 provider's VK backend resolves a few Vulkan 1.1 core entry points
// through their legacy KHR aliases; RADV exposes only the core names. The
// presenter carries the same shim for its own context (vulkan_presenter.cpp);
// this copy keeps the seam independent of the presenter's lifecycle.
std::atomic<PFN_vkGetDeviceProcAddr> g_fsr_vk_get_device_proc_addr{nullptr};

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FsrVkGetDeviceProcAddrCompat(VkDevice vk_device,
                                                                      const char* name) {
  if (!name) {
    return nullptr;
  }
  PFN_vkGetDeviceProcAddr get_device_proc_addr =
      g_fsr_vk_get_device_proc_addr.load(std::memory_order_relaxed);
  if (!get_device_proc_addr) {
    return nullptr;
  }
  PFN_vkVoidFunction proc = get_device_proc_addr(vk_device, name);
  if (proc) {
    return proc;
  }
  if (!std::strcmp(name, "vkGetBufferMemoryRequirements2KHR")) {
    return get_device_proc_addr(vk_device, "vkGetBufferMemoryRequirements2");
  }
  if (!std::strcmp(name, "vkGetImageMemoryRequirements2KHR")) {
    return get_device_proc_addr(vk_device, "vkGetImageMemoryRequirements2");
  }
  if (!std::strcmp(name, "vkBindBufferMemory2KHR")) {
    return get_device_proc_addr(vk_device, "vkBindBufferMemory2");
  }
  if (!std::strcmp(name, "vkBindImageMemory2KHR")) {
    return get_device_proc_addr(vk_device, "vkBindImageMemory2");
  }
  return nullptr;
}

int64_t CurrentGuestFrameIndex();
void RecordLiveDumpFrameFsr4(VkCommandBuffer cmd, const vulkan::VulkanDevice::Functions& dfn,
                             uint64_t frame, uint32_t scene_width, uint32_t scene_height,
                             bool bypass, uint32_t descriptor_slot);
#endif  // REX_HAS_FIDELITYFX_RUNTIME

// Defined below, after the SHA-256 helper (the dump hashes the readback).
void ConsumePendingLiveDump();
bool RecordLiveDumpFrame(VkCommandBuffer cmd, const vulkan::VulkanDevice::Functions& dfn,
                         uint64_t frame, uint32_t scene_width, uint32_t scene_height,
                         bool takes_target, bool depth_enabled, uint32_t descriptor_slot);
#endif  // REX_HAS_DLSS5_AMD

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
  // Mode 3 substitutes the live NR answer instead of the uploaded image, so it
  // is enabled whenever the live resources exist, even without dlss5_amd_image.
  return REXCVAR_GET(dlss5_amd_mode) != 0 &&
         (g_substitute_view_unorm != VK_NULL_HANDLE || LiveEnabled());
}

bool SubstituteForFrame(uint64_t frame) {
  // Modes 3 and 4 are live: every guest frame is substituted (the frame cvar's
  // -1 semantics), the per-frame gating happens in the command processor.
  if (REXCVAR_GET(dlss5_amd_mode) == 3 || REXCVAR_GET(dlss5_amd_mode) == 4) {
    return true;
  }
  const int32_t first = REXCVAR_GET(dlss5_amd_frame);
  return first < 0 || uint64_t(first) == frame;
}

bool LiveEnabled() {
#if defined(REX_HAS_DLSS5_AMD) && REX_HAS_DLSS5_AMD
  if (REXCVAR_GET(dlss5_amd_mode) == 4) {
#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
    return g_fsr_ready && g_live_substitute_view != VK_NULL_HANDLE;
#else
    return false;
#endif
  }
  return REXCVAR_GET(dlss5_amd_mode) == 3 && g_runtime != nullptr &&
         g_live_substitute_view != VK_NULL_HANDLE;
#else
  return false;
#endif
}

VkImageView LiveTargetView() {
  // The view substituted for the unsigned composition fetch in mode 3/4: the
  // NR/FSR answer (a separate target when the runtime takes one, else the
  // in-place colour). The signed fetch keeps SubstituteViewForFetch - it is
  // never sampled by the composition shader.
#if defined(REX_HAS_DLSS5_AMD) && REX_HAS_DLSS5_AMD
  return g_live_substitute_view;
#else
  return VK_NULL_HANDLE;
#endif
}

#if defined(REX_HAS_DLSS5_AMD) && REX_HAS_DLSS5_AMD
namespace {

// Bindings of shaders/nr_motion_prep_cs.glsl for one frame: the guide buffer
// and the guest colour/depth sources are per-frame, the three output images are
// the caller's (the runtime extent in mode 3, the scene extent in mode 4).
void UpdateLivePrepDescriptors(VkDescriptorSet descriptor_set, const LiveFrameInput& input,
                               VkImageView motion_out, VkImageView colour_out,
                               VkImageView depth_out) {
  VkDescriptorBufferInfo guide_info{};
  guide_info.buffer = input.guide_buffer;
  guide_info.offset = 0;
  guide_info.range = kLiveGuideBytes;
  VkDescriptorImageInfo motion_info{};
  motion_info.imageView = motion_out;
  motion_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo colour_src_info{};
  colour_src_info.sampler = input.sampler != VK_NULL_HANDLE ? input.sampler : g_live_sampler;
  colour_src_info.imageView = input.colour_view;
  colour_src_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkDescriptorImageInfo colour_out_info{};
  colour_out_info.imageView = colour_out;
  colour_out_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  const bool has_depth = input.depth_view != VK_NULL_HANDLE;
  VkDescriptorImageInfo depth_src_info{};
  depth_src_info.sampler = g_live_sampler;
  depth_src_info.imageView = has_depth ? input.depth_view : g_live_null_depth_view;
  depth_src_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkDescriptorImageInfo depth_out_info{};
  depth_out_info.imageView = depth_out;
  depth_out_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkWriteDescriptorSet writes[6] = {};
  for (uint32_t i = 0; i < 6; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = descriptor_set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
  }
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[0].pBufferInfo = &guide_info;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[1].pImageInfo = &motion_info;
  writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[2].pImageInfo = &colour_src_info;
  writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[3].pImageInfo = &colour_out_info;
  writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[4].pImageInfo = &depth_src_info;
  writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[5].pImageInfo = &depth_out_info;
  g_live_device->functions().vkUpdateDescriptorSets(g_live_device->device(),
                                                    uint32_t(rex::countof(writes)), writes, 0,
                                                    nullptr);
}

}  // namespace
#endif  // REX_HAS_DLSS5_AMD

#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
// Defined after RecordLiveFrame; mode 4 dispatches here from the shared seam.
bool RecordLiveFsr4Frame(VkCommandBuffer cmd, const LiveFrameInput& input);
#endif

bool RecordLiveFrame(VkCommandBuffer cmd, const LiveFrameInput& input) {
#if defined(REX_HAS_DLSS5_AMD) && REX_HAS_DLSS5_AMD
  const bool fsr4_seam = REXCVAR_GET(dlss5_amd_mode) == 4;
  // Mode 4's identity control is deliberately the mode-3 bypass body below: it
  // runs the same prep at the runtime extent and copies that colour into the
  // target, so answer == colour is the same measurement in both modes.
  const bool fsr4_bypass = fsr4_seam && REXCVAR_GET(dlss5_live_bypass);
#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
  if (fsr4_seam && !fsr4_bypass) {
    return RecordLiveFsr4Frame(cmd, input);
  }
#else
  if (fsr4_seam) {
    return false;
  }
#endif
  if ((!g_runtime && !fsr4_bypass) || g_live_device == nullptr ||
      g_live_pipeline == VK_NULL_HANDLE || input.colour_view == VK_NULL_HANDLE ||
      input.guide_buffer == VK_NULL_HANDLE || !input.colour_width || !input.colour_height ||
      VkDeviceSize(input.colour_width) * VkDeviceSize(input.colour_height) * 8u > kLiveGuideBytes ||
      input.descriptor_slot >= kLiveDescriptorSlots) {
    return false;
  }
  const vulkan::VulkanDevice& device = *g_live_device;
  const vulkan::VulkanDevice::Functions& dfn = device.functions();
  const VkImageSubresourceRange full_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

  // A pending determinism dump is read back only when this call reuses the seam
  // slot its copies were recorded with: the command processor hands that slot
  // out only after the submission that followed the live command buffer has
  // completed, so the copy is done. Never wait on the queue here.
  if (g_live_dump_pending && input.descriptor_slot == g_live_dump_pending_slot) {
    ConsumePendingLiveDump();
  }

  // Rebind the slot's descriptor set for this frame: the guide buffer is
  // persistent but may have been recreated by a cache clear, and the guest
  // colour view changes as the texture cache re-creates textures. The caller
  // only hands out a slot whose previous submission has completed, so the set
  // is not in use by in-flight work. The mode-3 outputs live at the runtime
  // extent; mode 4 binds its scene-extent copies through the same helper.
  const VkDescriptorSet descriptor_set = g_live_sets[input.descriptor_slot];
  UpdateLivePrepDescriptors(descriptor_set, input, g_live_motion_view, g_live_colour_view,
                            g_live_depth_view);
  const bool has_depth = input.depth_view != VK_NULL_HANDLE;

  // Everything the previous (same-queue, already submitted) work wrote - the
  // guide resolve's buffer stores and the guest texture's detile/transfer
  // writes - must be visible to the prep dispatch. A device-scope memory
  // barrier covers the guest image, whose VkImage is not known here (only its
  // view), and the guide buffer together; the images stay in their current
  // layouts (GENERAL for ours, SHADER_READ_ONLY_OPTIMAL for the guest view).
  VkMemoryBarrier in_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  in_barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                             VK_ACCESS_TRANSFER_WRITE_BIT;
  in_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &in_barrier, 0, nullptr, 0,
                           nullptr);

  // The prep writes the colour as a storage image, which requires GENERAL. In
  // the in-place fallback the answer is left SHADER_READ_ONLY (the composition
  // samples it), so from the second frame on that is the colour's real layout.
  if (g_live_colour_layout != VK_IMAGE_LAYOUT_GENERAL) {
    VkImageMemoryBarrier to_general{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_general.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_general.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    to_general.oldLayout = g_live_colour_layout;
    to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_general.image = g_live_colour;
    to_general.subresourceRange = full_range;
    dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &to_general);
    g_live_colour_layout = VK_IMAGE_LAYOUT_GENERAL;
  }
  // The depth output has the same storage-write requirement and the same
  // in-place sample afterwards (record_engine reads it where the prep wrote
  // it), so it follows the colour's layout cycle.
  if (g_live_depth_layout != VK_IMAGE_LAYOUT_GENERAL) {
    VkImageMemoryBarrier to_general{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_general.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_general.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    to_general.oldLayout = g_live_depth_layout;
    to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_general.image = g_live_depth;
    to_general.subresourceRange = full_range;
    dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &to_general);
    g_live_depth_layout = VK_IMAGE_LAYOUT_GENERAL;
  }

  dfn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_live_pipeline);
  dfn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_live_pipeline_layout, 0, 1,
                              &descriptor_set, 0, nullptr);
  // [out extent, scene extent] - the pre-upscale maps the whole source onto the
  // whole output, exactly as the composition's own fetch of the source does.
  const uint32_t push_constants[5] = {g_live_width, g_live_height, input.colour_width,
                                      input.colour_height, has_depth ? 1u : 0u};
  dfn.vkCmdPushConstants(cmd, g_live_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                         sizeof(push_constants), push_constants);
  dfn.vkCmdDispatch(cmd, (g_live_width + 7u) / 8u, (g_live_height + 7u) / 8u, 1);
  const bool trace = REXCVAR_GET(dlss5_live_trace);
  if (trace) {
    REXGPU_INFO("dlss5_live frame={} prep dispatched {}x{} -> {}x{}", input.frame,
                input.colour_width, input.colour_height, g_live_width, g_live_height);
  }

  // Make the prep dispatch's stores visible to record_engine's own barriers.
  VkMemoryBarrier out_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  out_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  out_barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT |
                              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &out_barrier, 0, nullptr, 0,
                           nullptr);

  nr::Controls controls;
  // Mode 4's target is always the separate 1920x1080 answer; the in-place
  // fallback is an NR-runtime choice (mode 3). The bypass body below then
  // copies into that target exactly as mode 3 does with a separate target.
  const bool takes_target = fsr4_bypass ? true : g_runtime->takes_target(controls);
  if (REXCVAR_GET(dlss5_live_bypass)) {
    // The live identity control: no network. The pre-upscaled colour is the
    // answer, so copy it into the target (or keep it in place) and leave it in
    // the layout the composition sample needs.
    if (takes_target) {
      if (g_live_target_layout != VK_IMAGE_LAYOUT_GENERAL) {
        VkImageMemoryBarrier to_general{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        to_general.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        to_general.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_general.oldLayout = g_live_target_layout;
        to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_general.image = g_live_target;
        to_general.subresourceRange = full_range;
        dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &to_general);
      }
      VkImageCopy copy{};
      copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.dstSubresource = copy.srcSubresource;
      copy.extent = {g_live_width, g_live_height, 1};
      dfn.vkCmdCopyImage(cmd, g_live_colour, VK_IMAGE_LAYOUT_GENERAL, g_live_target,
                         VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
      VkImageMemoryBarrier to_sampled{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      to_sampled.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      to_sampled.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      to_sampled.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      to_sampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      to_sampled.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_sampled.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_sampled.image = g_live_target;
      to_sampled.subresourceRange = full_range;
      dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                               &to_sampled);
      g_live_target_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    } else {
      VkImageMemoryBarrier to_sampled{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      to_sampled.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      to_sampled.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      to_sampled.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      to_sampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      to_sampled.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_sampled.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_sampled.image = g_live_colour;
      to_sampled.subresourceRange = full_range;
      dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                               &to_sampled);
      g_live_colour_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    // The dump copies are recorded after the answer is final, in this same
    // command buffer; they restore every image to the layout tracked above.
    RecordLiveDumpFrame(cmd, dfn, input.frame, input.colour_width, input.colour_height,
                        takes_target, has_depth, input.descriptor_slot);
    REXGPU_INFO("dlss5_live frame={} BYPASS pre-upscale only ({}x{} -> {}x{})", input.frame,
                input.colour_width, input.colour_height, g_live_width, g_live_height);
    ReportRecordedFrame(true, 0.0f);
    return true;
  }

  nr::EngineFrame engine_frame;
  engine_frame.colour.image = g_live_colour;
  engine_frame.colour.format = VK_FORMAT_R8G8B8A8_UNORM;
  engine_frame.colour.width = g_live_width;
  engine_frame.colour.height = g_live_height;
  engine_frame.colour.before = VK_IMAGE_LAYOUT_GENERAL;
  // In-place: the answer stays here and the composition samples it, so it must
  // end in the sampled layout. A separate target leaves the colour as an input.
  engine_frame.colour.after =
      takes_target ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  engine_frame.colour.before_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  engine_frame.colour.before_access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  engine_frame.colour.after_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  engine_frame.colour.after_access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  engine_frame.colour.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  engine_frame.motion.image = g_live_motion;
  engine_frame.motion.format = VK_FORMAT_R16G16_SFLOAT;
  engine_frame.motion.width = g_live_width;
  engine_frame.motion.height = g_live_height;
  engine_frame.motion.before = VK_IMAGE_LAYOUT_GENERAL;
  engine_frame.motion.after = VK_IMAGE_LAYOUT_GENERAL;
  engine_frame.motion.before_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  engine_frame.motion.before_access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  engine_frame.motion.after_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  engine_frame.motion.after_access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  engine_frame.motion.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
  // Forward guest-NDC motion -> backward normalized UV (motion_contract.h).
  engine_frame.motion_scale_x = rex::ui::kMotionNdcToBackwardUvScaleX;
  engine_frame.motion_scale_y = rex::ui::kMotionNdcToBackwardUvScaleY;
  engine_frame.motion_texture_width = g_live_width;
  engine_frame.motion_texture_height = g_live_height;
  // Depth is optional: the prep's nearest resample of the guest's scene depth
  // resolve, at the runtime extent, sampled in place by record_engine (the
  // runtime's depth_in_place path - no blit, and no TRANSFER_SRC needed on the
  // guest texture). `depth_inverted=false` is the Xenos convention the depth
  // probe validated.
  if (has_depth) {
    engine_frame.depth.image = g_live_depth;
    engine_frame.depth.format = VK_FORMAT_R32_SFLOAT;
    engine_frame.depth.width = g_live_width;
    engine_frame.depth.height = g_live_height;
    engine_frame.depth.before = VK_IMAGE_LAYOUT_GENERAL;
    engine_frame.depth.after = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    engine_frame.depth.before_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    engine_frame.depth.before_access = VK_ACCESS_SHADER_WRITE_BIT;
    engine_frame.depth.after_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    engine_frame.depth.after_access = VK_ACCESS_SHADER_READ_BIT;
    engine_frame.depth.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  }
  engine_frame.depth_inverted = false;
  engine_frame.reset = input.reset;
  engine_frame.feature = 0;
  if (takes_target) {
    engine_frame.target.image = g_live_target;
    engine_frame.target.format = VK_FORMAT_R8G8B8A8_UNORM;
    engine_frame.target.width = g_live_width;
    engine_frame.target.height = g_live_height;
    engine_frame.target.before = g_live_target_layout;
    // The composition draw samples the answer, so end in the sampled layout.
    engine_frame.target.after = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    engine_frame.target.before_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    engine_frame.target.before_access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    engine_frame.target.after_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    engine_frame.target.after_access = VK_ACCESS_SHADER_READ_BIT;
    engine_frame.target.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  }

  nr::EngineResult result;
  try {
    if (REXCVAR_GET(dlss5_live_trace)) {
      REXGPU_INFO("dlss5_live frame={} engine_begin target={} colour_before={}", input.frame,
                  takes_target ? "separate" : "in-place",
                  engine_frame.target.before == VK_IMAGE_LAYOUT_GENERAL ? "general" : "sampled");
    }
    result = g_runtime->record_engine(cmd, engine_frame, controls);
  } catch (const std::exception& e) {
    REXLOG_WARN("DLSS 5 NR (AMD): record_engine failed on frame {}: {}", input.frame, e.what());
    // The layouts the runtime left behind are unknown; stop touching the live
    // resources rather than hand the next frame a wrong oldLayout.
    g_live_resources_failed = true;
    return false;
  }
  // record_engine ends each image at its requested `after` (the colour only
  // when it is the answer; a separate target otherwise).
  g_live_colour_layout = engine_frame.colour.after;
  g_live_motion_layout = VK_IMAGE_LAYOUT_GENERAL;
  if (has_depth) {
    g_live_depth_layout = engine_frame.depth.after;
  }
  if (takes_target) {
    g_live_target_layout = engine_frame.target.after;
  }
  // The dump copies run after record_engine, when colour/motion/depth and the
  // answer hold this frame's final values; the helper restores each image to
  // the layout tracked above, so the composition still sees the same layouts.
  RecordLiveDumpFrame(cmd, dfn, input.frame, input.colour_width, input.colour_height, takes_target,
                      has_depth, input.descriptor_slot);
  REXGPU_INFO("dlss5_live frame={} applied={} dispatches={} history_consumed={} provider={} "
              "gpu_ms={:.3f} reset={} extent={}x{} scene={}x{} depth={}",
              input.frame, result.frame.applied ? 1 : 0, result.frame.network_dispatches,
              result.history_consumed ? 1 : 0, result.motion_provider,
              double(g_runtime->last_gpu_ms()), input.reset ? 1 : 0, g_live_width, g_live_height,
              input.colour_width, input.colour_height,
              has_depth ? "engine" : "none");
  ReportRecordedFrame(result.frame.applied, g_runtime->last_gpu_ms());
  return result.frame.applied;
#else
  (void)cmd;
  (void)input;
  return false;
#endif
}

#if defined(REX_HAS_DLSS5_AMD) && REX_HAS_DLSS5_AMD && \
    defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
// Mode 4: one FSR 4 dispatch at the same composition seam. The prep shader is
// the mode-3 one, run at the scene extent into the FSR input copies; FSR then
// upscales 1000x600 -> the live target, which the composition substitutes for
// the guest scene exactly as mode 3 substitutes the NR answer. The HUD draws
// after the seam and never sees the substitute.
bool RecordLiveFsr4Frame(VkCommandBuffer cmd, const LiveFrameInput& input) {
  if (!g_fsr_ready || g_fsr_context == nullptr || g_fsr_functions == nullptr ||
      g_live_device == nullptr || g_live_pipeline == VK_NULL_HANDLE ||
      input.colour_view == VK_NULL_HANDLE || input.guide_buffer == VK_NULL_HANDLE ||
      input.descriptor_slot >= kLiveDescriptorSlots) {
    return false;
  }
  // The context was created for the guest scene extent; a different resolve
  // size would be a different renderSize, which FSR validates. Skip the frame
  // (guest view kept) rather than hand FSR a mismatched extent.
  if (input.colour_width != kFsrSceneWidth || input.colour_height != kFsrSceneHeight) {
    static bool extent_logged = false;
    if (!extent_logged) {
      extent_logged = true;
      REXGPU_WARN("fsr4_seam: scene is {}x{}, expected {}x{}; keeping the guest view",
                  input.colour_width, input.colour_height, kFsrSceneWidth, kFsrSceneHeight);
    }
    return false;
  }
  // FSR 4 uses depth for disocclusion; a frame without the guest depth resolve
  // has no honest input, so it keeps the guest view too.
  if (input.depth_view == VK_NULL_HANDLE) {
    static bool depth_logged = false;
    if (!depth_logged) {
      depth_logged = true;
      REXGPU_WARN("fsr4_seam: no guest depth resolve this frame; keeping the guest view");
    }
    return false;
  }
  const vulkan::VulkanDevice& device = *g_live_device;
  const vulkan::VulkanDevice::Functions& dfn = device.functions();
  const VkImageSubresourceRange full_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  const uint32_t slot = input.descriptor_slot;

  // Read the timestamp pair recorded for this seam slot the previous time it
  // was used. The command processor hands a slot out only after the submission
  // following its live command buffer completed, so the pair is ready and the
  // queue is never waited on.
  if (g_fsr_timing_pool != VK_NULL_HANDLE && g_fsr_timing_recorded[slot]) {
    uint64_t stamps[2] = {};
    if (dfn.vkGetQueryPoolResults(device.device(), g_fsr_timing_pool, slot * 2, 2, sizeof(stamps),
                                  stamps, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
        stamps[1] > stamps[0]) {
      g_fsr_last_gpu_ms =
          double(stamps[1] - stamps[0]) * double(g_fsr_timestamp_period_ns) / 1e6;
      if (REXCVAR_GET(dlss5_live_fsr4_timing)) {
        REXGPU_INFO("fsr4_seam timing frame={} gpu_ms={:.3f}", g_fsr_timing_frame[slot],
                    g_fsr_last_gpu_ms);
      }
    }
    g_fsr_timing_recorded[slot] = false;
  }
  // A pending determinism dump is read back on the same slot-reuse proof as in
  // mode 3 (the copies were recorded into this slot's previous command buffer).
  if (g_live_dump_pending && slot == g_live_dump_pending_slot) {
    ConsumePendingLiveDump();
  }

  // Bind the prep outputs at the scene extent: these are the images FSR reads,
  // not the mode-3 runtime-extent ones (the target is the same 1920x1080 one).
  UpdateLivePrepDescriptors(g_live_sets[slot], input, g_fsr_motion_view, g_fsr_colour_view,
                            g_fsr_depth_view);

  // The guide resolve and the guest textures wrote in the submission that just
  // ended; make those writes visible to the prep dispatch, exactly as mode 3.
  VkMemoryBarrier in_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  in_barrier.srcAccessMask =
      VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  in_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &in_barrier, 0, nullptr, 0,
                           nullptr);

  // FSR sampled these images last frame or the composition will after this
  // one, so their real layout is SHADER_READ_ONLY from the second frame on;
  // the storage write needs GENERAL.
  VkImageMemoryBarrier scene_to_general[3] = {};
  const VkImage scene_images[3] = {g_fsr_colour, g_fsr_motion, g_fsr_depth};
  const VkImageLayout scene_layouts[3] = {g_fsr_colour_layout, g_fsr_motion_layout,
                                          g_fsr_depth_layout};
  uint32_t scene_transition_count = 0;
  for (uint32_t i = 0; i < 3; ++i) {
    if (scene_layouts[i] == VK_IMAGE_LAYOUT_GENERAL) {
      continue;
    }
    VkImageMemoryBarrier& barrier = scene_to_general[scene_transition_count++];
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.oldLayout = scene_layouts[i];
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = scene_images[i];
    barrier.subresourceRange = full_range;
  }
  if (scene_transition_count != 0) {
    dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                             scene_transition_count, scene_to_general);
  }
  g_fsr_colour_layout = VK_IMAGE_LAYOUT_GENERAL;
  g_fsr_motion_layout = VK_IMAGE_LAYOUT_GENERAL;
  g_fsr_depth_layout = VK_IMAGE_LAYOUT_GENERAL;

  dfn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_live_pipeline);
  dfn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_live_pipeline_layout, 0, 1,
                              &g_live_sets[slot], 0, nullptr);
  // [out extent, scene extent, depth enabled] - the scene inputs are a 1:1
  // resample of the guest resolve at its own extent.
  const uint32_t push_constants[5] = {kFsrSceneWidth, kFsrSceneHeight, input.colour_width,
                                      input.colour_height, 1u};
  dfn.vkCmdPushConstants(cmd, g_live_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                         sizeof(push_constants), push_constants);
  dfn.vkCmdDispatch(cmd, (kFsrSceneWidth + 7u) / 8u, (kFsrSceneHeight + 7u) / 8u, 1);

  // Make the prep stores visible, then put the three images where FSR's
  // PIXEL_COMPUTE_READ state says they are (SHADER_READ_ONLY).
  VkMemoryBarrier prep_done{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  prep_done.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  prep_done.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &prep_done, 0, nullptr, 0,
                           nullptr);
  VkImageMemoryBarrier scene_to_sampled[3] = {};
  for (uint32_t i = 0; i < 3; ++i) {
    VkImageMemoryBarrier& barrier = scene_to_sampled[i];
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = scene_images[i];
    barrier.subresourceRange = full_range;
  }
  dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 3,
                           scene_to_sampled);
  g_fsr_colour_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  g_fsr_motion_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  g_fsr_depth_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  // The answer image is FSR's UAV, i.e. GENERAL while it is dispatched. It is
  // SHADER_READ_ONLY after the previous frame's composition sampled it.
  if (g_live_target_layout != VK_IMAGE_LAYOUT_GENERAL) {
    VkImageMemoryBarrier target_to_general{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    target_to_general.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    target_to_general.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    target_to_general.oldLayout = g_live_target_layout;
    target_to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    target_to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    target_to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    target_to_general.image = g_live_target;
    target_to_general.subresourceRange = full_range;
    dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &target_to_general);
    g_live_target_layout = VK_IMAGE_LAYOUT_GENERAL;
  }

  // The frame's jitter is the guest projection's own Halton offset (flip 1,
  // which fsr4=true selects at startup). The guide motion is already
  // jitter-removed, so the context does not enable jitter cancellation and the
  // motion scale below turns the guide's forward guest NDC into the backward
  // top-left UV FSR consumes (motion_contract.h).
  const int64_t guest_frame = CurrentGuestFrameIndex();
  float jitter_x = 0.0f;
  float jitter_y = 0.0f;
  rex::ui::temporal::JitterForFrame(guest_frame >= 0 ? uint64_t(guest_frame) : input.frame,
                                    jitter_x, jitter_y);
  float frame_time_ms = 16.666f;
  const auto now = std::chrono::steady_clock::now();
  if (g_fsr_have_last_time) {
    frame_time_ms = std::clamp(
        std::chrono::duration<float, std::milli>(now - g_fsr_last_time).count(), 1.0f, 100.0f);
  }
  g_fsr_last_time = now;
  g_fsr_have_last_time = true;

  auto describe = [](VkImage image, VkFormat format, uint32_t width, uint32_t height,
                     uint32_t extra_usage) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    return ffxApiGetResourceVK(
        reinterpret_cast<void*>(image),
        ffxApiGetImageResourceDescriptionVK(image, info, extra_usage),
        FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  };
  ffxDispatchDescUpscale desc{};
  desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  desc.commandList = cmd;
  desc.color = describe(g_fsr_colour, VK_FORMAT_R8G8B8A8_UNORM, kFsrSceneWidth, kFsrSceneHeight,
                        FFX_API_RESOURCE_USAGE_READ_ONLY);
  desc.depth = describe(g_fsr_depth, VK_FORMAT_R32_SFLOAT, kFsrSceneWidth, kFsrSceneHeight,
                        FFX_API_RESOURCE_USAGE_READ_ONLY);
  desc.motionVectors =
      describe(g_fsr_motion, VK_FORMAT_R16G16_SFLOAT, kFsrSceneWidth, kFsrSceneHeight,
               FFX_API_RESOURCE_USAGE_READ_ONLY);
  desc.exposure = {};
  desc.reactive = {};
  desc.transparencyAndComposition = {};
  {
    VkImageCreateInfo output_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    output_info.imageType = VK_IMAGE_TYPE_2D;
    output_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    output_info.extent = {g_live_width, g_live_height, 1};
    output_info.mipLevels = 1;
    output_info.arrayLayers = 1;
    output_info.samples = VK_SAMPLE_COUNT_1_BIT;
    output_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    desc.output = ffxApiGetResourceVK(
        reinterpret_cast<void*>(g_live_target),
        ffxApiGetImageResourceDescriptionVK(g_live_target, output_info,
                                            FFX_API_RESOURCE_USAGE_UAV |
                                                FFX_API_RESOURCE_USAGE_READ_ONLY),
        FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  desc.jitterOffset = {jitter_x, jitter_y};
  desc.motionVectorScale = {rex::ui::kMotionNdcToBackwardUvScaleX,
                            rex::ui::kMotionNdcToBackwardUvScaleY};
  desc.renderSize = {kFsrSceneWidth, kFsrSceneHeight};
  desc.upscaleSize = {g_live_width, g_live_height};
  // Pure SR: no extra RCAS, the presenter's own output effect stays in charge.
  desc.enableSharpening = false;
  desc.sharpness = 0.0f;
  desc.frameTimeDelta = frame_time_ms;
  desc.preExposure = 1.0f;
  desc.reset = input.reset;
  // The recompiled game does not expose camera parameters; the presenter uses
  // the same placeholders. FSR's depth use with these is the open T4.6/T4.12
  // measurement (real depth, estimated camera).
  desc.cameraNear = 0.1f;
  desc.cameraFar = 1000.0f;
  desc.cameraFovAngleVertical = 1.0472f;
  desc.viewSpaceToMetersFactor = 1.0f;
  desc.flags = 0;

  const bool timing = g_fsr_timing_pool != VK_NULL_HANDLE;
  if (timing) {
    dfn.vkCmdResetQueryPool(cmd, g_fsr_timing_pool, slot * 2, 2);
    dfn.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_fsr_timing_pool, slot * 2);
  }
  const ffxReturnCode_t dispatch_result =
      g_fsr_functions->Dispatch(&g_fsr_context, &desc.header);
  if (timing) {
    dfn.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_fsr_timing_pool,
                            slot * 2 + 1);
    g_fsr_timing_frame[slot] = input.frame;
    g_fsr_timing_recorded[slot] = true;
  }
  if (dispatch_result != FFX_API_RETURN_OK) {
    static bool dispatch_logged = false;
    if (!dispatch_logged) {
      dispatch_logged = true;
      REXGPU_WARN("fsr4_seam: FSR 4 dispatch failed ({}), keeping the guest view",
                  dispatch_result);
    }
    // The backend may have left the target in any layout when it bails out;
    // stop substituting rather than track a guess.
    g_fsr_ready = false;
    return false;
  }

  // The backend reset the imported resources to their initial states, so the
  // answer is GENERAL again; the composition samples it.
  VkImageMemoryBarrier target_to_sampled{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  target_to_sampled.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  target_to_sampled.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  target_to_sampled.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  target_to_sampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  target_to_sampled.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  target_to_sampled.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  target_to_sampled.image = g_live_target;
  target_to_sampled.subresourceRange = full_range;
  dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &target_to_sampled);
  g_live_target_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  RecordLiveDumpFrameFsr4(cmd, dfn, input.frame, input.colour_width, input.colour_height, false,
                          slot);
  REXGPU_INFO("fsr4_seam frame={} applied=1 provider={} render={}x{} output={}x{} "
              "jitter=({:.3f},{:.3f}) gpu_ms={:.3f} reset={}",
              input.frame, g_fsr_provider, kFsrSceneWidth, kFsrSceneHeight, g_live_width,
              g_live_height, jitter_x, jitter_y, g_fsr_last_gpu_ms, input.reset ? 1 : 0);
  ReportRecordedFrame(true, float(g_fsr_last_gpu_ms));
  return true;
}
#endif  // REX_HAS_DLSS5_AMD && REX_HAS_FIDELITYFX_RUNTIME

VkImageView SubstituteView() {
  // Unsigned-fetch case of SubstituteViewForFetch(). dlss5_amd_view is
  // kInitOnly and both views are created at upload, so this is a pure
  // selection between the UNORM and SRGB reinterpret of the image.
  return REXCVAR_GET(dlss5_amd_view) == 0 ? g_substitute_view_unorm : g_substitute_view_srgb;
}

VkImageView SubstituteViewForFetch(bool is_signed) {
  // The composition draw samples base page 0x11C6F twice per draw: once
  // unsigned (guest host view UNORM) and once signed (guest host view SNORM),
  // and each fetch must see a substitute view in the same domain. All three
  // views are created at upload over the one UNORM image (both cvars are
  // kInitOnly), so this is a pure selection per fetch signedness.
  if (!is_signed) {
    return SubstituteView();
  }
  switch (REXCVAR_GET(dlss5_amd_signed_view)) {
    case 1:
      return g_substitute_view_unorm;
    case 2:
      return g_substitute_view_srgb;
    default:
      return g_substitute_view_snorm;
  }
}

VkFormat SubstituteViewFormatForFetch(bool is_signed) {
  // The VkFormat of the view SubstituteViewForFetch() would bind for this
  // fetch, for the one-shot substitution log.
  const VkImageView view = SubstituteViewForFetch(is_signed);
  if (view == g_substitute_view_snorm) {
    return VK_FORMAT_R8G8B8A8_SNORM;
  }
  if (view == g_substitute_view_srgb) {
    return VK_FORMAT_R8G8B8A8_SRGB;
  }
  return VK_FORMAT_R8G8B8A8_UNORM;
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

  // Three views of the one UNORM image: UNORM is domain-identical to the guest
  // unsigned composition fetch (no gamma PWL anywhere on the guest path), SNORM
  // is domain-identical to the guest SIGNED host view of the same base page
  // (the composition fetches 0x11C6F twice per draw, once per sign), and SRGB
  // inserts a decode the guest never applies. dlss5_amd_view /
  // dlss5_amd_signed_view pick the active view per fetch signedness.
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
  view_info.format = VK_FORMAT_R8G8B8A8_SNORM;
  if (dfn.vkCreateImageView(device.device(), &view_info, nullptr, &g_substitute_view_snorm) !=
      VK_SUCCESS) {
    return false;
  }
  g_substitute_device = device.device();
  g_substitute_destroy_view = dfn.vkDestroyImageView;
  g_substitute_destroy_image = dfn.vkDestroyImage;
  g_substitute_free_memory = dfn.vkFreeMemory;
  const bool view_unorm = REXCVAR_GET(dlss5_amd_view) == 0;
  const char* signed_view_name =
      REXCVAR_GET(dlss5_amd_signed_view) == 1   ? "R8G8B8A8_UNORM"
      : REXCVAR_GET(dlss5_amd_signed_view) == 2 ? "R8G8B8A8_SRGB"
                                                : "R8G8B8A8_SNORM";
  REXLOG_INFO("DLSS 5 NR (AMD): substitute image loaded from {} (1920x1080 RGBA8 UNORM image, "
              "unsigned view {}, signed view {})",
              path, view_unorm ? "R8G8B8A8_UNORM" : "R8G8B8A8_SRGB", signed_view_name);
  return true;
}

#if defined(REX_HAS_DLSS5_AMD) && REX_HAS_DLSS5_AMD
// Creates the persistent live-mode (dlss5_amd_mode=3) resources: the 1920x1080
// target/colour/motion images with their views, the prep pipeline and the
// descriptor ring, and transitions the images to GENERAL once (the queue is
// idle here, at setup). Called from InitializeRuntime after the runtime was
// constructed, so g_runtime is valid. A failure releases everything created so
// far and is remembered, so it is not retried.
bool CreateLiveImage(const vulkan::VulkanDevice& device, VkFormat format,
                     VkImageUsageFlags usage, VkImage& image_out, VkDeviceMemory& memory_out) {
  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent = {g_live_width, g_live_height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = usage;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  return vulkan::util::CreateDedicatedAllocationImage(
      &device, image_info, vulkan::util::MemoryPurpose::kDeviceLocal, image_out, memory_out);
}

bool CreateLiveView(const vulkan::VulkanDevice& device, VkImage image, VkFormat format,
                    VkImageView& view_out) {
  VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format;
  view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return device.functions().vkCreateImageView(device.device(), &view_info, nullptr, &view_out) ==
         VK_SUCCESS;
}

// One-time UNDEFINED -> GENERAL transition of the live images, recorded into a
// one-off command buffer submitted with a fence and waited on: at setup the
// queue is idle, and later RecordLiveFrame calls assume GENERAL.
bool TransitionLiveImages(const vulkan::VulkanDevice& device, VkQueue queue, uint32_t family) {
  const vulkan::VulkanDevice::Functions& dfn = device.functions();
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.queueFamilyIndex = family;
  pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
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
    dfn.vkDestroyCommandPool(device.device(), pool, nullptr);
    return false;
  }
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (dfn.vkCreateFence(device.device(), &fence_info, nullptr, &fence) != VK_SUCCESS) {
    dfn.vkDestroyCommandPool(device.device(), pool, nullptr);
    return false;
  }
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  dfn.vkBeginCommandBuffer(command_buffer, &begin);
  const VkImage images[5] = {g_live_target, g_live_colour, g_live_motion, g_live_depth,
                             g_live_null_depth};
  VkImageMemoryBarrier barriers[5] = {};
  for (uint32_t i = 0; i < 5; ++i) {
    barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[i].srcAccessMask = 0;
    barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[i].image = images[i];
    barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  }
  dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 3,
                           barriers);
  dfn.vkEndCommandBuffer(command_buffer);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command_buffer;
  const bool ok = dfn.vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS &&
                  dfn.vkWaitForFences(device.device(), 1, &fence, VK_TRUE, UINT64_MAX) ==
                      VK_SUCCESS;
  dfn.vkDestroyFence(device.device(), fence, nullptr);
  dfn.vkDestroyCommandPool(device.device(), pool, nullptr);
  return ok;
}

#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
// One-time UNDEFINED -> GENERAL for the three mode-4 FSR inputs (the prep
// writes them as storage images). Kept separate from TransitionLiveImages so
// mode 3's initial transition sequence is untouched.
bool TransitionFsrSceneImages(const vulkan::VulkanDevice& device, VkQueue queue, uint32_t family) {
  const vulkan::VulkanDevice::Functions& dfn = device.functions();
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.queueFamilyIndex = family;
  pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
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
    dfn.vkDestroyCommandPool(device.device(), pool, nullptr);
    return false;
  }
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (dfn.vkCreateFence(device.device(), &fence_info, nullptr, &fence) != VK_SUCCESS) {
    dfn.vkDestroyCommandPool(device.device(), pool, nullptr);
    return false;
  }
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  dfn.vkBeginCommandBuffer(command_buffer, &begin);
  const VkImage images[3] = {g_fsr_colour, g_fsr_motion, g_fsr_depth};
  VkImageMemoryBarrier barriers[3] = {};
  for (uint32_t i = 0; i < 3; ++i) {
    barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[i].srcAccessMask = 0;
    barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[i].image = images[i];
    barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  }
  dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 3,
                           barriers);
  dfn.vkEndCommandBuffer(command_buffer);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command_buffer;
  const bool ok = dfn.vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS &&
                  dfn.vkWaitForFences(device.device(), 1, &fence, VK_TRUE, UINT64_MAX) ==
                      VK_SUCCESS;
  dfn.vkDestroyFence(device.device(), fence, nullptr);
  dfn.vkDestroyCommandPool(device.device(), pool, nullptr);
  return ok;
}
#endif  // REX_HAS_FIDELITYFX_RUNTIME

// --- dlss5_live_dump helpers ---
// The guest frame the recorded live frame belongs to: the command processor
// increments this once per XE_SWAP packet it actually processes, in stream
// order, so the value read during a frame's composition draw is that frame's
// zero-based index. It is the value the presenter stamps onto
// guest_output_properties.frame_index, so a presenter (FSR) dump named with
// that index is the same guest frame as this one. GuestSwapCount() is not used:
// that is the CPU-side VdSwap count, which the command processor's own frame
// advance deliberately avoids because it can be ahead of the processed stream.
// Returns -1 when no guest runtime is attached (offline tooling).
int64_t CurrentGuestFrameIndex() {
  const rex::Runtime* const runtime = rex::Runtime::instance();
  const rex::system::IGraphicsSystem* const graphics =
      runtime != nullptr ? runtime->graphics_system() : nullptr;
  return graphics != nullptr ? int64_t(graphics->guest_frame_count()) : -1;
}

// The files are named <prefix>-f<frame>-<kind><ext>; the prefix is a path base
// (a directory plus a run name), and the frame keeps two runs' files apart.
std::string LiveDumpFilePath(const std::string& prefix, uint64_t frame, const char* kind,
                             const char* extension) {
  return prefix + "-f" + std::to_string(frame) + "-" + kind + extension;
}

std::string JsonEscape(const std::string& text) {
  std::string escaped;
  escaped.reserve(text.size());
  for (const char c : text) {
    if (c == '\\' || c == '"') {
      escaped.push_back('\\');
    }
    escaped.push_back(c);
  }
  return escaped;
}

// Hash the bytes the GPU produced, then write them raw. The hash is computed
// from the mapping rather than the file so a short write still names the bytes
// that were recorded.
bool WriteLiveDumpFile(const std::string& path, const void* data, size_t size,
                       std::string& sha256) {
  Sha256 ctx;
  Sha256Update(ctx, static_cast<const uint8_t*>(data), size);
  sha256 = Sha256Final(ctx);
  std::FILE* const file = std::fopen(path.c_str(), "wb");
  if (!file) {
    return false;
  }
  const size_t written = std::fwrite(data, 1, size, file);
  std::fclose(file);
  return written == size;
}

// Reads the pending dump back on the CPU. Called only from RecordLiveFrame,
// only when the same seam slot is handed out again - that reuse is the proof
// the submission containing the copies has completed, so nothing here waits.
void ConsumePendingLiveDump() {
  if (!g_live_dump_pending) {
    return;
  }
  g_live_dump_pending = false;
  if (g_live_dump_buffer == VK_NULL_HANDLE || g_live_dump_mapped == nullptr ||
      g_live_device == nullptr || g_live_dump_image_bytes == 0) {
    REXGPU_WARN("dlss5_live_dump: readback unavailable, frame={} dropped",
                g_live_dump_pending_frame);
    return;
  }
  const vulkan::VulkanDevice::Functions& dfn = g_live_device->functions();
  const VkDevice device = g_live_device->device();
  // The readback memory is host-visible but not necessarily coherent, so the
  // CPU view is only valid after invalidation (the motion_dump does the same).
  VkMappedMemoryRange mapped{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
  mapped.memory = g_live_dump_memory;
  mapped.offset = 0;
  mapped.size = VK_WHOLE_SIZE;
  dfn.vkInvalidateMappedMemoryRanges(device, 1, &mapped);

  const std::string& prefix = g_live_dump_prefix;
  const uint64_t frame = g_live_dump_pending_frame;
  static constexpr const char* kKinds[4] = {"colour", "motion", "depth", "answer"};
  static constexpr const char* kExtensions[4] = {".rgba", ".rg16f", ".r32f", ".rgba"};
  static constexpr const char* kFormats[4] = {"R8G8B8A8_UNORM", "R16G16_SFLOAT", "R32_SFLOAT",
                                              "R8G8B8A8_UNORM"};
  const auto* const base = static_cast<const uint8_t*>(g_live_dump_mapped);
  std::string paths[4];
  std::string shas[4];
  for (uint32_t i = 0; i < 4; ++i) {
    const VkDeviceSize bytes =
        g_live_dump_pending_bytes[i] != 0 ? g_live_dump_pending_bytes[i] : g_live_dump_image_bytes;
    paths[i] = LiveDumpFilePath(prefix, frame, kKinds[i], kExtensions[i]);
    if (!WriteLiveDumpFile(paths[i], base + VkDeviceSize(i) * g_live_dump_image_bytes,
                           size_t(bytes), shas[i])) {
      REXGPU_WARN("dlss5_live_dump: can't write {} ({} bytes)", paths[i], uint64_t(bytes));
      continue;
    }
    REXGPU_INFO("dlss5_live_dump frame={} file={} bytes={} sha256={}", frame, paths[i],
                uint64_t(bytes), shas[i]);
  }
  // The JSON is the run's index: it names the extents, formats and each file's
  // hash so a comparison script does not need the log. guest_frame is the
  // guest frame index the presenter gives the same frame, so it aligns with a
  // presenter (FSR) temporal dump of that frame; frame is the command
  // processor's own frame id (normally guest_frame + 1).
  const std::string json_path = prefix + "-f" + std::to_string(frame) + ".json";
  std::ofstream json(json_path);
  if (!json) {
    REXGPU_WARN("dlss5_live_dump: can't write {}", json_path);
    return;
  }
  json << "{\n";
  json << "  \"frame\": " << frame << ",\n";
  json << "  \"guest_frame\": " << g_live_dump_pending_guest_frame << ",\n";
  json << "  \"extent\": [" << g_live_width << ", " << g_live_height << "],\n";
  json << "  \"scene_extent\": [" << g_live_dump_pending_scene_width << ", "
       << g_live_dump_pending_scene_height << "],\n";
  json << "  \"bytes_per_image\": " << uint64_t(g_live_dump_image_bytes) << ",\n";
  json << "  \"depth_enabled\": " << (g_live_dump_pending_depth ? "true" : "false") << ",\n";
  json << "  \"answer_in_place\": " << (g_live_dump_pending_in_place ? "true" : "false") << ",\n";
  json << "  \"images\": {\n";
  for (uint32_t i = 0; i < 4; ++i) {
    const VkDeviceSize bytes =
        g_live_dump_pending_bytes[i] != 0 ? g_live_dump_pending_bytes[i] : g_live_dump_image_bytes;
    json << "    \"" << kKinds[i] << "\": {\"file\": \"" << JsonEscape(paths[i])
         << "\", \"format\": \"" << kFormats[i] << "\", \"extent\": ["
         << g_live_dump_pending_width[i] << ", " << g_live_dump_pending_height[i]
         << "], \"bytes\": " << uint64_t(bytes) << ", \"sha256\": \"" << shas[i] << "\"}"
         << (i + 1 < 4 ? "," : "") << "\n";
  }
  json << "  }\n}\n";
  json.close();
  if (!json) {
    REXGPU_WARN("dlss5_live_dump: short write to {}", json_path);
    return;
  }
  REXGPU_INFO("dlss5_live_dump frame={} guest_frame={} json={} extent={}x{} scene={}x{} depth={} "
              "answer={}",
              frame, g_live_dump_pending_guest_frame, json_path, g_live_width, g_live_height,
              g_live_dump_pending_scene_width, g_live_dump_pending_scene_height,
              g_live_dump_pending_depth ? "engine" : "none",
              g_live_dump_pending_in_place ? "in-place" : "target");
}

// Records the four dump copies into the live command buffer after the answer is
// final and restores every image to the layout the caller tracked. Returns true
// when this frame recorded the dump.
bool RecordLiveDumpFrame(VkCommandBuffer cmd, const vulkan::VulkanDevice::Functions& dfn,
                         uint64_t frame, uint32_t scene_width, uint32_t scene_height,
                         bool takes_target, bool depth_enabled, uint32_t descriptor_slot) {
  if (g_live_dump_done || g_live_dump_pending || !g_live_dump_enabled ||
      g_live_dump_buffer == VK_NULL_HANDLE || g_live_dump_image_bytes == 0) {
    return false;
  }
  // -1 is the first frame that reaches this point, i.e. the first frame live NR
  // records (record_engine or the bypass copy succeeded above).
  if (g_live_dump_frame >= 0 && uint64_t(g_live_dump_frame) != frame) {
    return false;
  }
  if (g_live_colour == VK_NULL_HANDLE || g_live_motion == VK_NULL_HANDLE ||
      g_live_depth == VK_NULL_HANDLE || g_live_substitute_image == VK_NULL_HANDLE) {
    return false;
  }
  // Colour, motion, depth and the answer at four fixed offsets. All four live
  // images are 4 bytes per pixel (RGBA8 / RG16F / R32F / RGBA8), so the offset
  // stride is one image's byte size. In-place live NR makes the answer the
  // colour image itself; slot 3 then duplicates slot 0 and the JSON says so.
  struct DumpCopy {
    VkImage image;
    VkImageLayout layout;
    VkDeviceSize offset;
  };
  const DumpCopy copies[4] = {
      {g_live_colour, g_live_colour_layout, 0},
      {g_live_motion, g_live_motion_layout, g_live_dump_image_bytes},
      {g_live_depth, g_live_depth_layout, 2 * g_live_dump_image_bytes},
      {g_live_substitute_image,
       takes_target ? g_live_target_layout : g_live_colour_layout,
       3 * g_live_dump_image_bytes},
  };
  const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageMemoryBarrier src[4] = {};
  uint32_t src_count = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    bool duplicate = false;
    for (uint32_t j = 0; j < i; ++j) {
      duplicate = duplicate || copies[j].image == copies[i].image;
    }
    if (duplicate) {
      continue;
    }
    VkImageMemoryBarrier& barrier = src[src_count++];
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                            VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = copies[i].layout;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = copies[i].image;
    barrier.subresourceRange = range;
  }
  dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, src_count,
                           src);
  for (const DumpCopy& copy : copies) {
    VkBufferImageCopy region{};
    region.bufferOffset = copy.offset;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {g_live_width, g_live_height, 1};
    dfn.vkCmdCopyImageToBuffer(cmd, copy.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               g_live_dump_buffer, 1, &region);
  }
  // Back to exactly the layout the caller tracked, so the composition draw and
  // the next frame see the same state as without the dump.
  VkImageMemoryBarrier dst[4] = {};
  uint32_t dst_count = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    bool duplicate = false;
    for (uint32_t j = 0; j < i; ++j) {
      duplicate = duplicate || copies[j].image == copies[i].image;
    }
    if (duplicate) {
      continue;
    }
    VkImageMemoryBarrier& barrier = dst[dst_count++];
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT |
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = copies[i].layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = copies[i].image;
    barrier.subresourceRange = range;
  }
  dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                           dst_count, dst);
  // Host visibility for the readback that happens after the submission completes.
  VkBufferMemoryBarrier to_host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_host.buffer = g_live_dump_buffer;
  to_host.offset = 0;
  to_host.size = VK_WHOLE_SIZE;
  dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                           nullptr, 1, &to_host, 0, nullptr);

  g_live_dump_pending = true;
  g_live_dump_done = true;
  g_live_dump_pending_slot = descriptor_slot;
  g_live_dump_pending_frame = frame;
  g_live_dump_pending_guest_frame = CurrentGuestFrameIndex();
  g_live_dump_pending_scene_width = scene_width;
  g_live_dump_pending_scene_height = scene_height;
  g_live_dump_pending_depth = depth_enabled;
  g_live_dump_pending_in_place = !takes_target;
  // Mode 3: all four images are the runtime extent with the same byte size.
  for (uint32_t i = 0; i < 4; ++i) {
    g_live_dump_pending_width[i] = g_live_width;
    g_live_dump_pending_height[i] = g_live_height;
    g_live_dump_pending_bytes[i] = g_live_dump_image_bytes;
  }
  if (REXCVAR_GET(dlss5_live_trace)) {
    REXGPU_INFO("dlss5_live_dump frame={} copies recorded ({} bytes/image, slot={})", frame,
                uint64_t(g_live_dump_image_bytes), descriptor_slot);
  }
  return true;
}

// Mode-4 dump: the FSR inputs next to the answer. In bypass the pre-upscaled
// colour is the 1920x1080 image the copy came from (answer == colour); in real
// mode colour/motion/depth are the 1000x600 scene copies FSR consumed and the
// answer is the 1920x1080 upscale. Offsets stay the mode-3 ones so one buffer
// and one readback path serve both modes.
void RecordLiveDumpFrameFsr4(VkCommandBuffer cmd, const vulkan::VulkanDevice::Functions& dfn,
                             uint64_t frame, uint32_t scene_width, uint32_t scene_height,
                             bool bypass, uint32_t descriptor_slot) {
  if (g_live_dump_done || g_live_dump_pending || !g_live_dump_enabled ||
      g_live_dump_buffer == VK_NULL_HANDLE || g_live_dump_image_bytes == 0) {
    return;
  }
  if (g_live_dump_frame >= 0 && uint64_t(g_live_dump_frame) != frame) {
    return;
  }
  if (g_live_target == VK_NULL_HANDLE) {
    return;
  }
  struct DumpCopy {
    VkImage image;
    VkImageLayout layout;
    VkDeviceSize offset;
    uint32_t width;
    uint32_t height;
    VkDeviceSize bytes;
  };
  const VkDeviceSize scene_bytes = VkDeviceSize(scene_width) * scene_height * 4u;
  DumpCopy copies[4];
  if (bypass) {
    if (g_live_colour == VK_NULL_HANDLE || g_live_motion == VK_NULL_HANDLE ||
        g_live_depth == VK_NULL_HANDLE) {
      return;
    }
    copies[0] = {g_live_colour, g_live_colour_layout, 0, g_live_width, g_live_height,
                 g_live_dump_image_bytes};
    copies[1] = {g_live_motion, g_live_motion_layout, g_live_dump_image_bytes, g_live_width,
                 g_live_height, g_live_dump_image_bytes};
    copies[2] = {g_live_depth, g_live_depth_layout, 2 * g_live_dump_image_bytes, g_live_width,
                 g_live_height, g_live_dump_image_bytes};
  } else {
    if (g_fsr_colour == VK_NULL_HANDLE || g_fsr_motion == VK_NULL_HANDLE ||
        g_fsr_depth == VK_NULL_HANDLE) {
      return;
    }
    copies[0] = {g_fsr_colour, g_fsr_colour_layout, 0, scene_width, scene_height, scene_bytes};
    copies[1] = {g_fsr_motion, g_fsr_motion_layout, g_live_dump_image_bytes, scene_width,
                 scene_height, scene_bytes};
    copies[2] = {g_fsr_depth, g_fsr_depth_layout, 2 * g_live_dump_image_bytes, scene_width,
                 scene_height, scene_bytes};
  }
  copies[3] = {g_live_target, g_live_target_layout, 3 * g_live_dump_image_bytes, g_live_width,
               g_live_height, g_live_dump_image_bytes};

  const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageMemoryBarrier src[4] = {};
  uint32_t src_count = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    bool duplicate = false;
    for (uint32_t j = 0; j < i; ++j) {
      duplicate = duplicate || copies[j].image == copies[i].image;
    }
    if (duplicate) {
      continue;
    }
    VkImageMemoryBarrier& barrier = src[src_count++];
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask =
        VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = copies[i].layout;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = copies[i].image;
    barrier.subresourceRange = range;
  }
  dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, src_count,
                           src);
  for (const DumpCopy& copy : copies) {
    VkBufferImageCopy region{};
    region.bufferOffset = copy.offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {copy.width, copy.height, 1};
    dfn.vkCmdCopyImageToBuffer(cmd, copy.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               g_live_dump_buffer, 1, &region);
  }
  VkImageMemoryBarrier dst[4] = {};
  uint32_t dst_count = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    bool duplicate = false;
    for (uint32_t j = 0; j < i; ++j) {
      duplicate = duplicate || copies[j].image == copies[i].image;
    }
    if (duplicate) {
      continue;
    }
    VkImageMemoryBarrier& barrier = dst[dst_count++];
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT |
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = copies[i].layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = copies[i].image;
    barrier.subresourceRange = range;
  }
  dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                           dst_count, dst);
  VkBufferMemoryBarrier to_host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_host.buffer = g_live_dump_buffer;
  to_host.offset = 0;
  to_host.size = VK_WHOLE_SIZE;
  dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                           nullptr, 1, &to_host, 0, nullptr);

  g_live_dump_pending = true;
  g_live_dump_done = true;
  g_live_dump_pending_slot = descriptor_slot;
  g_live_dump_pending_frame = frame;
  g_live_dump_pending_guest_frame = CurrentGuestFrameIndex();
  g_live_dump_pending_scene_width = scene_width;
  g_live_dump_pending_scene_height = scene_height;
  g_live_dump_pending_depth = true;
  g_live_dump_pending_in_place = false;
  for (uint32_t i = 0; i < 4; ++i) {
    g_live_dump_pending_width[i] = copies[i].width;
    g_live_dump_pending_height[i] = copies[i].height;
    g_live_dump_pending_bytes[i] = copies[i].bytes;
  }
  if (REXCVAR_GET(dlss5_live_trace)) {
    REXGPU_INFO("dlss5_live_dump frame={} fsr4 copies recorded ({}, slot={})", frame,
                bypass ? "bypass" : "scene inputs", descriptor_slot);
  }
}

bool InitializeLiveResources(const vulkan::VulkanDevice& device, VkQueue queue, uint32_t family) {
  if (g_live_resources_failed) {
    return false;
  }
  if (g_live_pipeline != VK_NULL_HANDLE) {
    return true;
  }
  const vulkan::VulkanDevice::Functions& dfn = device.functions();
  const VkDevice vk_device = device.device();
  g_live_device = &device;
  const bool fsr4_seam = REXCVAR_GET(dlss5_amd_mode) == 4;
  auto fail = [&](const char* what) {
    REXLOG_WARN("DLSS 5 NR (AMD): failed to create the live seam {} - live mode inactive", what);
    // Partially created handles stay set; Shutdown() releases them.
    g_live_resources_failed = true;
    return false;
  };
  // The runtime extent is what record_engine validates the frames against; the
  // images must exist at exactly that size, and the prep dispatch runs over it.
  // Mode 4 has no NR runtime: InitializeRuntime set the live extent to the
  // output size before calling this, because FSR's upscale size is the target's.
  if (!g_live_width || !g_live_height) {
    g_live_width = g_runtime->model_width();
    g_live_height = g_runtime->model_height();
  }
  if (!g_live_width || !g_live_height) {
    return fail("runtime extent");
  }
  // Target: the NR answer, substituted for the composition source (sampled by
  // the composition draw), dumpable (TRANSFER_SRC) and a write-back destination
  // (TRANSFER_DST: the runtime's transfer pass copies into it).
  if (!CreateLiveImage(device, VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       g_live_target, g_live_target_memory)) {
    return fail("target image");
  }
  // Colour: the 1920x1080 pre-upscaled scene colour record_engine consumes.
  if (!CreateLiveImage(device, VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       g_live_colour, g_live_colour_memory)) {
    return fail("colour image");
  }
  // Motion: the prep shader's R16G16_SFLOAT output, sampled in place by NR.
  if (!CreateLiveImage(device, VK_FORMAT_R16G16_SFLOAT,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, g_live_motion,
                       g_live_motion_memory)) {
    return fail("motion image");
  }
  // Depth: the prep shader's R32_SFLOAT output. record_engine either samples it
  // in place (depth_in_place) or blits it into its own depth image, so it needs
  // both SAMPLED and TRANSFER_SRC.
  if (!CreateLiveImage(device, VK_FORMAT_R32_SFLOAT,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       g_live_depth, g_live_depth_memory)) {
    return fail("depth image");
  }
  // The bound-but-unused depth source for frames without a guest depth resolve.
  {
    VkImageCreateInfo null_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    null_info.imageType = VK_IMAGE_TYPE_2D;
    null_info.format = VK_FORMAT_R32_SFLOAT;
    null_info.extent = {1, 1, 1};
    null_info.mipLevels = 1;
    null_info.arrayLayers = 1;
    null_info.samples = VK_SAMPLE_COUNT_1_BIT;
    null_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    null_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    null_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!vulkan::util::CreateDedicatedAllocationImage(
            &device, null_info, vulkan::util::MemoryPurpose::kDeviceLocal, g_live_null_depth,
            g_live_null_depth_memory)) {
      return fail("null depth image");
    }
  }
#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
  // Mode 4: FSR's render-resolution inputs. The mode-3 prep writes these in one
  // dispatch over the guest resolve, 1:1, so FSR gets the native scene colour,
  // the guide motion and the guest depth at the extent its renderSize declares.
  if (fsr4_seam) {
    struct SceneImage {
      VkFormat format;
      VkImage* image;
      VkDeviceMemory* memory;
    };
    const SceneImage scene_images[3] = {
        {VK_FORMAT_R8G8B8A8_UNORM, &g_fsr_colour, &g_fsr_colour_memory},
        {VK_FORMAT_R16G16_SFLOAT, &g_fsr_motion, &g_fsr_motion_memory},
        {VK_FORMAT_R32_SFLOAT, &g_fsr_depth, &g_fsr_depth_memory},
    };
    for (const SceneImage& scene_image : scene_images) {
      VkImageCreateInfo scene_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      scene_info.imageType = VK_IMAGE_TYPE_2D;
      scene_info.format = scene_image.format;
      scene_info.extent = {kFsrSceneWidth, kFsrSceneHeight, 1};
      scene_info.mipLevels = 1;
      scene_info.arrayLayers = 1;
      scene_info.samples = VK_SAMPLE_COUNT_1_BIT;
      scene_info.tiling = VK_IMAGE_TILING_OPTIMAL;
      scene_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      scene_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      scene_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      if (!vulkan::util::CreateDedicatedAllocationImage(
              &device, scene_info, vulkan::util::MemoryPurpose::kDeviceLocal, *scene_image.image,
              *scene_image.memory)) {
        return fail("FSR scene image");
      }
    }
  }
#endif  // REX_HAS_FIDELITYFX_RUNTIME
  // The optional determinism dump: one host-visible buffer for all four images,
  // sized for at least 1920x1080 each (every live image is 4 bytes per pixel).
  // Created and mapped here so the recorded-frame path never allocates or maps;
  // a failure only disables the dump, never live NR.
  if (!REXCVAR_GET(dlss5_live_dump).empty()) {
    const VkDeviceSize pixels = std::max<VkDeviceSize>(
        VkDeviceSize(g_live_width) * VkDeviceSize(g_live_height),
        VkDeviceSize(1920) * VkDeviceSize(1080));
    g_live_dump_image_bytes = pixels * 4;
    const VkDeviceSize dump_bytes = g_live_dump_image_bytes * 4;
    if (!vulkan::util::CreateDedicatedAllocationBuffer(
            &device, dump_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            vulkan::util::MemoryPurpose::kReadback, g_live_dump_buffer,
            g_live_dump_memory)) {
      REXLOG_WARN("DLSS 5 NR (AMD): failed to create the dlss5_live_dump readback buffer "
                  "({} bytes) - the dump is off, live NR is unaffected",
                  uint64_t(dump_bytes));
      g_live_dump_image_bytes = 0;
    } else if (dfn.vkMapMemory(vk_device, g_live_dump_memory, 0, VK_WHOLE_SIZE, 0,
                               &g_live_dump_mapped) != VK_SUCCESS) {
      g_live_dump_mapped = nullptr;
      REXLOG_WARN("DLSS 5 NR (AMD): failed to map the dlss5_live_dump readback buffer - "
                  "the dump is off, live NR is unaffected");
      vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, vk_device, g_live_dump_buffer);
      vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, vk_device, g_live_dump_memory);
      g_live_dump_image_bytes = 0;
    } else {
      g_live_dump_prefix = REXCVAR_GET(dlss5_live_dump);
      g_live_dump_frame = REXCVAR_GET(dlss5_live_dump_frame);
      g_live_dump_enabled = true;
      REXLOG_INFO("DLSS 5 NR (AMD): live determinism dump enabled, prefix {} ({} bytes, "
                  "{} bytes per image)",
                  g_live_dump_prefix, uint64_t(dump_bytes), uint64_t(g_live_dump_image_bytes));
    }
  }
  if (!CreateLiveView(device, g_live_target, VK_FORMAT_R8G8B8A8_UNORM, g_live_target_view) ||
      !CreateLiveView(device, g_live_colour, VK_FORMAT_R8G8B8A8_UNORM, g_live_colour_view) ||
      !CreateLiveView(device, g_live_motion, VK_FORMAT_R16G16_SFLOAT, g_live_motion_view) ||
      !CreateLiveView(device, g_live_depth, VK_FORMAT_R32_SFLOAT, g_live_depth_view) ||
      !CreateLiveView(device, g_live_null_depth, VK_FORMAT_R32_SFLOAT, g_live_null_depth_view)) {
    return fail("views");
  }
#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
  if (fsr4_seam &&
      (!CreateLiveView(device, g_fsr_colour, VK_FORMAT_R8G8B8A8_UNORM, g_fsr_colour_view) ||
       !CreateLiveView(device, g_fsr_motion, VK_FORMAT_R16G16_SFLOAT, g_fsr_motion_view) ||
       !CreateLiveView(device, g_fsr_depth, VK_FORMAT_R32_SFLOAT, g_fsr_depth_view))) {
    return fail("FSR scene views");
  }
#endif
  if (!TransitionLiveImages(device, queue, family)) {
    return fail("initial layout transition");
  }
  g_live_target_layout = VK_IMAGE_LAYOUT_GENERAL;
  g_live_colour_layout = VK_IMAGE_LAYOUT_GENERAL;
  g_live_motion_layout = VK_IMAGE_LAYOUT_GENERAL;
  g_live_depth_layout = VK_IMAGE_LAYOUT_GENERAL;
#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
  g_fsr_colour_layout = VK_IMAGE_LAYOUT_GENERAL;
  g_fsr_motion_layout = VK_IMAGE_LAYOUT_GENERAL;
  g_fsr_depth_layout = VK_IMAGE_LAYOUT_GENERAL;
#endif
  // The colour pre-upscale resamples the 1000x600 scene onto the runtime
  // extent; the draw's own sampler is passed per frame when available, and this
  // one (linear, clamp) is the fallback for the same mapping.
  VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.minLod = 0.0f;
  sampler_info.maxLod = 0.0f;
  if (dfn.vkCreateSampler(vk_device, &sampler_info, nullptr, &g_live_sampler) != VK_SUCCESS) {
    return fail("sampler");
  }
  // set 0: binding 0 = guide SSBO, 1 = motion storage image, 2 = colour
  // sampler, 3 = colour storage image, 4 = depth sampler, 5 = depth storage
  // image (see nr_motion_prep_cs.glsl).
  VkDescriptorSetLayoutBinding bindings[6] = {};
  bindings[0].binding = 0;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  bindings[1].binding = 1;
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  bindings[1].descriptorCount = 1;
  bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  bindings[2].binding = 2;
  bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  bindings[2].descriptorCount = 1;
  bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  bindings[3].binding = 3;
  bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  bindings[3].descriptorCount = 1;
  bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  bindings[4].binding = 4;
  bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  bindings[4].descriptorCount = 1;
  bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  bindings[5].binding = 5;
  bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  bindings[5].descriptorCount = 1;
  bindings[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo set_layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_layout_info.bindingCount = uint32_t(rex::countof(bindings));
  set_layout_info.pBindings = bindings;
  if (dfn.vkCreateDescriptorSetLayout(vk_device, &set_layout_info, nullptr, &g_live_set_layout) !=
      VK_SUCCESS) {
    return fail("descriptor set layout");
  }
  VkDescriptorPoolSize pool_sizes[3] = {};
  pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  pool_sizes[0].descriptorCount = kLiveDescriptorSlots;
  pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  pool_sizes[1].descriptorCount = 3 * kLiveDescriptorSlots;
  pool_sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  pool_sizes[2].descriptorCount = 2 * kLiveDescriptorSlots;
  g_live_pool = std::make_unique<vulkan::SingleLayoutDescriptorSetPool>(
      &device, kLiveDescriptorSlots, uint32_t(rex::countof(pool_sizes)), pool_sizes,
      g_live_set_layout);
  for (uint32_t i = 0; i < kLiveDescriptorSlots; ++i) {
    const size_t index = g_live_pool->Allocate();
    if (index == SIZE_MAX) {
      return fail("descriptor set");
    }
    g_live_sets[i] = g_live_pool->Get(index);
  }
  VkPushConstantRange push_constant_range{};
  push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  push_constant_range.offset = 0;
  push_constant_range.size = 5 * sizeof(uint32_t);
  VkPipelineLayoutCreateInfo pipeline_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pipeline_layout_info.setLayoutCount = 1;
  pipeline_layout_info.pSetLayouts = &g_live_set_layout;
  pipeline_layout_info.pushConstantRangeCount = 1;
  pipeline_layout_info.pPushConstantRanges = &push_constant_range;
  if (dfn.vkCreatePipelineLayout(vk_device, &pipeline_layout_info, nullptr,
                                 &g_live_pipeline_layout) != VK_SUCCESS) {
    return fail("pipeline layout");
  }
  g_live_pipeline = vulkan::util::CreateComputePipeline(
      &device, g_live_pipeline_layout, shaders::nr_motion_prep_cs, sizeof(shaders::nr_motion_prep_cs));
  if (g_live_pipeline == VK_NULL_HANDLE) {
    return fail("prep pipeline");
  }
#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
  if (fsr4_seam) {
    if (!TransitionFsrSceneImages(device, queue, family)) {
      return fail("FSR scene layout transition");
    }
    // The optional timestamp ring around the FSR dispatch. A queue without
    // timestamps only disables the measurement, never the seam.
    if (REXCVAR_GET(dlss5_live_fsr4_timing)) {
      const vulkan::VulkanInstance* instance = device.vulkan_instance();
      VkPhysicalDeviceProperties properties{};
      instance->functions().vkGetPhysicalDeviceProperties(device.physical_device(), &properties);
      uint32_t family_count = 0;
      instance->functions().vkGetPhysicalDeviceQueueFamilyProperties(
          device.physical_device(), &family_count, nullptr);
      std::vector<VkQueueFamilyProperties> families(family_count);
      instance->functions().vkGetPhysicalDeviceQueueFamilyProperties(
          device.physical_device(), &family_count, families.data());
      const uint32_t graphics_family = device.queue_family_graphics_compute();
      if (graphics_family < families.size() &&
          families[graphics_family].timestampValidBits != 0 &&
          properties.limits.timestampPeriod > 0.0f) {
        VkQueryPoolCreateInfo query_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query_info.queryCount = kLiveDescriptorSlots * 2;
        if (dfn.vkCreateQueryPool(vk_device, &query_info, nullptr, &g_fsr_timing_pool) ==
            VK_SUCCESS) {
          g_fsr_timestamp_period_ns = properties.limits.timestampPeriod;
          REXLOG_INFO("FSR 4 seam: GPU timestamps enabled ({} slots, period {:.2f} ns)",
                      kLiveDescriptorSlots, double(g_fsr_timestamp_period_ns));
        }
      }
      if (g_fsr_timing_pool == VK_NULL_HANDLE) {
        REXLOG_WARN("FSR 4 seam: the graphics queue has no usable timestamps; the seam still "
                    "runs, the dispatch measurement is off");
      }
    }
  }
#endif  // REX_HAS_FIDELITYFX_RUNTIME
  // Whether the runtime wants a separate answer image with the default controls;
  // that decides which view the composition substitutes. Mode 4 always writes
  // the separate 1920x1080 target (the bypass body copies into it).
  nr::Controls controls;
  const bool takes_target = fsr4_seam ? true : g_runtime->takes_target(controls);
  g_live_substitute_view = takes_target ? g_live_target_view : g_live_colour_view;
  g_live_substitute_image = takes_target ? g_live_target : g_live_colour;
  REXLOG_INFO("{}: live seam resources ready ({}x{}, target={}, {})",
              fsr4_seam ? "FSR 4" : "DLSS 5 NR (AMD)", g_live_width, g_live_height,
              takes_target ? "separate" : "in-place",
              g_live_substitute_view == g_live_target_view ? "target view" : "colour view");
  return true;
}

#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
// Mode 4 setup: load the already-staged FSR 4 provider, create its upscale
// context at the fixed guest-scene -> output extents, then build the shared
// live-seam resources (plus the scene-extent FSR inputs). Any failure is a
// named inactive reason; no Vulkan object is left half-bound in the frame.
bool InitializeFsr4SeamRuntime(const vulkan::VulkanDevice& device, VkQueue queue, uint32_t family,
                               uint32_t output_width, uint32_t output_height) {
  if (g_fsr_ready) {
    return true;
  }
  const ffx::Functions* functions = ffx::Get(ffx::Api::kVulkan);
  if (!functions) {
    SetInactive("the FidelityFX Vulkan runtime is unavailable");
    return false;
  }
  if (!ffx::IsProviderActive()) {
    const rex::ui::fsr4::Status& status = rex::ui::fsr4::GetStatus();
    SetInactive(status.reason.empty()
                    ? "dlss5_amd_mode=4 needs the FSR 4 provider .so (fsr4=true + fsr4_data_path)"
                    : status.reason);
    return false;
  }
  g_fsr_functions = functions;
  g_live_device = &device;
  g_live_width = output_width;
  g_live_height = output_height;

  ffxCreateContextDescUpscale create_desc{};
  create_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  // The guest scene resolve is perceptual UNORM (no gamma PWL anywhere on the
  // guest path), so NON_LINEAR matches the presenter's FSR context. The guide
  // motion is already jitter-removed, so jitter cancellation stays off.
  create_desc.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
  create_desc.maxRenderSize = {kFsrSceneWidth, kFsrSceneHeight};
  create_desc.maxUpscaleSize = {g_live_width, g_live_height};

  ffxCreateBackendVKDesc backend_desc{};
  backend_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
  PFN_vkGetDeviceProcAddr vk_device_proc_addr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
      device.vulkan_instance()->functions().vkGetInstanceProcAddr(
          device.vulkan_instance()->instance(), "vkGetDeviceProcAddr"));
  if (!vk_device_proc_addr) {
    SetInactive("vkGetDeviceProcAddr is unavailable for the FSR 4 seam");
    return false;
  }
  g_fsr_vk_get_device_proc_addr.store(vk_device_proc_addr, std::memory_order_relaxed);
  backend_desc.vkDevice = device.device();
  backend_desc.vkPhysicalDevice = device.physical_device();
  backend_desc.vkDeviceProcAddr = FsrVkGetDeviceProcAddrCompat;
  create_desc.header.pNext = &backend_desc.header;

  if (functions->CreateContext(&g_fsr_context, &create_desc.header, nullptr) !=
      FFX_API_RETURN_OK) {
    g_fsr_context = nullptr;
    SetInactive("the FSR 4 provider could not create a context on this device (needs Vulkan 1.3, "
                "cooperative matrix and float8)");
    return false;
  }
  ffxQueryGetProviderVersion provider_version{};
  provider_version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
  if (functions->Query(&g_fsr_context, &provider_version.header) == FFX_API_RETURN_OK &&
      provider_version.versionName != nullptr) {
    g_fsr_provider = provider_version.versionName;
  } else {
    g_fsr_provider = "unknown provider";
  }
  if (!InitializeLiveResources(device, queue, family)) {
    functions->DestroyContext(&g_fsr_context, nullptr);
    g_fsr_context = nullptr;
    SetInactive("FSR 4 seam resources could not be created");
    return false;
  }
  g_fsr_ready = true;
  // The guest must render each frame with the sub-pixel offset FSR is told
  // about, or there is no new sample to accumulate: the projection hook only
  // jitters while the temporal path reports an active render size. The
  // presenter normally sets this when its own temporal upscaler runs; mode 4
  // replaces that path, so it reports the scene extent itself. The guide's
  // MotionJitterDeltaNdc then removes the same flip-adjusted jitter, keeping
  // the motion input jitter-free as the context expects.
  rex::ui::temporal::SetActive(kFsrSceneWidth, kFsrSceneHeight);
  REXLOG_INFO("FSR 4 seam: provider {} ready, render {}x{} -> {}x{} (mode 4, guest jitter active)",
              g_fsr_provider, kFsrSceneWidth, kFsrSceneHeight, g_live_width, g_live_height);
  return true;
}
#endif  // REX_HAS_FIDELITYFX_RUNTIME
#endif  // REX_HAS_DLSS5_AMD
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

  if (REXCVAR_GET(dlss5_amd_mode) == 4) {
    // Mode 4 is the FSR 4 seam: the NR model and shader tree are not used at
    // all, so their absence must not make it inactive. The provider is loaded
    // by ffx_runtime when fsr4=true; InitializeRuntime creates the context.
#if !defined(REX_HAS_FIDELITYFX_RUNTIME) || !REX_HAS_FIDELITYFX_RUNTIME
    SetInactive("this build has no FidelityFX Vulkan runtime for the FSR 4 seam");
    return;
#else
    if (!rex::ui::fsr4::Selected()) {
      SetInactive("dlss5_amd_mode=4 needs fsr4=true (the FSR 4 provider .so)");
      return;
    }
    g_status.data_root = rex::ui::fsr4::DataRoot().string();
    g_status.state = State::kReady;
    g_status.reason.clear();
    REXLOG_INFO("DLSS 5 NR (AMD): mode 4 is the FSR 4 composition seam; provider data root {} "
                "(the NR model is not loaded)",
                g_status.data_root);
    return;
#endif
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
  if (REXCVAR_GET(dlss5_amd_mode) == 4) {
#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
    try {
      vulkan::VulkanDevice::Queue::Acquisition queue =
          device.AcquireQueue(device.queue_family_graphics_compute(), 0);
      if (!InitializeFsr4SeamRuntime(device, queue.queue(),
                                     device.queue_family_graphics_compute(), width, height)) {
        // InitializeFsr4SeamRuntime names the reason (provider, context, or
        // resources); the renderer keeps the guest composition.
        return;
      }
      g_status.gpu = device.properties().deviceName;
      g_status.state = State::kReady;
    } catch (const std::exception& e) {
      SetInactive(std::string("FSR 4 seam initialization failed: ") + e.what());
    }
#else
    SetInactive("this build has no FidelityFX Vulkan runtime for the FSR 4 seam");
#endif
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
    if (!substitute_path.empty() && REXCVAR_GET(dlss5_amd_mode) != 0 &&
        REXCVAR_GET(dlss5_amd_mode) != 3) {
      if (!UploadSubstituteImage(device, queue.queue(), device.queue_family_graphics_compute(),
                                 substitute_path)) {
        SetInactive("substitute image could not be loaded");
        return;
      }
    }
    if (REXCVAR_GET(dlss5_amd_mode) == 3 &&
        !InitializeLiveResources(device, queue.queue(), device.queue_family_graphics_compute())) {
      g_runtime.reset();
      SetInactive("live NR resources could not be created");
      return;
    }
    g_status.state = State::kReady;
    REXLOG_INFO("DLSS 5 NR (AMD): runtime built on {} at {}x{} (small model, mode {})",
                g_status.gpu, width, height, REXCVAR_GET(dlss5_amd_mode));
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
  const bool fsr4_seam = REXCVAR_GET(dlss5_amd_mode) == 4;
#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
  if (fsr4_seam) {
    // Mode 4 reported itself as the temporal path for the projection jitter;
    // drop that so a teardown does not leave the guest jittering.
    rex::ui::temporal::SetInactive();
  }
  if (g_fsr_context != nullptr && g_fsr_functions != nullptr) {
    g_fsr_functions->DestroyContext(&g_fsr_context, nullptr);
  }
  g_fsr_context = nullptr;
  g_fsr_functions = nullptr;
  g_fsr_ready = false;
  g_fsr_provider.clear();
  g_fsr_last_gpu_ms = 0.0;
  g_fsr_have_last_time = false;
  for (uint32_t i = 0; i < kLiveDescriptorSlots; ++i) {
    g_fsr_timing_recorded[i] = false;
    g_fsr_timing_frame[i] = 0;
  }
#endif
  if (g_live_device != nullptr) {
    const vulkan::VulkanDevice::Functions& dfn = g_live_device->functions();
    const VkDevice device = g_live_device->device();
#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
    vulkan::util::DestroyAndNullHandle(dfn.vkDestroyQueryPool, device, g_fsr_timing_pool);
    vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, g_fsr_colour_view);
    vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, g_fsr_motion_view);
    vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, g_fsr_depth_view);
    vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, g_fsr_colour);
    vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, g_fsr_motion);
    vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, g_fsr_depth);
    vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, g_fsr_colour_memory);
    vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, g_fsr_motion_memory);
    vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, g_fsr_depth_memory);
    g_fsr_colour_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_fsr_motion_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_fsr_depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_fsr_timestamp_period_ns = 0.0f;
#endif
    vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, g_live_pipeline);
    vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                       g_live_pipeline_layout);
    g_live_pool.reset();
    vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device, g_live_set_layout);
    vulkan::util::DestroyAndNullHandle(dfn.vkDestroySampler, device, g_live_sampler);
    if (g_live_dump_pending) {
      REXGPU_WARN("dlss5_live_dump: dropped frame={} with the live resources",
                  g_live_dump_pending_frame);
    }
    g_live_dump_pending = false;
    g_live_dump_done = false;
    g_live_dump_enabled = false;
    g_live_dump_prefix.clear();
    g_live_dump_frame = -1;
    g_live_dump_image_bytes = 0;
    if (g_live_dump_mapped != nullptr) {
      dfn.vkUnmapMemory(device, g_live_dump_memory);
      g_live_dump_mapped = nullptr;
    }
    vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, g_live_dump_buffer);
    vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, g_live_dump_memory);
    vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, g_live_target_view);
    vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, g_live_colour_view);
    vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, g_live_motion_view);
    vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, g_live_depth_view);
    vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, g_live_null_depth_view);
    vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, g_live_target);
    vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, g_live_colour);
    vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, g_live_motion);
    vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, g_live_depth);
    vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, g_live_null_depth);
    vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, g_live_target_memory);
    vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, g_live_colour_memory);
    vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, g_live_motion_memory);
    vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, g_live_depth_memory);
    vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, g_live_null_depth_memory);
    g_live_substitute_view = VK_NULL_HANDLE;
    g_live_substitute_image = VK_NULL_HANDLE;
    g_live_target_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_live_colour_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_live_motion_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_live_depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_live_width = 0;
    g_live_height = 0;
    g_live_device = nullptr;
  }
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
    if (g_substitute_view_snorm != VK_NULL_HANDLE) {
      g_substitute_destroy_view(g_substitute_device, g_substitute_view_snorm, nullptr);
      g_substitute_view_snorm = VK_NULL_HANDLE;
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
