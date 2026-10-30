// Motion vectors for the temporal upscalers on D3D12, estimated from the guest output alone (block
// matching). The same algorithm as temporal_motion_cs.glsl (Vulkan); see that file for details.
//
// Output: per pixel, previous position minus current position, in pixels. Pass 0 also fills the
// constant depth, and without history writes zero motion instead of the other passes.
//
// D3D12 only guarantees typed UAV loads for 32-bit single-channel formats, so the quarter luma is
// R32_FLOAT and the coarse vectors are packed into R32_UINT.
//
// Rebuild the header with (fxc from the Windows SDK, under Wine on Linux):
//   fxc /nologo /T cs_5_1 /E main /O3 /Vn temporal_motion_cs
//     /Fh bytecode/d3d12_5_1/temporal_motion_cs.h temporal_motion_cs.hlsl

cbuffer Constants : register(b0) {
  uint2 size;  // render size in pixels
  uint pass_index;
  uint has_history;
};

Texture2D<float4> current_color : register(t0);
Texture2D<float4> previous_color : register(t1);
RWTexture2D<float> current_luma_quarter : register(u0);
RWTexture2D<float> previous_luma_quarter : register(u1);
RWTexture2D<uint> coarse_motion : register(u2);
RWTexture2D<float2> motion : register(u3);
RWTexture2D<float> depth : register(u4);

static const int kCoarseBlock = 4;   // quarter pixels, so 16x16 full pixels
static const int kCoarseRadius = 6;  // quarter pixels, so +-24 full pixels
static const int kFineBlock = 8;     // full pixels
static const int kFineRadius = 2;    // full pixels around each candidate
// Moving must beat standing still by this much per pixel, or the block stays at zero.
static const float kZeroBias = 0.004;
// Constant depth handed to the upscaler, which requires one but gets nothing better here.
static const float kConstantDepth = 0.5;

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float CurrentLuma(int2 p) {
  return Luma(current_color.Load(int3(clamp(p, int2(0, 0), int2(size) - 1), 0)).rgb);
}

float PreviousLuma(int2 p) {
  return Luma(previous_color.Load(int3(clamp(p, int2(0, 0), int2(size) - 1), 0)).rgb);
}

float FineCost(int2 origin, int2 offset) {
  float cost = 0.0;
  [loop] for (int y = 0; y < kFineBlock; y++) {
    [loop] for (int x = 0; x < kFineBlock; x++) {
      int2 p = origin + int2(x, y);
      cost += abs(CurrentLuma(p) - PreviousLuma(p + offset));
    }
  }
  return cost / float(kFineBlock * kFineBlock);
}

uint PackOffset(int2 offset) { return uint(offset.x + 32768) | (uint(offset.y + 32768) << 16); }

int2 UnpackOffset(uint packed) {
  return int2(int(packed & 0xFFFFu) - 32768, int(packed >> 16) - 32768);
}

[numthreads(8, 8, 1)] void main(uint3 dispatch_id : SV_DispatchThreadID) {
  int2 id = int2(dispatch_id.xy);
  int2 quarter_size = max(int2(size) / 4, int2(1, 1));

  if (pass_index == 0u) {
    if (any(id >= quarter_size)) return;
    float sum = 0.0;
    for (int y = 0; y < 4; y++) {
      for (int x = 0; x < 4; x++) {
        int2 p = id * 4 + int2(x, y);
        sum += CurrentLuma(p);
        if (all(p < int2(size))) {
          depth[p] = kConstantDepth;
          if (!has_history) motion[p] = float2(0.0, 0.0);
        }
      }
    }
    current_luma_quarter[id] = sum / 16.0;
    // The quarter grid rounds down; fill the leftover right and bottom edge pixels too.
    int2 edge = quarter_size * 4;
    for (int ey = 0; ey < 4; ey++) {
      for (int ex = 0; ex < 4; ex++) {
        int2 extra[2] = {int2(edge.x + ex, id.y * 4 + ey), int2(id.x * 4 + ex, edge.y + ey)};
        for (int e = 0; e < 2; e++) {
          bool owner = e == 0 ? id.x == quarter_size.x - 1 : id.y == quarter_size.y - 1;
          int2 p = extra[e];
          if (owner && all(p < int2(size))) {
            depth[p] = kConstantDepth;
            if (!has_history) motion[p] = float2(0.0, 0.0);
          }
        }
      }
    }
    return;
  }

  if (pass_index == 1u) {
    int2 blocks = (quarter_size + kCoarseBlock - 1) / kCoarseBlock;
    if (any(id >= blocks)) return;
    int2 origin = id * kCoarseBlock;
    float best_cost = 1e9;
    int2 best = int2(0, 0);
    [loop] for (int dy = -kCoarseRadius; dy <= kCoarseRadius; dy++) {
      [loop] for (int dx = -kCoarseRadius; dx <= kCoarseRadius; dx++) {
        float cost = 0.0;
        for (int y = 0; y < kCoarseBlock; y++) {
          for (int x = 0; x < kCoarseBlock; x++) {
            int2 p = min(origin + int2(x, y), quarter_size - 1);
            int2 q = clamp(p + int2(dx, dy), int2(0, 0), quarter_size - 1);
            cost += abs(current_luma_quarter[p] - previous_luma_quarter[q]);
          }
        }
        cost /= float(kCoarseBlock * kCoarseBlock);
        if (dx != 0 || dy != 0) cost += kZeroBias;
        if (cost < best_cost) {
          best_cost = cost;
          best = int2(dx, dy);
        }
      }
    }
    coarse_motion[id] = PackOffset(best * 4);
    return;
  }

  int2 blocks = (int2(size) + kFineBlock - 1) / kFineBlock;
  if (any(id >= blocks)) return;
  int2 origin = id * kFineBlock;
  // 8x8 blocks pair up inside the 16x16 coarse blocks.
  int2 coarse_size = (quarter_size + kCoarseBlock - 1) / kCoarseBlock;
  int2 coarse = UnpackOffset(coarse_motion[min(id / 2, coarse_size - 1)]);

  float best_cost = FineCost(origin, int2(0, 0));
  int2 best = int2(0, 0);
  [loop] for (int c = 0; c < 2; c++) {
    if (c == 1 && all(coarse == int2(0, 0))) break;
    int2 center = c == 0 ? int2(0, 0) : coarse;
    [loop] for (int dy = -kFineRadius; dy <= kFineRadius; dy++) {
      [loop] for (int dx = -kFineRadius; dx <= kFineRadius; dx++) {
        int2 offset = center + int2(dx, dy);
        if (all(offset == int2(0, 0))) continue;
        float cost = FineCost(origin, offset) + kZeroBias;
        if (cost < best_cost) {
          best_cost = cost;
          best = offset;
        }
      }
    }
  }

  // Sub-pixel: fit a parabola through the costs either side of the best match on each axis.
  float2 sub = float2(0.0, 0.0);
  if (any(best != int2(0, 0))) {
    float c0 = best_cost - kZeroBias;
    float xm = FineCost(origin, best - int2(1, 0)), xp = FineCost(origin, best + int2(1, 0));
    float ym = FineCost(origin, best - int2(0, 1)), yp = FineCost(origin, best + int2(0, 1));
    float dx2 = xm - 2.0 * c0 + xp, dy2 = ym - 2.0 * c0 + yp;
    if (dx2 > 1e-6) sub.x = clamp((xm - xp) / (2.0 * dx2), -0.5, 0.5);
    if (dy2 > 1e-6) sub.y = clamp((ym - yp) / (2.0 * dy2), -0.5, 0.5);
  }
  float2 result = float2(best) + sub;
  for (int y = 0; y < kFineBlock; y++) {
    for (int x = 0; x < kFineBlock; x++) {
      int2 p = origin + int2(x, y);
      if (all(p < int2(size))) motion[p] = result;
    }
  }
}
