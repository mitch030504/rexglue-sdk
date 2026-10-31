/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <limits>
#include <functional>
#include <mutex>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/math.h>

REXCVAR_DEFINE_BOOL(clock_no_scaling, false, "Clock",
                    "Disable clock scaling (inverted: false = scaling enabled)");

REXCVAR_DEFINE_BOOL(clock_source_raw, false, "Clock", "Use raw clock source without scaling");

REXCVAR_DEFINE_BOOL(clock_frame_locked, false, "Clock",
                    "Deterministic guest time for netplay and replays: guest time advances by "
                    "exactly 1/clock_frame_locked_hz per guest frame (VdSwap) and stands still "
                    "in between, and the guest date starts at a fixed 2013-01-01. Before the "
                    "first frame and during stalls without frames (loading), it follows the host "
                    "clock instead, so timeouts still fire.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(clock_frame_locked_waits, true, "Clock",
                    "With clock_frame_locked, guest sleeps and wait timeouts end by guest time "
                    "too, so a 1 ms sleep lasts until the next guest frame instead of 1 ms of "
                    "host time. Off only to compare against the old behaviour.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(clock_frame_locked_stall_ms, 250, "Clock",
                     "With clock_frame_locked, how long without a guest frame counts as a stall "
                     "(loading), after which guest time advances in virtual frames. Raise it for "
                     "replay comparisons whose tooling pauses the game between frames.")
    .range(50, 60000)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(clock_frame_locked_hz, 60, "Clock",
                     "Guest frames per second of guest time under clock_frame_locked.")
    .range(1, 240)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::chrono {

// Time scalar applied to all time operations.
double guest_time_scalar_ = 1.0;
// Tick frequency of guest.
uint64_t guest_tick_frequency_ = Clock::host_tick_frequency_platform();
// Base FILETIME of the guest system from app start.
uint64_t guest_system_time_base_ = Clock::QueryHostSystemTime();
// Combined time and frequency ratio between host and guest.
// Split in numerator (first) and denominator (second).
// Computed by RecomputeGuestTickScalar.
std::pair<uint64_t, uint64_t> guest_tick_ratio_ = std::make_pair(1, 1);

// Native guest ticks.
uint64_t last_guest_tick_count_ = 0;
// Last sampled host tick count.
uint64_t last_host_tick_count_ = Clock::QueryHostTickCount();
// Mutex to ensure last_host_tick_count_ and last_guest_tick_count_ are in sync
std::mutex tick_mutex_;

// clock_frame_locked state, guarded by tick_mutex_.
// Host tick count of the last guest frame, 0 before the first one.
uint64_t frame_locked_last_frame_host_tick_ = 0;
// Guest time at the last guest frame.
uint64_t frame_locked_frame_guest_tick_ = 0;
// Virtual frames added during the current stall, reported at the next frame.
uint64_t frame_locked_stall_frames_ = 0;
// Off under sched_deterministic, which adds virtual frames itself at points fixed by guest work
// (Clock::AdvanceVirtualFrame), so host time never reaches guest time.
bool frame_locked_stall_fallback_ = true;
// Guest time at the first frame. Boot takes a varying amount of host time, so the first frame
// starts at this fixed point instead (unless boot ran longer, which breaks determinism and warns).
constexpr uint64_t kFrameLockedFirstFrameSeconds = 10;
// No frame for clock_frame_locked_stall_ms counts as a stall (loading). The game polls guest time
// during some loads (waiting out ~1 s at boot), so a stall can't freeze time; it then advances in
// whole virtual frames, one per frame period of host time. A deadline-bound wait ends on the same
// virtual frame every run; a load bound by host I/O doesn't. Hitches shorter than this leak
// nothing.
// 2013-01-01 00:00 UTC as FILETIME: a fixed guest date, so date-derived state matches across runs.
constexpr uint64_t kFrameLockedSystemTimeBase = 130014720000000000ull;

bool FrameLocked() {
  return REXCVAR_GET(clock_frame_locked) && !REXCVAR_GET(clock_no_scaling);
}

void RecomputeGuestTickScalar() {
  // Create a rational number with numerator (first) and denominator (second)
  auto frac = std::make_pair(guest_tick_frequency_, Clock::QueryHostTickFrequency());
  // Doing it this way ensures we don't mess up our frequency scaling and
  // precisely controls the precision the guest_time_scalar_ can have.
  if (guest_time_scalar_ > 1.0) {
    frac.first *= static_cast<uint64_t>(guest_time_scalar_ * 10.0);
    frac.second *= 10;
  } else {
    frac.first *= 10;
    frac.second *= static_cast<uint64_t>(10.0 / guest_time_scalar_);
  }
  // Keep this a rational calculation and reduce the fraction
  reduce_fraction(frac);

  std::lock_guard<std::mutex> lock(tick_mutex_);
  guest_tick_ratio_ = frac;
}

// Update the guest timer for all threads.
// Return a copy of the value so locking is reduced.
uint64_t UpdateGuestClock() {
  uint64_t host_tick_count = Clock::QueryHostTickCount();

  if (REXCVAR_GET(clock_no_scaling)) {
    // Nothing to update, calculate on the fly
    return host_tick_count * guest_tick_ratio_.first / guest_tick_ratio_.second;
  }

  if (FrameLocked()) {
    // Guest frames advance the time (Clock::OnGuestFrame). Before the first frame it follows the
    // host clock, and during stalls it advances in virtual frames.
    std::lock_guard<std::mutex> lock(tick_mutex_);
    uint64_t host_tick_delta =
        host_tick_count > last_host_tick_count_ ? host_tick_count - last_host_tick_count_ : 0;
    last_host_tick_count_ = host_tick_count;
    if (!frame_locked_last_frame_host_tick_) {
      // Without the stall fallback (sched_deterministic) boot is guest-timed too: only virtual
      // frames move time before the first frame.
      if (frame_locked_stall_fallback_) {
        last_guest_tick_count_ +=
            host_tick_delta * guest_tick_ratio_.first / guest_tick_ratio_.second;
      }
      return last_guest_tick_count_;
    }
    const uint64_t hz = uint64_t(REXCVAR_GET(clock_frame_locked_hz));
    const uint64_t host_frequency = Clock::QueryHostTickFrequency();
    const uint64_t stall_host_ticks =
        host_frequency * uint64_t(REXCVAR_GET(clock_frame_locked_stall_ms)) / 1000;
    // host_tick_count was sampled before the lock, so a frame may have arrived after it.
    const uint64_t since_frame = host_tick_count > frame_locked_last_frame_host_tick_
                                     ? host_tick_count - frame_locked_last_frame_host_tick_
                                     : 0;
    if (frame_locked_stall_fallback_ && since_frame > stall_host_ticks) {
      const uint64_t virtual_frames = 1 + (since_frame - stall_host_ticks) * hz / host_frequency;
      const uint64_t target =
          frame_locked_frame_guest_tick_ + virtual_frames * (guest_tick_frequency_ / hz);
      if (target > last_guest_tick_count_) {
        last_guest_tick_count_ = target;
        frame_locked_stall_frames_ = virtual_frames;
      }
    }
    return last_guest_tick_count_;
  }

  std::unique_lock<std::mutex> lock(tick_mutex_, std::defer_lock);
  if (lock.try_lock()) {
    // Translate host tick count to guest tick count.
    uint64_t host_tick_delta =
        host_tick_count > last_host_tick_count_ ? host_tick_count - last_host_tick_count_ : 0;
    last_host_tick_count_ = host_tick_count;
    uint64_t guest_tick_delta =
        host_tick_delta * guest_tick_ratio_.first / guest_tick_ratio_.second;
    last_guest_tick_count_ += guest_tick_delta;
    return last_guest_tick_count_;
  } else {
    // Wait until another thread has finished updating the clock.
    lock.lock();
    return last_guest_tick_count_;
  }
}

// Offset of the current guest system file time relative to the guest base time.
inline uint64_t QueryGuestSystemTimeOffset() {
  if (REXCVAR_GET(clock_no_scaling)) {
    return Clock::QueryHostSystemTime() - guest_system_time_base_;
  }

  auto guest_tick_count = UpdateGuestClock();

  uint64_t numerator = 10000000;  // 100ns/10MHz resolution
  uint64_t denominator = guest_tick_frequency_;
  reduce_fraction(numerator, denominator);

  return guest_tick_count * numerator / denominator;
}

uint64_t Clock::QueryHostTickFrequency() {
#if REX_CLOCK_RAW_AVAILABLE
  if (REXCVAR_GET(clock_source_raw)) {
    return host_tick_frequency_raw();
  }
#endif
  return host_tick_frequency_platform();
}
uint64_t Clock::QueryHostTickCount() {
#if REX_CLOCK_RAW_AVAILABLE
  if (REXCVAR_GET(clock_source_raw)) {
    return host_tick_count_raw();
  }
#endif
  return host_tick_count_platform();
}

double Clock::guest_time_scalar() {
  return guest_time_scalar_;
}

void Clock::set_guest_time_scalar(double scalar) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return;
  }

  guest_time_scalar_ = scalar;
  RecomputeGuestTickScalar();
}

std::pair<uint64_t, uint64_t> Clock::guest_tick_ratio() {
  std::lock_guard<std::mutex> lock(tick_mutex_);
  return guest_tick_ratio_;
}

uint64_t Clock::guest_tick_frequency() {
  return guest_tick_frequency_;
}

void Clock::set_guest_tick_frequency(uint64_t frequency) {
  guest_tick_frequency_ = frequency;
  RecomputeGuestTickScalar();
}

uint64_t Clock::guest_system_time_base() {
  return guest_system_time_base_;
}

void Clock::set_guest_system_time_base(uint64_t time_base) {
  guest_system_time_base_ = time_base;
}

uint64_t Clock::QueryGuestTickCount() {
  auto guest_tick_count = UpdateGuestClock();
  return guest_tick_count;
}

uint64_t Clock::QueryGuestSystemTime() {
  if (REXCVAR_GET(clock_no_scaling)) {
    return Clock::QueryHostSystemTime();
  }

  auto guest_system_time_offset = QueryGuestSystemTimeOffset();
  return (FrameLocked() ? kFrameLockedSystemTimeBase : guest_system_time_base_) +
         guest_system_time_offset;
}

static void OnGuestFrameImpl() {
  if (!FrameLocked()) {
    return;
  }
  const uint64_t host_tick_count = Clock::QueryHostTickCount();
  std::lock_guard<std::mutex> lock(tick_mutex_);
  if (!frame_locked_last_frame_host_tick_) {
    const uint64_t first_frame_ticks = guest_tick_frequency_ * kFrameLockedFirstFrameSeconds;
    if (last_guest_tick_count_ > first_frame_ticks && frame_locked_stall_fallback_) {
      REXLOG_WARN("clock_frame_locked: boot took over {} s of guest time; runs won't match",
                  kFrameLockedFirstFrameSeconds);
    } else {
      last_guest_tick_count_ = first_frame_ticks;
    }
  } else {
    if (frame_locked_stall_frames_) {
      REXLOG_INFO(
          "clock_frame_locked: {} ms without a guest frame, {} virtual frames",
          (host_tick_count - frame_locked_last_frame_host_tick_) * 1000 / Clock::QueryHostTickFrequency(),
          frame_locked_stall_frames_);
      frame_locked_stall_frames_ = 0;
    }
    last_guest_tick_count_ += guest_tick_frequency_ / uint64_t(REXCVAR_GET(clock_frame_locked_hz));
  }
  frame_locked_last_frame_host_tick_ = std::max<uint64_t>(host_tick_count, 1);
  frame_locked_frame_guest_tick_ = last_guest_tick_count_;
  last_host_tick_count_ = host_tick_count;
}

static bool AdvanceVirtualFrameImpl() {
  if (!FrameLocked()) {
    return false;
  }
  const uint64_t host_tick_count = Clock::QueryHostTickCount();
  std::lock_guard<std::mutex> lock(tick_mutex_);
  if (!frame_locked_last_frame_host_tick_) {
    if (frame_locked_stall_fallback_) {
      return false;  // Before the first frame guest time follows the host clock anyway.
    }
    // Boot under the deterministic scheduler: OnGuestFrame still sees the first frame as such.
    last_guest_tick_count_ +=
        guest_tick_frequency_ / uint64_t(REXCVAR_GET(clock_frame_locked_hz));
    last_host_tick_count_ = host_tick_count;
    return true;
  }
  last_guest_tick_count_ += guest_tick_frequency_ / uint64_t(REXCVAR_GET(clock_frame_locked_hz));
  frame_locked_frame_guest_tick_ = last_guest_tick_count_;
  // Restart the stall timer so the host fallback (if on) doesn't add the same frame again.
  frame_locked_last_frame_host_tick_ = std::max<uint64_t>(host_tick_count, 1);
  last_host_tick_count_ = host_tick_count;
  return true;
}

// Called after guest time moved under clock_frame_locked (see SetFrameLockedTimeCallback).
std::function<void()> frame_locked_time_callback_;

void Clock::OnGuestFrame() {
  OnGuestFrameImpl();
  if (FrameLocked() && frame_locked_time_callback_) {
    frame_locked_time_callback_();
  }
}

bool Clock::AdvanceVirtualFrame() {
  if (!AdvanceVirtualFrameImpl()) {
    return false;
  }
  if (frame_locked_time_callback_) {
    frame_locked_time_callback_();
  }
  return true;
}

void Clock::SetFrameLockedTimeCallback(std::function<void()> callback) {
  frame_locked_time_callback_ = std::move(callback);
}

void Clock::SetFrameLockedStallFallback(bool enabled) {
  std::lock_guard<std::mutex> lock(tick_mutex_);
  frame_locked_stall_fallback_ = enabled;
  if (!enabled && FrameLocked() && !frame_locked_last_frame_host_tick_) {
    // Host start-up so far took a varying amount of host time; guest time starts from zero.
    last_guest_tick_count_ = 0;
  }
}

bool Clock::GuestTimedWaits() {
  return FrameLocked() && REXCVAR_GET(clock_frame_locked_waits);
}

uint64_t Clock::GuestDeadline(int64_t timeout_ticks) {
  // FILETIME and the relative form share the 100 ns unit.
  return timeout_ticks > 0 ? uint64_t(timeout_ticks)
                           : QueryGuestSystemTime() + uint64_t(-timeout_ticks);
}

uint32_t Clock::QueryGuestUptimeMillis() {
  return static_cast<uint32_t>(std::min<uint64_t>(QueryGuestSystemTimeOffset() / 10000,
                                                  std::numeric_limits<uint32_t>::max()));
}

void Clock::SetGuestSystemTime(uint64_t system_time) {
  if (REXCVAR_GET(clock_no_scaling)) {
    // Time is fixed to host time.
    return;
  }

  // Query the filetime offset to calculate a new base time.
  auto guest_system_time_offset = QueryGuestSystemTimeOffset();
  guest_system_time_base_ = system_time - guest_system_time_offset;
}

uint32_t Clock::ScaleGuestDurationMillis(uint32_t guest_ms) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return guest_ms;
  }

  constexpr uint64_t max = std::numeric_limits<uint32_t>::max();

  if (guest_ms >= max) {
    return max;
  } else if (!guest_ms) {
    return 0;
  }
  uint64_t scaled_ms =
      static_cast<uint64_t>((static_cast<uint64_t>(guest_ms) * guest_time_scalar_));
  return static_cast<uint32_t>(std::min(scaled_ms, max));
}

int64_t Clock::ScaleGuestDurationFileTime(int64_t guest_file_time) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return static_cast<uint64_t>(guest_file_time);
  }

  if (!guest_file_time) {
    return 0;
  } else if (guest_file_time > 0) {
    // Absolute time.
    uint64_t guest_time = Clock::QueryGuestSystemTime();
    int64_t relative_time = guest_file_time - static_cast<int64_t>(guest_time);
    int64_t scaled_time = static_cast<int64_t>(relative_time * guest_time_scalar_);
    return static_cast<int64_t>(guest_time) + scaled_time;
  } else {
    // Relative time.
    uint64_t scaled_file_time =
        static_cast<uint64_t>((static_cast<uint64_t>(guest_file_time) * guest_time_scalar_));
    // TODO(benvanik): check for overflow?
    return scaled_file_time;
  }
}

void Clock::ScaleGuestDurationTimeval(int32_t* tv_sec, int32_t* tv_usec) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return;
  }

  uint64_t scaled_sec = static_cast<uint64_t>(static_cast<uint64_t>(*tv_sec) * guest_time_scalar_);
  uint64_t scaled_usec =
      static_cast<uint64_t>(static_cast<uint64_t>(*tv_usec) * guest_time_scalar_);
  if (scaled_usec > std::numeric_limits<uint32_t>::max()) {
    uint64_t overflow_sec = scaled_usec / 1000000;
    scaled_usec -= overflow_sec * 1000000;
    scaled_sec += overflow_sec;
  }
  *tv_sec = int32_t(scaled_sec);
  *tv_usec = int32_t(scaled_usec);
}

}  // namespace rex::chrono
