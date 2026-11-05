#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <rex/cvar.h>
#include <rex/graphics/frame_rate_limit.h>
#include <rex/graphics/video_mode_util.h>  // also brings rex/ui/flags.h ('resolution' declare)
#include <rex/ui/display_mode.h>

// The 'resolution' and 'frame_rate_limit' cvars are defined by the SDK itself
// (src/ui/window.cpp / src/ui/presenter.cpp), compiled into rexruntime, which
// unit_tests links -- so declare the shared storages here instead of defining
// duplicates (a second registration would be ignored and split the storage).
REXCVAR_DECLARE(int32_t, frame_rate_limit);

namespace {

using rex::graphics::GetEffectiveFrameRateLimit;
using rex::ui::display_mode::GetNativeDisplayMode;
using rex::ui::display_mode::SetNativeDisplayMode;
namespace video_mode_util = rex::graphics::video_mode_util;

void SetFrameRateLimit(int32_t value) { REXCVAR_SET(frame_rate_limit, value); }

bool Resolve(std::string_view value, int32_t& width_out, int32_t& height_out) {
  return video_mode_util::TryResolveResolutionPreset(value, width_out, height_out);
}

// Catch2 runs test cases in random order and the native display mode is a
// process-global with no "unset" API, so the never-set behavior can only be
// observed before main() runs -- capture it at static-init time, before any
// test case can call SetNativeDisplayMode, and assert on the snapshot from a
// test case below.
struct UnsetSnapshot {
  bool get_reports_unset = false;
  bool native_aliases_fall_back_to_1920x1080 = false;
  bool oversized_wxh_passes_through = false;
  int32_t effective_limit_while_unset = 0;
  bool positive_limit_unclamped_while_unset = false;
};
UnsetSnapshot g_unset_snapshot;

[[maybe_unused]] const bool g_unset_snapshot_initialized = [] {
  auto& s = g_unset_snapshot;

  int32_t width = 0;
  int32_t height = 0;
  double refresh_hz = 0.0;
  s.get_reports_unset = !GetNativeDisplayMode(width, height, refresh_hz);

  bool aliases_ok = true;
  for (std::string_view alias : {"native", "display", "desktop"}) {
    int32_t alias_width = 0;
    int32_t alias_height = 0;
    if (!video_mode_util::TryResolveResolutionPreset(alias, alias_width, alias_height) ||
        alias_width != 1920 || alias_height != 1080) {
      aliases_ok = false;
    }
  }
  s.native_aliases_fall_back_to_1920x1080 = aliases_ok;

  int32_t big_width = 0;
  int32_t big_height = 0;
  s.oversized_wxh_passes_through =
      video_mode_util::TryResolveResolutionPreset("5120x2880", big_width, big_height) &&
      big_width == 5120 && big_height == 2880;

  SetFrameRateLimit(240);
  s.effective_limit_while_unset = GetEffectiveFrameRateLimit();
  s.positive_limit_unclamped_while_unset = (s.effective_limit_while_unset == 240);
  SetFrameRateLimit(0);  // restore the compiled-in default
  return true;
}();

TEST_CASE("with the native display mode never set, GetNativeDisplayMode reports unset, "
          "native aliases fall back to 1920x1080 and nothing is clamped",
          "[graphics][resolution]") {
  // Values captured at static-init time, before any test could set the mode.
  CHECK(g_unset_snapshot.get_reports_unset);
  CHECK(g_unset_snapshot.native_aliases_fall_back_to_1920x1080);
  CHECK(g_unset_snapshot.oversized_wxh_passes_through);
  CHECK(g_unset_snapshot.effective_limit_while_unset == 240);
  CHECK(g_unset_snapshot.positive_limit_unclamped_while_unset);
}

TEST_CASE("TryParseResolutionPreset parses presets and WxH values", "[graphics][resolution]") {
  int32_t width = 0;
  int32_t height = 0;

  SECTION("named presets") {
    const std::pair<std::string_view, std::pair<int32_t, int32_t>> cases[] = {
        {"480p", {640, 480}},    {"540p", {960, 540}},    {"720p", {1280, 720}},
        {"900p", {1600, 900}},   {"1080p", {1920, 1080}}, {"1440p", {2560, 1440}},
        {"1800p", {3200, 1800}}, {"2160p", {3840, 2160}}, {"4k", {3840, 2160}},
    };
    for (const auto& [input, expected] : cases) {
      REQUIRE(video_mode_util::TryParseResolutionPreset(input, width, height));
      CHECK(width == expected.first);
      CHECK(height == expected.second);
    }
  }

  SECTION("custom WxH, case- and space-insensitive") {
    REQUIRE(video_mode_util::TryParseResolutionPreset("1280x720", width, height));
    CHECK(width == 1280);
    CHECK(height == 720);
    REQUIRE(video_mode_util::TryParseResolutionPreset("1920X1080", width, height));
    CHECK(width == 1920);
    CHECK(height == 1080);
    REQUIRE(video_mode_util::TryParseResolutionPreset("1920 x 1080", width, height));
    CHECK(width == 1920);
    CHECK(height == 1080);
  }

  SECTION("invalid values are rejected") {
    for (std::string_view input : {"", "abc", "0x100", "100x0", "100x", "x100", "10x20x30"}) {
      CHECK_FALSE(video_mode_util::TryParseResolutionPreset(input, width, height));
    }
  }

  SECTION("the raw parser never clamps to a display (that is TryResolveResolutionPreset's job)") {
    REQUIRE(video_mode_util::TryParseResolutionPreset("3840x2160", width, height));
    CHECK(width == 3840);
    CHECK(height == 2160);
  }
}

TEST_CASE("SetNativeDisplayMode round-trips, and TryResolveResolutionPreset resolves native "
          "aliases and clamps to a 1920x1080@60Hz display",
          "[graphics][resolution]") {
  SetNativeDisplayMode(1920, 1080, 60.0);

  int32_t width = 0;
  int32_t height = 0;
  double refresh_hz = 0.0;
  REQUIRE(GetNativeDisplayMode(width, height, refresh_hz));
  CHECK(width == 1920);
  CHECK(height == 1080);
  CHECK(refresh_hz == 60.0);

  SECTION("native/display/desktop resolve to the native display mode") {
    for (std::string_view alias : {"native", "display", "desktop"}) {
      REQUIRE(Resolve(alias, width, height));
      CHECK(width == 1920);
      CHECK(height == 1080);
    }
  }

  SECTION("presets and custom sizes larger than the display scale down to it exactly") {
    for (std::string_view value : {"4k", "2160p", "3840x2160", "2560x1440"}) {
      REQUIRE(Resolve(value, width, height));
      CHECK(width == 1920);
      CHECK(height == 1080);
    }
  }

  SECTION("a value equal to the display is unchanged") {
    REQUIRE(Resolve("1080p", width, height));
    CHECK(width == 1920);
    CHECK(height == 1080);
  }

  SECTION("custom sizes scale down preserving aspect, and are never upscaled") {
    // 2560x720 exceeds the 1920x1080 display in width only; the binding scale
    // factor is 1920/2560 = 0.75, so 720 * 0.75 = 540 exactly.
    REQUIRE(Resolve("2560x720", width, height));
    CHECK(width == 1920);
    CHECK(height == 540);

    // Smaller than the display: never upscaled.
    REQUIRE(Resolve("1280x720", width, height));
    CHECK(width == 1280);
    CHECK(height == 720);
  }

  SECTION("invalid values are still rejected") {
    CHECK_FALSE(Resolve("", width, height));
    CHECK_FALSE(Resolve("bogus", width, height));
  }

  SECTION("GetEffectiveFrameRateLimit clamps positive values to the 60Hz display refresh") {
    SetFrameRateLimit(120);
    CHECK(GetEffectiveFrameRateLimit() == 60);
    SetFrameRateLimit(61);
    CHECK(GetEffectiveFrameRateLimit() == 60);
    SetFrameRateLimit(60);
    CHECK(GetEffectiveFrameRateLimit() == 60);
    SetFrameRateLimit(30);
    CHECK(GetEffectiveFrameRateLimit() == 30);
    SetFrameRateLimit(1);
    CHECK(GetEffectiveFrameRateLimit() == 1);
    // -1 (unlimited) and 0 (default) pass through even with a native mode set.
    SetFrameRateLimit(-1);
    CHECK(GetEffectiveFrameRateLimit() == -1);
    SetFrameRateLimit(0);
    CHECK(GetEffectiveFrameRateLimit() == 0);
  }

  SECTION("the typed storage and the by-name registry query agree") {
    SetFrameRateLimit(120);
    CHECK(rex::cvar::Query<int32_t>("frame_rate_limit") == 120);
    CHECK(REXCVAR_GET(frame_rate_limit) == 120);
    SetFrameRateLimit(0);
  }

  SECTION("the frame_rate_limit cvar accepts -1 through the registry (unlimited)") {
    // The feature widens the cvar range to include -1.
    CHECK(rex::cvar::SetFlagByName("frame_rate_limit", "-1"));
    CHECK(REXCVAR_GET(frame_rate_limit) == -1);
    CHECK(rex::cvar::GetFlagByName("frame_rate_limit") == "-1");
    SetFrameRateLimit(0);
  }
}

TEST_CASE("clamping to a 2560x1440@144Hz display preserves aspect, and the frame rate limit "
          "follows the new refresh rate",
          "[graphics][resolution]") {
  SetNativeDisplayMode(2560, 1440, 144.0);

  int32_t width = 0;
  int32_t height = 0;
  double refresh_hz = 0.0;
  REQUIRE(GetNativeDisplayMode(width, height, refresh_hz));
  CHECK(width == 2560);
  CHECK(height == 1440);
  CHECK(refresh_hz == 144.0);

  SECTION("the native alias tracks the newly set mode") {
    REQUIRE(Resolve("native", width, height));
    CHECK(width == 2560);
    CHECK(height == 1440);
  }

  SECTION("an ultrawide request is scaled down to fit, preserving aspect") {
    // 3440x1440 (aspect ~2.389) into 2560x1440: width binds, scale 2560/3440,
    // height lands at ~1071.6 -> 1071 or 1072 depending on rounding.
    REQUIRE(Resolve("3440x1440", width, height));
    CHECK(width <= 2560);
    CHECK(height <= 1440);
    CHECK(width == 2560);
    CHECK(height >= 1070);
    CHECK(height <= 1073);
    const double in_aspect = 3440.0 / 1440.0;
    const double out_aspect = double(width) / double(height);
    CHECK(out_aspect == Catch::Approx(in_aspect).epsilon(0.01));
  }

  SECTION("GetEffectiveFrameRateLimit clamps to 144Hz") {
    SetFrameRateLimit(240);
    CHECK(GetEffectiveFrameRateLimit() == 144);
    SetFrameRateLimit(144);
    CHECK(GetEffectiveFrameRateLimit() == 144);
    SetFrameRateLimit(100);
    CHECK(GetEffectiveFrameRateLimit() == 100);
    SetFrameRateLimit(-1);
    CHECK(GetEffectiveFrameRateLimit() == -1);
    SetFrameRateLimit(0);
    CHECK(GetEffectiveFrameRateLimit() == 0);
  }
}

TEST_CASE("TryGetResolutionPresetFromCVar resolves through TryResolveResolutionPreset",
          "[graphics][resolution]") {
  SetNativeDisplayMode(1920, 1080, 60.0);

  int32_t width = 0;
  int32_t height = 0;

  SECTION("the default (empty) cvar value reports false") {
    REXCVAR_SET(resolution, "");
    CHECK_FALSE(video_mode_util::TryGetResolutionPresetFromCVar(width, height));
  }

  SECTION("a native alias resolves via the cvar") {
    REQUIRE(rex::cvar::SetFlagByName("resolution", "native"));
    REQUIRE(video_mode_util::TryGetResolutionPresetFromCVar(width, height));
    CHECK(width == 1920);
    CHECK(height == 1080);
  }

  SECTION("cvar values larger than the display are clamped") {
    REQUIRE(rex::cvar::SetFlagByName("resolution", "4k"));
    REQUIRE(video_mode_util::TryGetResolutionPresetFromCVar(width, height));
    CHECK(width == 1920);
    CHECK(height == 1080);

    REQUIRE(rex::cvar::SetFlagByName("resolution", "2560x1440"));
    REQUIRE(video_mode_util::TryGetResolutionPresetFromCVar(width, height));
    CHECK(width == 1920);
    CHECK(height == 1080);
  }

  SECTION("cvar values at or below the display pass through") {
    REQUIRE(rex::cvar::SetFlagByName("resolution", "720p"));
    REQUIRE(video_mode_util::TryGetResolutionPresetFromCVar(width, height));
    CHECK(width == 1280);
    CHECK(height == 720);
  }

  SECTION("invalid cvar values report false") {
    REQUIRE(rex::cvar::SetFlagByName("resolution", "bogus"));
    CHECK_FALSE(video_mode_util::TryGetResolutionPresetFromCVar(width, height));
  }

  REXCVAR_SET(resolution, "");
}

}  // namespace
