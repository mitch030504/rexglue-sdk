// Motion vectors for the temporal upscalers, estimated from the guest output alone (block
// matching), since a recompiled game hands the presenter no depth or motion of its own.
//
// Output: per pixel, previous position minus current position, in pixels (the upscaler is given a
// motion vector scale of 1). Blocks that match as well without moving keep zero motion, so the
// static HUD baked into the image isn't smeared.
//
// Pass 0: quarter-resolution luma of the current frame.
// Pass 1: per 16x16 block, coarse search in quarter-resolution luma against the previous frame.
// Pass 2: per 8x8 block, refine around the coarse vector (and zero) at full resolution, with a
//         sub-pixel parabola fit, and write the vector to every pixel of the block.
//
// Rebuild the header with:
//   glslangValidator -V -S comp --vn temporal_motion_cs
//     -o vulkan_spirv/temporal_motion_cs.h temporal_motion_cs.glsl
#version 450
#extension GL_EXT_samplerless_texture_functions : require

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform Constants {
  uvec2 size;  // render size in pixels
  uint pass_index;
};

layout(set = 0, binding = 0) uniform texture2D current_color;
layout(set = 0, binding = 1) uniform texture2D previous_color;
layout(set = 0, binding = 2, r16f) uniform image2D current_luma_quarter;
layout(set = 0, binding = 3, r16f) uniform readonly image2D previous_luma_quarter;
layout(set = 0, binding = 4, rg16f) uniform image2D coarse_motion;
layout(set = 0, binding = 5, rg16f) uniform writeonly image2D motion;

const int kCoarseBlock = 4;     // quarter pixels, so 16x16 full pixels
const int kCoarseRadius = 6;    // quarter pixels, so +-24 full pixels
const int kFineBlock = 8;       // full pixels
const int kFineRadius = 2;      // full pixels around each candidate
// Moving must beat standing still by this much per pixel, or the block stays at zero.
const float kZeroBias = 0.004;

float Luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

float CurrentLuma(ivec2 p) {
  return Luma(texelFetch(current_color, clamp(p, ivec2(0), ivec2(size) - 1), 0).rgb);
}

float PreviousLuma(ivec2 p) {
  return Luma(texelFetch(previous_color, clamp(p, ivec2(0), ivec2(size) - 1), 0).rgb);
}

float FineCost(ivec2 origin, ivec2 offset) {
  float cost = 0.0;
  for (int y = 0; y < kFineBlock; y++) {
    for (int x = 0; x < kFineBlock; x++) {
      ivec2 p = origin + ivec2(x, y);
      cost += abs(CurrentLuma(p) - PreviousLuma(p + offset));
    }
  }
  return cost / float(kFineBlock * kFineBlock);
}

void main() {
  ivec2 id = ivec2(gl_GlobalInvocationID.xy);
  ivec2 quarter_size = max(ivec2(size) / 4, ivec2(1));

  if (pass_index == 0u) {
    if (any(greaterThanEqual(id, quarter_size))) return;
    float sum = 0.0;
    for (int y = 0; y < 4; y++) {
      for (int x = 0; x < 4; x++) sum += CurrentLuma(id * 4 + ivec2(x, y));
    }
    imageStore(current_luma_quarter, id, vec4(sum / 16.0));
    return;
  }

  if (pass_index == 1u) {
    ivec2 blocks = (quarter_size + kCoarseBlock - 1) / kCoarseBlock;
    if (any(greaterThanEqual(id, blocks))) return;
    ivec2 origin = id * kCoarseBlock;
    float best_cost = 1e9;
    ivec2 best = ivec2(0);
    for (int dy = -kCoarseRadius; dy <= kCoarseRadius; dy++) {
      for (int dx = -kCoarseRadius; dx <= kCoarseRadius; dx++) {
        float cost = 0.0;
        for (int y = 0; y < kCoarseBlock; y++) {
          for (int x = 0; x < kCoarseBlock; x++) {
            ivec2 p = min(origin + ivec2(x, y), quarter_size - 1);
            ivec2 q = clamp(p + ivec2(dx, dy), ivec2(0), quarter_size - 1);
            cost += abs(imageLoad(current_luma_quarter, p).r -
                        imageLoad(previous_luma_quarter, q).r);
          }
        }
        cost /= float(kCoarseBlock * kCoarseBlock);
        if (dx != 0 || dy != 0) cost += kZeroBias;
        if (cost < best_cost) {
          best_cost = cost;
          best = ivec2(dx, dy);
        }
      }
    }
    imageStore(coarse_motion, id, vec4(vec2(best * 4), 0.0, 0.0));
    return;
  }

  ivec2 blocks = (ivec2(size) + kFineBlock - 1) / kFineBlock;
  if (any(greaterThanEqual(id, blocks))) return;
  ivec2 origin = id * kFineBlock;
  // 8x8 blocks pair up inside the 16x16 coarse blocks.
  ivec2 coarse_size = (quarter_size + kCoarseBlock - 1) / kCoarseBlock;
  ivec2 coarse = ivec2(imageLoad(coarse_motion, min(id / 2, coarse_size - 1)).xy);

  float best_cost = FineCost(origin, ivec2(0));
  ivec2 best = ivec2(0);
  ivec2 centers[2] = ivec2[2](ivec2(0), coarse);
  for (int c = 0; c < 2; c++) {
    if (c == 1 && coarse == ivec2(0)) break;
    for (int dy = -kFineRadius; dy <= kFineRadius; dy++) {
      for (int dx = -kFineRadius; dx <= kFineRadius; dx++) {
        ivec2 offset = centers[c] + ivec2(dx, dy);
        if (offset == ivec2(0)) continue;
        float cost = FineCost(origin, offset) + kZeroBias;
        if (cost < best_cost) {
          best_cost = cost;
          best = offset;
        }
      }
    }
  }

  // Sub-pixel: fit a parabola through the costs either side of the best match on each axis.
  vec2 sub = vec2(0.0);
  if (best != ivec2(0)) {
    float c0 = best_cost - kZeroBias;
    float xm = FineCost(origin, best - ivec2(1, 0)), xp = FineCost(origin, best + ivec2(1, 0));
    float ym = FineCost(origin, best - ivec2(0, 1)), yp = FineCost(origin, best + ivec2(0, 1));
    float dx2 = xm - 2.0 * c0 + xp, dy2 = ym - 2.0 * c0 + yp;
    if (dx2 > 1e-6) sub.x = clamp((xm - xp) / (2.0 * dx2), -0.5, 0.5);
    if (dy2 > 1e-6) sub.y = clamp((ym - yp) / (2.0 * dy2), -0.5, 0.5);
  }
  vec2 vector = vec2(best) + sub;
  for (int y = 0; y < kFineBlock; y++) {
    for (int x = 0; x < kFineBlock; x++) {
      ivec2 p = origin + ivec2(x, y);
      if (all(lessThan(p, ivec2(size)))) imageStore(motion, p, vec4(vector, 0.0, 0.0));
    }
  }
}
