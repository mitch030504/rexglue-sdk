/**
 * @file        src/system/guest_scheduler.cpp
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/guest_scheduler.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>

REXCVAR_DEFINE_BOOL(sched_deterministic, false, "Kernel",
                    "Netplay and replays: run one guest thread at a time and hand over in a fixed "
                    "order where the guest blocks, so thread interleaving (lock and heap "
                    "allocation order) repeats between runs. Needs clock_frame_locked.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::system {

namespace {
// While nobody can run (everyone waits on the host: GPU, I/O), the waiters re-check this often.
constexpr std::chrono::milliseconds kIdlePoll{1};
// Waiting this long for the token means the holder is stuck (a guest spin loop with no kernel
// call, or a wait on host work while holding the token); reported once per wait.
constexpr std::chrono::seconds kStuckReport{5};
// The token being free this long (everyone blocked) is reported once per idle period.
constexpr std::chrono::milliseconds kIdleReport{200};
// Sync operations the holder may do before it has to let others run (see Tick). Adapts like
// kLoopQuantum, up to kMaxQuantum.
constexpr uint32_t kQuantum = 16;
constexpr uint32_t kMaxQuantum = 1u << 14;
// Backward branches a thread may take before it has to let others run (REX_LOOP_BACKEDGE).
// Doubles, up to kMaxLoopQuantum, each time nobody else could run at the yield (a long
// computation alone), and drops back as soon as someone else gets a turn (a busy-wait on another
// thread). Both depend only on scheduling decisions, so they repeat between runs.
constexpr uint32_t kLoopQuantum = 1024;
constexpr uint32_t kMaxLoopQuantum = 1u << 20;
// Retry soon when the budget runs out inside an interrupt-disabled section.
constexpr uint32_t kLoopRetry = 64;
// Virtual frames in a row while nobody can run before giving up and waiting on the host (10 s of
// guest time: something waits on host work that has no fixed turn).
constexpr uint32_t kMaxIdleFramesInARow = 600;
// Backward branches without a guest frame that count as one frame of guest work (a virtual
// frame), for guest code that spins on the clock without ever blocking.
constexpr uint64_t kLoopBackedgesPerVirtualFrame = uint64_t(1) << 22;
// NoSwitchScope nesting on this thread.
thread_local int no_switch_depth = 0;
// The current loop quantum of this thread (see kLoopQuantum).
thread_local uint32_t loop_quantum = kLoopQuantum;
}  // namespace

GuestScheduler::NoSwitchScope::NoSwitchScope() {
  ++no_switch_depth;
}

GuestScheduler::NoSwitchScope::~NoSwitchScope() {
  --no_switch_depth;
}

GuestScheduler* GuestScheduler::Get() {
  static GuestScheduler* scheduler = [] {
    if (!REXCVAR_GET(sched_deterministic)) {
      return static_cast<GuestScheduler*>(nullptr);
    }
    if (!chrono::Clock::GuestTimedWaits()) {
      REXSYS_WARN("sched_deterministic needs clock_frame_locked (and clock_frame_locked_waits)");
      return static_cast<GuestScheduler*>(nullptr);
    }
    REXSYS_INFO("sched_deterministic: one guest thread at a time");
    chrono::Clock::SetFrameLockedStallFallback(false);
    return new GuestScheduler();
  }();
  return scheduler;
}

GuestScheduler* GuestScheduler::ForCurrentThread() {
  GuestScheduler* scheduler = Get();
  if (!scheduler || !XThread::IsInThread()) {
    return nullptr;
  }
  return scheduler->Holds(XThread::GetCurrentThread()) ? scheduler : nullptr;
}

GuestScheduler::Entry* GuestScheduler::Find(XThread* thread) {
  auto it = by_thread_.find(thread);
  return it == by_thread_.end() ? nullptr : it->second;
}

GuestScheduler::Entry* GuestScheduler::CachedEntry(XThread* thread) {
  // Entries are never freed (dead ones stay in entries_), so the pointer can be kept.
  thread_local XThread* cached_thread = nullptr;
  thread_local Entry* cached_entry = nullptr;
  if (cached_thread != thread) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = Find(thread);
    if (!entry) {
      return nullptr;
    }
    cached_thread = thread;
    cached_entry = entry;
  }
  return cached_entry;
}

bool GuestScheduler::Holds(XThread* thread) {
  // Without the mutex: only the holder itself can make this true or (by passing the token on)
  // false.
  Entry* entry = CachedEntry(thread);
  return entry && holder_.load(std::memory_order_acquire) == entry;
}

void GuestScheduler::OnCreate(XThread* thread, bool guest) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto entry = std::make_unique<Entry>();
  entry->thread = thread;
  entry->order = guest ? next_guest_order_++ : next_host_order_++;
  // A guest thread may run as soon as its host thread starts (and it isn't suspended).
  entry->state = guest ? State::kRunnable : State::kIdle;
  by_thread_[thread] = entry.get();
  entries_[entry->order] = std::move(entry);
}

bool GuestScheduler::CanRun(Entry* entry) {
  switch (entry->state) {
    case State::kRunnable:
      return entry->thread->suspend_count() == 0;
    case State::kBlocked:
      return entry->thread->suspend_count() == 0 && entry->ready && (*entry->ready)();
    case State::kIdle:
      return entry->due && entry->due();
    default:
      return false;
  }
}

std::string GuestScheduler::DescribeStates() {
  constexpr const char* kStateNames[] = {"idle", "runnable", "blocked", "running", "dead"};
  std::string states;
  for (auto& [order, other] : entries_) {
    if (other->state == State::kDead || (other->state == State::kIdle && !other->due)) {
      continue;
    }
    // lr: the guest return address of the kernel call the thread is in.
    states += fmt::format(
        " {}:{}{}@{:08X}", other->thread->thread_id(), kStateNames[size_t(other->state)],
        other->state == State::kBlocked && other->ready && (*other->ready)() ? "(ready)" : "",
        uint32_t(other->thread->thread_state()->context()->lr));
  }
  return states;
}

void GuestScheduler::PassOn(Entry* from) {
  const auto now = std::chrono::steady_clock::now();
  const bool was_free = !holder_;
  if (holder_) {
    holder_.load()->held += now - holder_since_;
  }
  if (holder_ || !from) {
    token_free_since_ = now;
    idle_reported_ = false;
  }
  holder_ = nullptr;
  last_holder_ = from;
  if (entries_.empty()) {
    return;
  }
  // Start after `from` and wrap around, so every thread gets its turn; `from` comes last.
  auto start = from ? entries_.upper_bound(from->order) : entries_.begin();
  for (size_t i = 0; i < entries_.size(); ++i, ++start) {
    if (start == entries_.end()) {
      start = entries_.begin();
    }
    Entry* candidate = start->second.get();
    if (CanRun(candidate)) {
      holder_ = candidate;
      idle_frames_in_a_row_ = 0;
      ++handoffs_;
      ++candidate->turns;
      if (was_free) {
        free_time_ += now - token_free_since_;
      }
      holder_since_ = now;
      holder_reported_ = false;
      candidate->cv.notify_one();
      return;
    }
  }
}

void GuestScheduler::WaitForToken(Entry* entry, std::unique_lock<std::mutex>& lock) {
  const auto wait_start = std::chrono::steady_clock::now();
  bool reported = false;
  while (holder_ != entry) {
    if (!holder_) {
      PassOn(last_holder_);
      if (holder_ == entry) {
        break;
      }
      // Nobody can run, and every host thread with work to do has a fixed turn (SetDue), so only
      // guest time can wake anyone: a timeout, or vblank. Advance it now, at a point fixed by
      // guest state, instead of waiting for host time to pass. Boot included: the clock stands
      // still before the first frame too (Clock::SetFrameLockedStallFallback).
      if (!holder_ && idle_frames_in_a_row_ < kMaxIdleFramesInARow &&
          chrono::Clock::AdvanceVirtualFrame()) {
        loop_backedges_since_frame_ = 0;
        ++idle_frames_in_a_row_;
        if (++idle_frames_total_ % 600 == 0) {
          REXSYS_INFO("sched_deterministic: {} idle virtual frames so far",
                      idle_frames_total_);
        }
        continue;
      }
      // Nobody can run even after 10 s of guest time: someone waits on host work without a fixed
      // turn (I/O, a notification); from here on host time decides.
      const auto now = std::chrono::steady_clock::now();
      if (!holder_ && now - token_free_since_ > kIdleReport && !idle_reported_) {
        idle_reported_ = true;
        REXSYS_WARN("sched_deterministic: nobody could run for {} virtual frames and {} ms;{}",
                    idle_frames_in_a_row_, kIdleReport.count(), DescribeStates());
      }
    }
    entry->cv.wait_for(lock, kIdlePoll);
    if (holder_ && !holder_reported_ &&
        std::chrono::steady_clock::now() - holder_since_ > kIdleReport) {
      holder_reported_ = true;
      REXSYS_WARN("sched_deterministic: thread {} has held the token for {} ms;{}",
                  holder_.load()->thread->thread_id(), kIdleReport.count(), DescribeStates());
    }
    if (!reported && std::chrono::steady_clock::now() - wait_start > kStuckReport) {
      reported = true;
      REXSYS_WARN(
          "sched_deterministic: thread {} waited {} s for the token; holder is thread {};{}",
          entry->thread->thread_id(), kStuckReport.count(),
          holder_ ? int64_t(holder_.load()->thread->thread_id()) : -1, DescribeStates());
    }
  }
  entry->state = State::kRunning;
  entry->ticks = 0;
}

void GuestScheduler::Begin(XThread* thread) {
  std::unique_lock<std::mutex> lock(mutex_);
  Entry* entry = Find(thread);
  if (!entry) {
    return;
  }
  entry->state = State::kRunnable;
  WaitForToken(entry, lock);
}

void GuestScheduler::End(XThread* thread) {
  std::lock_guard<std::mutex> lock(mutex_);
  Entry* entry = Find(thread);
  if (!entry) {
    return;
  }
  entry->state = State::kDead;
  by_thread_.erase(thread);
  // The entry stays in entries_ (dead) so the round-robin position after it still works.
  if (holder_ == entry) {
    PassOn(entry);
  }
}

void GuestScheduler::Enter(XThread* thread) {
  std::unique_lock<std::mutex> lock(mutex_);
  Entry* entry = Find(thread);
  if (!entry || entry->enter_depth++ > 0) {
    return;
  }
  if (holder_ == entry) {
    // Picked (by its due condition) while on the way here.
    entry->state = State::kRunning;
    entry->ticks = 0;
    return;
  }
  // With a due condition the thread stays idle until that holds; CanRun checks it.
  entry->state = entry->due ? State::kIdle : State::kRunnable;
  WaitForToken(entry, lock);
}

void GuestScheduler::SetDue(XThread* thread, std::function<bool()> due) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (Entry* entry = Find(thread)) {
    entry->due = std::move(due);
  }
}

void GuestScheduler::OnGuestFrame() {
  std::lock_guard<std::mutex> lock(mutex_);
  loop_backedges_since_frame_ = 0;
  constexpr uint64_t kReportFrames = 600;
  const auto now = std::chrono::steady_clock::now();
  if (frames_++ % kReportFrames != 0 || !report_since_.time_since_epoch().count()) {
    if (!report_since_.time_since_epoch().count()) {
      report_since_ = now;
    }
    return;
  }
  // Where host time went over the last kReportFrames frames: who held the token, and how long
  // nobody did (everyone waiting on a host thread that hasn't arrived yet).
  const auto ms = [](std::chrono::steady_clock::duration d) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
  };
  std::string held;
  for (auto& [order, entry] : entries_) {
    auto time = entry->held;
    if (entry.get() == holder_) {
      time += now - holder_since_;
    }
    if (ms(time) >= 50 || entry->turns >= 10000) {
      const bool host = entry->order >= next_host_order_base_;
      held += fmt::format(" {}{}:{}ms/{}", entry->thread->thread_id(),
                          host ? "(" + entry->thread->name() + ")" : "", ms(time), entry->turns);
    }
    entry->held = {};
    entry->turns = 0;
  }
  if (holder_) {
    holder_since_ = now;
  }
  REXSYS_INFO("sched_deterministic: {} frames in {} ms, {} handoffs, token free {} ms; held{}",
              kReportFrames, ms(now - report_since_), handoffs_, ms(free_time_), held);
  report_since_ = now;
  handoffs_ = 0;
  free_time_ = {};
}

void GuestScheduler::CountLoopQuantum(uint32_t backedges) {
  std::lock_guard<std::mutex> lock(mutex_);
  loop_backedges_since_frame_ += backedges;
  if (loop_backedges_since_frame_ < kLoopBackedgesPerVirtualFrame) {
    return;
  }
  // A frame's worth of guest work without VdSwap (a load that polls the clock): time moves on by
  // the work done, the same in every run.
  loop_backedges_since_frame_ = 0;
  chrono::Clock::AdvanceVirtualFrame();
  if (++busy_frames_total_ % 60 == 1) {
    REXSYS_INFO("sched_deterministic: {} busy virtual frames so far", busy_frames_total_);
  }
}

void GuestScheduler::Leave(XThread* thread) {
  std::lock_guard<std::mutex> lock(mutex_);
  Entry* entry = Find(thread);
  if (!entry || --entry->enter_depth > 0) {
    return;
  }
  entry->state = State::kIdle;
  if (holder_ == entry) {
    PassOn(entry);
  }
}

void GuestScheduler::Block(XThread* thread, const std::function<bool()>& ready,
                           const std::function<bool()>& attempt) {
  std::unique_lock<std::mutex> lock(mutex_);
  Entry* entry = Find(thread);
  if (!entry || holder_ != entry) {
    // Not scheduled: plain polling (shouldn't happen; ForCurrentThread gates callers).
    lock.unlock();
    while (!attempt()) {
      std::this_thread::sleep_for(kIdlePoll);
    }
    return;
  }
  while (true) {
    entry->state = State::kBlocked;
    entry->ready = &ready;
    PassOn(entry);
    WaitForToken(entry, lock);
    entry->ready = nullptr;
    // Holding the token: nothing else runs guest code, so the attempt sees settled state. The
    // scheduler lock is dropped because attempts take the wait objects' own locks.
    lock.unlock();
    const bool done = attempt();
    lock.lock();
    if (done) {
      return;
    }
  }
}

void GuestScheduler::Yield(XThread* thread) {
  YieldTurn(thread);
}

bool GuestScheduler::YieldTurn(XThread* thread) {
  if (no_switch_depth) {
    return false;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  Entry* entry = Find(thread);
  if (!entry || holder_ != entry) {
    return false;
  }
  entry->state = State::kRunnable;
  PassOn(entry);
  // PassOn picks the yielding thread itself only when nobody else can run.
  const bool others_ran = holder_ != entry;
  WaitForToken(entry, lock);
  return others_ran;
}

void GuestScheduler::Tick(XThread* thread) {
  // Lock-free like Holds: ticks and quantum belong to the holder (WaitForToken resets ticks on
  // the entry's own thread).
  Entry* entry = CachedEntry(thread);
  if (!entry || holder_.load(std::memory_order_acquire) != entry ||
      ++entry->ticks < entry->quantum) {
    return;
  }
  entry->quantum = YieldTurn(thread) ? kQuantum : std::min(entry->quantum * 2, kMaxQuantum);
}

uint32_t GuestScheduler::OnLoopBudgetSpent(bool in_global_lock) {
  GuestScheduler* scheduler = Get();
  if (!scheduler) {
    return UINT32_MAX;
  }
  if (in_global_lock || no_switch_depth) {
    return kLoopRetry;
  }
  if (XThread::IsInThread()) {
    XThread* thread = XThread::GetCurrentThread();
    if (scheduler->Holds(thread)) {
      scheduler->CountLoopQuantum(loop_quantum);
      loop_quantum = scheduler->YieldTurn(thread) ? kLoopQuantum
                                                  : std::min(loop_quantum * 2, kMaxLoopQuantum);
    }
  }
  return loop_quantum;
}

void GuestScheduler::TickCurrentThread() {
  GuestScheduler* scheduler = Get();
  if (scheduler && XThread::IsInThread()) {
    scheduler->Tick(XThread::GetCurrentThread());
  }
}

}  // namespace rex::system
