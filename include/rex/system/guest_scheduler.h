#pragma once
/**
 * @file        rex/system/guest_scheduler.h
 * @brief       Deterministic guest scheduling: one guest thread runs at a time, in a fixed order.
 *
 * With sched_deterministic (and clock_frame_locked), guest code only runs on the thread holding a
 * single run token. The holder gives it up only where the guest blocks (a wait that isn't
 * satisfied yet, a sleep, a yield, thread exit), and the token goes to the first thread after it,
 * in creation order, that can continue: one that is runnable, whose wait objects are signalled,
 * whose guest-time deadline has passed or that has an APC to run. Which thread runs next then
 * depends only on guest state, so lock and heap allocation order repeat between runs.
 *
 * Host threads that call into guest code (the audio callback, GPU interrupts) take the token
 * around the call with Enter/Leave. Host events still arrive on host time; that is the next step.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace rex::system {

class XThread;

class GuestScheduler {
 public:
  /// The scheduler when sched_deterministic is on, otherwise null.
  static GuestScheduler* Get();
  /// The scheduler if the calling thread runs guest code under it (holds the token).
  static GuestScheduler* ForCurrentThread();

  /// Registers a thread as it is created. Guest threads are created by guest code holding the
  /// token, so their order is deterministic; host threads go after all guest threads.
  void OnCreate(XThread* thread, bool guest);
  /// A guest thread's host thread starts: wait for the token before the first guest instruction.
  void Begin(XThread* thread);
  /// The thread is exiting: give up the token for good.
  void End(XThread* thread);

  /// A host thread is about to call guest code: take the token (nests).
  void Enter(XThread* thread);
  void Leave(XThread* thread);

  /// Gives up the token until `ready` (evaluated by whichever thread picks the next holder) is
  /// true and then `attempt` (run on this thread, holding the token) reports success.
  void Block(XThread* thread, const std::function<bool()>& ready,
             const std::function<bool()>& attempt);
  /// Lets every other runnable thread go first.
  void Yield(XThread* thread);
  /// Counts a sync operation (a wait that succeeded, a release) by the token holder and yields
  /// every kQuantum of them. Guest code that polls a flag under a lock never blocks, so without
  /// this the thread that would set the flag never runs. Counting guest operations instead of
  /// time keeps the switch points the same in every run.
  void Tick(XThread* thread);
  /// Tick for the calling thread, if it runs under the scheduler.
  static void TickCurrentThread();
  /// Called by recompiled code when a thread's loop budget (REX_LOOP_BACKEDGE) runs out: yields
  /// if the thread holds the token and isn't inside an interrupt-disabled section. Returns the
  /// next budget.
  static uint32_t OnLoopBudgetSpent(bool in_global_lock);

  bool Holds(XThread* thread);

 private:
  enum class State { kIdle, kRunnable, kBlocked, kRunning, kDead };
  struct Entry {
    XThread* thread = nullptr;
    uint64_t order = 0;
    State state = State::kIdle;
    int enter_depth = 0;
    uint32_t ticks = 0;
    const std::function<bool()>* ready = nullptr;
    std::condition_variable cv;
  };

  Entry* Find(XThread* thread);
  std::string DescribeStates();
  bool CanRun(Entry* entry);
  // Hands the token to the first thread after `from` (ending with `from` itself) that can run,
  // or leaves it free. Called with mutex_ held.
  void PassOn(Entry* from);
  // Waits until `entry` holds the token, re-running the selection while the token is free (a
  // host event or guest time may have made someone ready). Called with mutex_ held.
  void WaitForToken(Entry* entry, std::unique_lock<std::mutex>& lock);

  std::mutex mutex_;
  std::map<uint64_t, std::unique_ptr<Entry>> entries_;  // by order
  std::map<XThread*, Entry*> by_thread_;
  Entry* holder_ = nullptr;
  std::chrono::steady_clock::time_point token_free_since_;
  std::chrono::steady_clock::time_point holder_since_;
  bool holder_reported_ = false;
  bool idle_reported_ = false;
  Entry* last_holder_ = nullptr;
  uint64_t next_guest_order_ = 0;
  uint64_t next_host_order_ = uint64_t(1) << 32;
};

}  // namespace rex::system
