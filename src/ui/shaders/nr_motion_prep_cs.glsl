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

// Live NR (dlss5_amd_mode=3) input preparation at the composition seam.
//
// One dispatch prepares both NR inputs from the guest scene:
//
// - Motion: the Phase 2 motion guide's resolve output is a scene_extent buffer
//   of uvec2 words (motion_resolve_cs.glsl packs RGBA16F as low =
//   half2(x, y) forward guest-NDC motion, high = half2(z, w) with w =
//   validity). NR wants one R16G16_SFLOAT image at the evaluation extent, so
//   each output pixel maps linearly onto the scene extent and bilinear-samples
//   the packed buffer. A pixel whose interpolated validity is 0 gets zero
//   motion: NR must read a static frame there, never a stale vector from
//   another pixel.
//
// - Colour: the guest composition source (base page 0x11C6F, scene_extent) is
//   sampled through its own view over the whole extent - the composition fetch
//   maps the full texture onto the full output, so normalized UVs reproduce
//   exactly what the composition would show - and stored into an RGBA8 image at
//   the evaluation extent, the size record_engine() requires. The store is a
//   resample, so the filter is the sampler's (the composition draw's own), which
//   makes the live identity control exact.
//
// Extents are push constants because the runtime extent follows the output
// resolution and the scene extent is the guest resolve, not a constant: the
// dispatch is rounded up to whole 8x8 tiles, so an invocation past the output
// must not write.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform PushConstants {
  // The NR evaluation extent (record_engine's width/height).
  uvec2 out_extent;
  // The guest composition source's extent (the scene resolve, 1000x600 today).
  uvec2 scene_extent;
} pc;

// set 0, binding 0: the motion guide output buffer, packed uvec2 words, one
// row of scene_extent.x at the start of the buffer.
layout(std430, set = 0, binding = 0) readonly buffer GuideIn {
  uvec2 pixels[];
};

// set 0, binding 1: the evaluation-extent R16G16_SFLOAT motion image NR consumes.
layout(rg16f, set = 0, binding = 1) writeonly uniform image2D motion_out;

// set 0, binding 2: the guest composition source view (sampled, never written).
layout(set = 0, binding = 2) uniform sampler2D colour_src;

// set 0, binding 3: the evaluation-extent R8G8B8A8_UNORM pre-upscaled colour NR
// consumes.
layout(rgba8, set = 0, binding = 3) writeonly uniform image2D colour_out;

vec4 unpack_guide(uvec2 word) {
  // Low word: packHalf2x16(motion.xy). High word: packHalf2x16(motion.zw),
  // whose y (the .w of the guide's RGBA16F) is the validity flag.
  return vec4(unpackHalf2x16(word.x), unpackHalf2x16(word.y));
}

void main() {
  const uvec2 px = gl_GlobalInvocationID.xy;
  if (px.x >= pc.out_extent.x || px.y >= pc.out_extent.y) {
    return;
  }
  // Output pixel centre in the scene's texel space.
  const vec2 scene_pos = (vec2(px) + 0.5) * vec2(pc.scene_extent) / vec2(pc.out_extent);
  // Bilinear fetch of the packed guide, aligned to texel centres and clamped
  // to the scene extent so the output border never reads out of the buffer.
  const vec2 aligned = scene_pos - 0.5;
  const ivec2 base = ivec2(floor(aligned));
  const ivec2 p0 = clamp(base, ivec2(0), ivec2(pc.scene_extent) - 1);
  const ivec2 p1 = min(p0 + 1, ivec2(pc.scene_extent) - 1);
  const vec2 f = clamp(aligned - vec2(base), 0.0, 1.0);
  const uint row0 = uint(p0.y) * pc.scene_extent.x;
  const uint row1 = uint(p1.y) * pc.scene_extent.x;
  const vec4 c00 = unpack_guide(pixels[row0 + uint(p0.x)]);
  const vec4 c10 = unpack_guide(pixels[row0 + uint(p1.x)]);
  const vec4 c01 = unpack_guide(pixels[row1 + uint(p0.x)]);
  const vec4 c11 = unpack_guide(pixels[row1 + uint(p1.x)]);
  const vec4 m = mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
  vec2 motion = m.xy;
  if (m.w <= 0.0) {
    motion = vec2(0.0);
  }
  imageStore(motion_out, ivec2(px), vec4(motion, 0.0, 1.0));
  // Colour: the same linear scene-to-output mapping as the composition fetch.
  const vec2 cuv = (vec2(px) + 0.5) / vec2(pc.out_extent);
  imageStore(colour_out, ivec2(px), textureLod(colour_src, cuv, 0.0));
}
