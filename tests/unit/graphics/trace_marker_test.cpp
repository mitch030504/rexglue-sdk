#include <array>
#include <catch2/catch_test_macros.hpp>
#include <rex/graphics/trace_marker.h>

using rex::graphics::TraceMarker;

TEST_CASE("PM4 diagnostic marker requires an exact supported payload", "[graphics]") {
  std::array<uint32_t, 6> words{TraceMarker::kMagic, 1, TraceMarker::kInstance,
                                1200, 0xB9222A04, 0xB90EB070};
  auto marker = TraceMarker::Parse(words);
  REQUIRE(marker);
  CHECK(marker->frame == 1200);
  CHECK(marker->node == 0xB9222A04);
  CHECK(marker->geometry == 0xB90EB070);
  CHECK(TraceMarker::kPacket == 0xC0051000);
  CHECK_FALSE(TraceMarker::Parse(std::span(words).first(5)));
  words[0] = 0;
  CHECK_FALSE(TraceMarker::Parse(words));
  words[0] = TraceMarker::kMagic;
  words[1] = 2;
  CHECK_FALSE(TraceMarker::Parse(words));
  words[1] = 1;
  words[2] = 0;
  CHECK_FALSE(TraceMarker::Parse(words));
  words[2] = 5;
  CHECK_FALSE(TraceMarker::Parse(words));
}
