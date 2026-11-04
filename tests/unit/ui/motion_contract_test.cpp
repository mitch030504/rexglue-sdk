#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <rex/ui/motion_contract.h>

namespace {

using Catch::Approx;
using rex::ui::kMotionNdcToBackwardUvScaleX;
using rex::ui::kMotionNdcToBackwardUvScaleY;
using rex::ui::MotionJitterDeltaNdc;
using rex::ui::MotionNdcToBackwardUv;

TEST_CASE("motion contract maps a zero forward delta to zero backward motion", "[graphics]") {
  float u = 1.0f;
  float v = 1.0f;
  MotionNdcToBackwardUv(0.0f, 0.0f, u, v);
  CHECK(u == 0.0f);
  CHECK(v == 0.0f);
}

TEST_CASE("motion contract maps guest NDC to backward top-left UV", "[graphics]") {
  // One pixel right at 1000 wide is +2/1000 guest NDC. The previous position is
  // to the left, so backward u is negative.
  float u = 0.0f;
  float v = 0.0f;
  MotionNdcToBackwardUv(2.0f / 1000.0f, 0.0f, u, v);
  CHECK(u == Approx(-1.0f / 1000.0f));
  CHECK(v == Approx(0.0f));

  // One pixel up at 600 high is +2/600 guest NDC (y up). In top-left UV, the
  // previous position is below the current one, so backward v is positive.
  MotionNdcToBackwardUv(0.0f, 2.0f / 600.0f, u, v);
  CHECK(u == Approx(0.0f));
  CHECK(v == Approx(1.0f / 600.0f));

  // The full extent: +2 NDC right is one frame left in backward UV; +2 NDC up is
  // one frame down.
  MotionNdcToBackwardUv(2.0f, 2.0f, u, v);
  CHECK(u == Approx(-1.0f));
  CHECK(v == Approx(1.0f));
}

TEST_CASE("motion contract is exactly the runtime motion_scale", "[graphics]") {
  // If the bridge feeds the raw forward-NDC field to EngineFrame, it must set
  // motion_scale to this pair and leave the field alone. A bridge that bakes the
  // conversion must instead use (1, 1).
  for (float x : {-1.5f, -0.25f, 0.0f, 0.25f, 1.5f}) {
    for (float y : {-1.5f, -0.25f, 0.0f, 0.25f, 1.5f}) {
      float u = 0.0f;
      float v = 0.0f;
      MotionNdcToBackwardUv(x, y, u, v);
      CHECK(u == Approx(kMotionNdcToBackwardUvScaleX * x));
      CHECK(v == Approx(kMotionNdcToBackwardUvScaleY * y));
    }
  }
}

TEST_CASE("motion contract carries a residual jitter term through unchanged", "[graphics]") {
  // The guide subtracts the projection-jitter delta before storing. A residual
  // (for example a sign error, or an inactive upscaler that moved the guest
  // projection) must be converted with the same sign as scene motion, not
  // dropped at the boundary.
  const float motion_x = -0.010f;
  const float motion_y = 0.004f;
  const float jitter_x = 0.0005f;
  const float jitter_y = -0.0002f;
  float whole_u = 0.0f;
  float whole_v = 0.0f;
  MotionNdcToBackwardUv(motion_x + jitter_x, motion_y + jitter_y, whole_u, whole_v);
  float motion_u = 0.0f;
  float motion_v = 0.0f;
  MotionNdcToBackwardUv(motion_x, motion_y, motion_u, motion_v);
  float jitter_u = 0.0f;
  float jitter_v = 0.0f;
  MotionNdcToBackwardUv(jitter_x, jitter_y, jitter_u, jitter_v);
  CHECK(whole_u == Approx(motion_u + jitter_u));
  CHECK(whole_v == Approx(motion_v + jitter_v));
}

TEST_CASE("motion contract matches the analyzer's forward pixel convention", "[graphics]") {
  // The diagnostic analyzer converts forward NDC to pixels with 2/extent. The
  // backward UV delta is the same pixel delta with the opposite sign and divided
  // by the extent, which is what the runtime's normalization expects.
  const float ndc_x = 0.006f;
  const float analyzer_px = ndc_x * (1000.0f / 2.0f);
  float u = 0.0f;
  float v = 0.0f;
  MotionNdcToBackwardUv(ndc_x, 0.0f, u, v);
  CHECK(-u * 1000.0f == Approx(analyzer_px));
}

TEST_CASE("motion contract pins the measured jitter sign", "[graphics]") {
  // 1920x1080 guest output, one frame apart. The hook moves the frustum window by
  // +j, so the stored clip moves by -2j/extent; the delta to subtract is the
  // opposite. The x term matches the measured static residual sign, and the y
  // term uses the height (not the width) or it is off by the aspect ratio.
  float x = 0.0f;
  float y = 0.0f;
  MotionJitterDeltaNdc(0.5f, -0.25f, 0.25f, 0.5f, 1920.0f, 1080.0f, x, y);
  CHECK(x == Approx((0.25f - 0.5f) * 2.0f / 1920.0f));
  CHECK(y == Approx((0.5f + 0.25f) * 2.0f / 1080.0f));

  // Swapping the frames negates the delta.
  float neg_x = 0.0f;
  float neg_y = 0.0f;
  MotionJitterDeltaNdc(0.25f, 0.5f, 0.5f, -0.25f, 1920.0f, 1080.0f, neg_x, neg_y);
  CHECK(neg_x == Approx(-x));
  CHECK(neg_y == Approx(-y));
}

}  // namespace
