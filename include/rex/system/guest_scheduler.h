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
 * Host threads that call into guest code (the audio callback, the GPU command processor and its
 * interrupts, vblank) take the token around the call with Enter/Leave. With SetDue, their turns
 * come at fixed points too: a host thread is picked in the round-robin as soon as its condition
 * on guest state holds, and the token waits for it to arrive.
 *
 * When nobody can run, only guest time can help (every waiter is on a timeout or on vblank), so
 * time advances by a virtual frame right away instead of after host time passes. Guest code that
 * spins on the clock without blocking gets a virtual frame after a fixed amount of guest work.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <atomic>
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
  /// Makes a host thread's turns deterministic: from now on Enter waits until `due` (evaluated by
  /// whichever thread picks the next holder, on guest state only) holds and the thread's turn
  /// comes, and the token is held for the thread while it is on its way to Enter. The thread must
  /// call Enter promptly whenever `due` holds, or the guest waits for it.
  void SetDue(XThread* thread, std::function<bool()> due);

  /// A guest frame (VdSwap) happened: restarts the count of guest work without a frame.
  void OnGuestFrame();

  /// Gives up the token until `ready` (evaluated by whichever thread picks the next holder) is
  /// true and then `attempt` (run on this thread, holding the token) reports success.
  void Block(XThread* thread, const std::function<bool()>& ready,
             const std::function<bool()>& attempt);
  /// Lets every other runnable thread go first.
  void YieldThread(XThread* thread);
  /// Yield for a thread spinning on guest memory another thread must change (a spinlock): also
  /// works inside an interrupt (see NoSwitchScope).
  void YieldSpinning(XThread* thread);
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

  /// While one exists on a thread, it doesn't give up the token voluntarily (Tick, Yield, loop
  /// budgets): it holds a lock every other guest thread may need, such as the global lock of an
  /// interrupt. With `interrupt_lock` (an interrupt holding the global lock), a loop budget running
  /// out in guest code still yields, with that lock released meanwhile: the handler spins on
  /// something the interrupted thread has to do first.
  class NoSwitchScope {
   public:
    explicit NoSwitchScope(std::unique_lock<std::recursive_mutex>* interrupt_lock = nullptr);
    ~NoSwitchScope();
    NoSwitchScope(const NoSwitchScope&) = delete;
    NoSwitchScope& operator=(const NoSwitchScope&) = delete;

   private:
    std::unique_lock<std::recursive_mutex>* outer_interrupt_lock_;
  };

 private:
  enum class State { kIdle, kRunnable, kBlocked, kRunning, kDead };
  struct Entry {
    XThread* thread = nullptr;
    uint64_t order = 0;
    State state = State::kIdle;
    int enter_depth = 0;
    uint32_t ticks = 0;
    uint32_t quantum = 16;  // Sync operations per turn (see Tick).
    std::chrono::steady_clock::duration held{};  // Token time since the last stats report.
    uint64_t turns = 0;                            // Turns since the last stats report.
    const std::function<bool()>* ready = nullptr;
    std::function<bool()> due;  // Host threads with fixed turns (SetDue).
    std::condition_variable cv;
  };

  Entry* Find(XThread* thread);
  // Find for the calling thread's own entry, cached per host thread.
  Entry* CachedEntry(XThread* thread);
  std::string DescribeStates();
  bool CanRun(Entry* entry);
  // Hands the token to the first thread after `from` (ending with `from` itself) that can run,
  // or leaves it free. Called with mutex_ held.
  void PassOn(Entry* from);
  // Waits until `entry` holds the token, re-running the selection while the token is free (a
  // host event or guest time may have made someone ready). Called with mutex_ held.
  void WaitForToken(Entry* entry, std::unique_lock<std::mutex>& lock);
  // The holder spent a loop quantum of `backedges`: counts guest work toward a virtual frame.
  void CountLoopQuantum(uint32_t backedges);
  // Yield; true if another thread got a turn.
  bool YieldTurn(XThread* thread);
  // Yield from an interrupt's guest code with the interrupt's global lock released meanwhile;
  // false (and nothing happens) if that isn't safe here.
  bool YieldInterrupt(XThread* thread);

  std::mutex mutex_;
  std::map<uint64_t, std::unique_ptr<Entry>> entries_;  // by order
  std::map<XThread*, Entry*> by_thread_;
  std::atomic<Entry*> holder_{nullptr};
  std::chrono::steady_clock::time_point token_free_since_;
  std::chrono::steady_clock::time_point holder_since_;
  bool holder_reported_ = false;
  bool idle_reported_ = false;
  Entry* last_holder_ = nullptr;
  // Virtual frames added in a row because nobody could run (reset when someone runs).
  uint32_t idle_frames_in_a_row_ = 0;
  uint64_t idle_frames_total_ = 0;
  // Backward branches (REX_LOOP_BACKEDGE) taken since the last guest or virtual frame.
  uint64_t loop_backedges_since_frame_ = 0;
  uint64_t busy_frames_total_ = 0;
  // Stats for the periodic report (OnGuestFrame).
  uint64_t frames_ = 0;
  uint64_t handoffs_ = 0;
  std::chrono::steady_clock::time_point report_since_;
  std::chrono::steady_clock::duration free_time_{};
  uint64_t next_guest_order_ = 0;
  static constexpr uint64_t next_host_order_base_ = uint64_t(1) << 32;
  uint64_t next_host_order_ = next_host_order_base_;
};

}  // namespace rex::system
