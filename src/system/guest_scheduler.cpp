/**
 * @file        src/system/guest_scheduler.cpp
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/guest_scheduler.h>

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
// Sync operations the holder may do before it has to let others run (see Tick).
constexpr uint32_t kQuantum = 16;
// Backward branches a thread may take before it has to let others run (REX_LOOP_BACKEDGE).
constexpr uint32_t kLoopQuantum = 1024;
// Retry soon when the budget runs out inside an interrupt-disabled section.
constexpr uint32_t kLoopRetry = 64;
}  // namespace

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

bool GuestScheduler::Holds(XThread* thread) {
  std::lock_guard<std::mutex> lock(mutex_);
  Entry* entry = Find(thread);
  return entry && holder_ == entry;
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
    default:
      return false;
  }
}

std::string GuestScheduler::DescribeStates() {
  constexpr const char* kStateNames[] = {"idle", "runnable", "blocked", "running", "dead"};
  std::string states;
  for (auto& [order, other] : entries_) {
    if (other->state == State::kDead || other->state == State::kIdle) {
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
  if (holder_ || !from) {
    token_free_since_ = std::chrono::steady_clock::now();
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
      holder_since_ = std::chrono::steady_clock::now();
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
      // Nobody can run: everyone waits on the host, or on guest time that only moves at the next
      // frame (then the clock's stall fallback has to step in, which leaks host time).
      const auto now = std::chrono::steady_clock::now();
      if (!holder_ && now - token_free_since_ > kIdleReport && !idle_reported_) {
        idle_reported_ = true;
        REXSYS_WARN("sched_deterministic: nobody could run for {} ms;{}", kIdleReport.count(),
                    DescribeStates());
      }
    }
    entry->cv.wait_for(lock, kIdlePoll);
    if (holder_ && !holder_reported_ &&
        std::chrono::steady_clock::now() - holder_since_ > kIdleReport) {
      holder_reported_ = true;
      REXSYS_WARN("sched_deterministic: thread {} has held the token for {} ms;{}",
                  holder_->thread->thread_id(), kIdleReport.count(), DescribeStates());
    }
    if (!reported && std::chrono::steady_clock::now() - wait_start > kStuckReport) {
      reported = true;
      REXSYS_WARN(
          "sched_deterministic: thread {} waited {} s for the token; holder is thread {};{}",
          entry->thread->thread_id(), kStuckReport.count(),
          holder_ ? int64_t(holder_->thread->thread_id()) : -1, DescribeStates());
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
  if (!entry || entry->enter_depth++ > 0 || holder_ == entry) {
    return;
  }
  entry->state = State::kRunnable;
  WaitForToken(entry, lock);
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
  std::unique_lock<std::mutex> lock(mutex_);
  Entry* entry = Find(thread);
  if (!entry || holder_ != entry) {
    return;
  }
  entry->state = State::kRunnable;
  PassOn(entry);
  WaitForToken(entry, lock);
}

void GuestScheduler::Tick(XThread* thread) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = Find(thread);
    if (!entry || holder_ != entry || ++entry->ticks < kQuantum) {
      return;
    }
  }
  Yield(thread);
}

uint32_t GuestScheduler::OnLoopBudgetSpent(bool in_global_lock) {
  GuestScheduler* scheduler = Get();
  if (!scheduler) {
    return UINT32_MAX;
  }
  if (in_global_lock) {
    return kLoopRetry;
  }
  if (XThread::IsInThread()) {
    XThread* thread = XThread::GetCurrentThread();
    if (scheduler->Holds(thread)) {
      scheduler->Yield(thread);
    }
  }
  return kLoopQuantum;
}

void GuestScheduler::TickCurrentThread() {
  GuestScheduler* scheduler = Get();
  if (scheduler && XThread::IsInThread()) {
    scheduler->Tick(XThread::GetCurrentThread());
  }
}

}  // namespace rex::system
