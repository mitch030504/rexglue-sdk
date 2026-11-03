#include <cstdint>

#include <catch2/catch_test_macros.hpp>
#include <rex/graphics/pipeline/render_target/cache.h>
#include <rex/graphics/xenos.h>

namespace {

// RenderTargetKey is protected in RenderTargetCache, and its width convention is
// the one the renderer (and the depth probe's choice of the widest target) uses,
// so reach that exact function through a derived class instead of restating the
// formula in the test.
class RenderTargetKeyAccess : public rex::graphics::RenderTargetCache {
 public:
  using RenderTargetCache::RenderTargetKey;
};

}  // namespace

TEST_CASE("render target width is 80 samples per 32bpp tile, halved only at 4x", "[graphics]") {
  using RenderTargetKey = RenderTargetKeyAccess::RenderTargetKey;
  // 2x MSAA is 1x2 samples per pixel, so all 80 sample columns of a tile remain
  // pixel columns; 4x is 2x2 and leaves 40. A 13-tile 2x depth target is
  // therefore 1040 px wide, which is the width the depth probe must log and
  // compare - halving at 2x reported it as 520 and picked the wrong target.
  CHECK(RenderTargetKey::GetWidth(13, rex::graphics::xenos::MsaaSamples::k1X) == 1040);
  CHECK(RenderTargetKey::GetWidth(13, rex::graphics::xenos::MsaaSamples::k2X) == 1040);
  CHECK(RenderTargetKey::GetWidth(13, rex::graphics::xenos::MsaaSamples::k4X) == 520);
}
