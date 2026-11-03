/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#version 450

// Resolves the Phase 2 motion guide attachment into one linear, single-sample
// scene rectangle.
//
// The guide is a fifth host colour target written by the scene draws, at the
// host pass extent (wider than the scene, and 2x MSAA on this title). NR needs a
// 1000x600 single-sample RGBA16F image, so this dispatch fetches exactly the
// scene rectangle's top-left corner and packs each half-float pair into one
// uvec2 word: (x, y) motion in the low word, (z, w) validity in the high word.
// It is a crop of the attachment, never a scale - the unused attachment area
// holds other EDRAM data and must not be resampled into the guide.
//
// Sample 0 is read explicitly, matching the host depth guide's sample choice:
// texelFetch on a sampler2DMS with a constant sample index is sample-count
// agnostic (the guide is 2x here, 4x under 2x-emulated-as-4x), so one shader
// serves every pass the guide is attached to.
//
// One frame is split across render pass keys, so the guide attachment exists per
// framebuffer and the composite is several dispatches into the same output. The
// first dispatch (keep_existing = 0) overwrites the whole output, so the pixels
// that pass left invalid are invalid. Every later dispatch (keep_existing = 1)
// writes only where its own sample is valid, so a later pass fills holes without
// erasing motion an earlier pass wrote.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform MotionPush {
  uint keep_existing;
} push;

layout(set = 0, binding = 0) uniform sampler2DMS motion_image;

layout(std430, set = 0, binding = 1) writeonly buffer MotionOut {
  // x + y * 1000, one RGBA16F pixel each.
  uvec2 pixels[];
};

const uint kSceneWidth = 1000u;
const uint kSceneHeight = 600u;

void main() {
  const uvec2 pixel = gl_GlobalInvocationID.xy;
  // The dispatch is rounded up to whole 8x8 tiles, so an invocation past the
  // scene rectangle must not write: the buffer is exactly the extent, not padded.
  if (pixel.x >= kSceneWidth || pixel.y >= kSceneHeight) {
    return;
  }
  const vec4 motion = texelFetch(motion_image, ivec2(pixel), 0);
  // w is the validity flag. A later pass must not erase an earlier pass's motion
  // with a pixel it never wrote, but a later valid sample does win.
  if (push.keep_existing != 0u && !(motion.w > 0.0)) {
    return;
  }
  // Half precision is the attachment's own storage precision, so the pack is
  // lossless for it and the dump is byte-for-byte the guide's values.
  pixels[pixel.y * kSceneWidth + pixel.x] =
      uvec2(packHalf2x16(motion.xy), packHalf2x16(motion.zw));
}
