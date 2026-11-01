#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include <rex/thread.h>

using namespace std::chrono_literals;
using rex::thread::IsSignaled;
using rex::thread::Wait;
using rex::thread::WaitResult;

TEST_CASE("Readiness peeks do not consume event signals", "[threading][readiness]") {
  for (bool manual : {false, true}) {
    auto event = manual ? rex::thread::Event::CreateManualResetEvent(false)
                        : rex::thread::Event::CreateAutoResetEvent(false);
    REQUIRE(event);
    CHECK_FALSE(IsSignaled(event.get()));
    event->Set();
    CHECK(IsSignaled(event.get()));
    CHECK(IsSignaled(event.get()));
    CHECK(Wait(event.get(), false, 0ms) == WaitResult::kSuccess);
    CHECK(IsSignaled(event.get()) == manual);
    event->Reset();
    CHECK_FALSE(IsSignaled(event.get()));
  }
}

TEST_CASE("Readiness peeks preserve semaphore counts", "[threading][readiness]") {
  auto semaphore = rex::thread::Semaphore::Create(0, 2);
  REQUIRE(semaphore);
  CHECK_FALSE(IsSignaled(semaphore.get()));
  REQUIRE(semaphore->Release(2, nullptr));
  CHECK(IsSignaled(semaphore.get()));
  CHECK(IsSignaled(semaphore.get()));
  CHECK(Wait(semaphore.get(), false, 0ms) == WaitResult::kSuccess);
  CHECK(IsSignaled(semaphore.get()));
  CHECK(Wait(semaphore.get(), false, 0ms) == WaitResult::kSuccess);
  CHECK_FALSE(IsSignaled(semaphore.get()));
  CHECK(Wait(semaphore.get(), false, 0ms) == WaitResult::kTimeout);
}

TEST_CASE("Mutex readiness preserves ownership and recursion", "[threading][readiness]") {
  auto mutant = rex::thread::Mutant::Create(true);
  REQUIRE(mutant);
  CHECK(IsSignaled(mutant.get()));  // The owner can acquire recursively.
  CHECK(IsSignaled(mutant.get()));
  std::atomic<bool> ready{true};
  std::thread observer([&] { ready = IsSignaled(mutant.get()); });
  observer.join();
  CHECK_FALSE(ready.load());
  CHECK(mutant->Release());  // A peek must not add a recursive acquisition.
  CHECK_FALSE(mutant->Release());
  CHECK(IsSignaled(mutant.get()));
  CHECK(Wait(mutant.get(), false, 0ms) == WaitResult::kSuccess);
  CHECK(mutant->Release());
}

TEST_CASE("Thread readiness changes only on thread completion", "[threading][readiness]") {
  auto release = rex::thread::Event::CreateManualResetEvent(false);
  REQUIRE(release);
  auto thread = rex::thread::Thread::Create({}, [&] { Wait(release.get(), false); });
  REQUIRE(thread);
  CHECK_FALSE(IsSignaled(thread.get()));
  release->Set();
  REQUIRE(Wait(thread.get(), false, 2s) == WaitResult::kSuccess);
  CHECK(IsSignaled(thread.get()));
  CHECK(IsSignaled(thread.get()));
}

TEST_CASE("Readiness peeks preserve synchronization timer signals", "[threading][readiness]") {
  auto timer = rex::thread::Timer::CreateSynchronizationTimer();
  REQUIRE(timer);
  CHECK_FALSE(IsSignaled(timer.get()));
  REQUIRE(timer->SetOnceAfter(1ms));
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!IsSignaled(timer.get()) && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  CHECK(IsSignaled(timer.get()));
  CHECK(IsSignaled(timer.get()));
  CHECK(Wait(timer.get(), false, 0ms) == WaitResult::kSuccess);
  CHECK_FALSE(IsSignaled(timer.get()));
}
