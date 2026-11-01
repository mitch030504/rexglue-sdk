/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/ui/temporal_jitter.h>
#include <rex/stream.h>
#include <rex/system/guest_scheduler.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/string_utils.h>  // For TranslateAnsiStringAddress
#include <rex/system/xobject.h>
// #include <rex/kernel/xboxkrnl/private.h>  // TODO: JIT only
#include <rex/system/xenumerator.h>
#include <rex/system/xevent.h>
#include <rex/system/xfile.h>
#include <rex/system/xmodule.h>
#include <rex/system/xmutant.h>
#include <rex/system/xnotifylistener.h>
#include <rex/system/xsemaphore.h>
#include <rex/system/xsymboliclink.h>
#include <rex/system/xthread.h>

#include <cstring>
#include <rex/ppc/function.h>

REXCVAR_DEFINE_STRING(sync_trace_log, "", "Kernel",
                      "Determinism debugging: write every guest wait (thread, guest frame, "
                      "object, timeout, result) to this file. Slow; off when empty.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(debug_guest_watch, "", "Kernel",
                      "Determinism debugging: hex guest address of a word to compare around every "
                      "kernel export call; each change is logged with the caller's guest address "
                      "(and in sync_trace_log as an M line). Off when empty.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::ppc {

uint32_t g_debug_guest_watch = 0;

void ReportGuestWatchChange(uint8_t* base, uint32_t caller_lr, uint32_t before) {
  uint32_t after;
  std::memcpy(&after, base + g_debug_guest_watch, sizeof(after));
  const std::string detail =
      fmt::format("{:08X} {:08X}->{:08X} lr {:08X}", g_debug_guest_watch,
                  rex::byte_swap(before), rex::byte_swap(after), caller_lr);
  REXSYS_INFO("debug_guest_watch: {}", detail);
  if (system::XObject::SyncTraceEnabled()) {
    system::XObject::TraceSync("M", detail, 0);
  }
}

}  // namespace rex::ppc

namespace rex::system {

void XObject::InitDebugGuestWatch() {
  const std::string watch = REXCVAR_GET(debug_guest_watch);
  if (!watch.empty()) {
    ppc::g_debug_guest_watch = uint32_t(std::stoul(watch, nullptr, 16)) & ~uint32_t(3);
    REXSYS_INFO("debug_guest_watch: watching {:08X}", ppc::g_debug_guest_watch);
  }
}

bool XObject::SyncTraceEnabled() {
  return !REXCVAR_GET(sync_trace_log).empty();
}

void XObject::TraceSync(std::string_view op, std::string_view detail, uint32_t result) {
  static std::mutex mutex;
  static FILE* file = nullptr;
  const uint32_t thread_id = XThread::IsInThread() ? XThread::GetCurrentThread()->thread_id() : 0;
  const auto line = fmt::format("{} {} {} {} {:X}\n", thread_id,
                                rex::ui::temporal::GuestSwapCount(), op, detail, result);
  std::lock_guard<std::mutex> lock(mutex);
  if (!file) {
    file = std::fopen(REXCVAR_GET(sync_trace_log).c_str(), "w");
    if (!file) {
      return;
    }
  }
  std::fwrite(line.data(), 1, line.size(), file);
  // Flush once per guest frame: replays end by being killed.
  static uint64_t flushed_frame = 0;
  if (rex::ui::temporal::GuestSwapCount() != flushed_frame) {
    flushed_frame = rex::ui::temporal::GuestSwapCount();
    std::fflush(file);
  }
}

std::string XObject::SyncTraceName() const {
  // A type letter (Event, Mutant, Semaphore, Timer, ...), then native objects by guest address and
  // handle-only objects by handle.
  constexpr std::string_view kTypeLetters = "unEFIMXLSskyHT";
  const size_t type_index = size_t(type_);
  const char type = type_index < kTypeLetters.size() ? kTypeLetters[type_index] : '?';
  return guest_object_ptr_
             ? fmt::format("{}{:X}", type, guest_object_ptr_)
             : fmt::format("{}h{:X}", type, handles_.empty() ? 0u : uint32_t(handles_[0]));
}

XObject::XObject(Type type) : kernel_state_(nullptr), pointer_ref_count_(1), type_(type) {
  handles_.reserve(10);
}

XObject::XObject(KernelState* kernel_state, Type type)
    : kernel_state_(kernel_state),
      type_(type),
      pointer_ref_count_(1),
      guest_object_ptr_(0),
      allocated_guest_object_(false) {
  handles_.reserve(10);

  // TODO: Assert kernel_state != nullptr in this constructor.
  if (kernel_state) {
    kernel_state->object_table()->AddHandle(this, nullptr);
  }
}

XObject::~XObject() {
  // TODO: these are being asserted true... find out why
  // assert_true(handles_.empty());
  // assert_zero(pointer_ref_count_);

  if (allocated_guest_object_) {
    uint32_t ptr = guest_object_ptr_ - sizeof(X_OBJECT_HEADER);
    auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(ptr);

    // Free the object creation info
    if (header->object_type_ptr) {
      memory()->SystemHeapFree(header->object_type_ptr);
    }

    memory()->SystemHeapFree(ptr);
  }
}

rex::Runtime* XObject::emulator() const {
  return kernel_state_->emulator_;
}
KernelState* XObject::kernel_state() const {
  return kernel_state_;
}
rex::memory::Memory* XObject::memory() const {
  return kernel_state_->memory();
}

XObject::Type XObject::type() const {
  return type_;
}

void XObject::RetainHandle() {
  kernel_state_->object_table()->RetainHandle(handles_[0]);
}

bool XObject::ReleaseHandle() {
  // FIXME: Return true when handle is actually released.
  return kernel_state_->object_table()->ReleaseHandle(handles_[0]) == X_STATUS_SUCCESS;
}

void XObject::Retain() {
  ++pointer_ref_count_;
}

void XObject::Release() {
  if (--pointer_ref_count_ == 0) {
    delete this;
  }
}

X_STATUS XObject::Delete() {
  if (kernel_state_ == nullptr) {
    // Fake return value for api-scanner
    return X_STATUS_SUCCESS;
  } else {
    if (!name_.empty()) {
      kernel_state_->object_table()->RemoveNameMapping(name_);
    }
    return kernel_state_->object_table()->RemoveHandle(handles_[0]);
  }
}

bool XObject::SaveObject(stream::ByteStream* stream) {
  stream->Write<uint32_t>(allocated_guest_object_);
  stream->Write<uint32_t>(guest_object_ptr_);

  stream->Write(uint32_t(handles_.size()));
  stream->Write(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  return true;
}

bool XObject::RestoreObject(stream::ByteStream* stream) {
  allocated_guest_object_ = stream->Read<uint32_t>() > 0;
  guest_object_ptr_ = stream->Read<uint32_t>();

  handles_.resize(stream->Read<uint32_t>());
  stream->Read(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  // Restore our pointer to our handles in the object table.
  for (size_t i = 0; i < handles_.size(); i++) {
    kernel_state_->object_table()->RestoreHandle(handles_[i], this);
  }

  return true;
}

object_ref<XObject> XObject::Restore(KernelState* kernel_state, Type type,
                                     stream::ByteStream* stream) {
  switch (type) {
    case Type::Enumerator:
      break;
    case Type::Event:
      return XEvent::Restore(kernel_state, stream);
    case Type::File:
      return XFile::Restore(kernel_state, stream);
    case Type::IOCompletion:
      break;
    case Type::Module:
      return XModule::Restore(kernel_state, stream);
    case Type::Mutant:
      return XMutant::Restore(kernel_state, stream);
    case Type::NotifyListener:
      return XNotifyListener::Restore(kernel_state, stream);
    case Type::Semaphore:
      return XSemaphore::Restore(kernel_state, stream);
    case Type::Session:
      break;
    case Type::Socket:
      break;
    case Type::SymbolicLink:
      return XSymbolicLink::Restore(kernel_state, stream);
    case Type::Thread:
      return XThread::Restore(kernel_state, stream);
    case Type::Timer:
      break;
    case Type::Undefined:
      break;
  }

  assert_always("No restore handler exists for this object!");
  return nullptr;
}

void XObject::SetAttributes(uint32_t obj_attributes_ptr) {
  if (!obj_attributes_ptr) {
    return;
  }

  auto name = util::TranslateAnsiStringAddress(
      memory(),
      memory::load_and_swap<uint32_t>(memory()->TranslateVirtual(obj_attributes_ptr + 4)));
  if (!name.empty()) {
    name_ = std::string(name);
    kernel_state_->object_table()->AddNameMapping(name_, handles_[0]);
  }
}

namespace {

// Under clock_frame_locked guest time only moves at guest frames, so a host-time timeout would end
// a guest wait after a different number of guest frames each run (a 1 ms poll loop times out ~20
// times in one frame on one run and ~25 on the next). With Clock::GuestTimedWaits the timeout is a
// deadline in guest time instead: the wait goes on in short host slices until the object is
// signalled or guest time has passed the deadline, so a 1 ms timeout ends at the next guest frame.
constexpr std::chrono::milliseconds kGuestDeadlineSlice{1};

bool IsTimeout(rex::thread::WaitResult result) {
  return result == rex::thread::WaitResult::kTimeout;
}
bool IsTimeout(const std::pair<rex::thread::WaitResult, size_t>& result) {
  return IsTimeout(result.first);
}

// Under sched_deterministic (GuestScheduler) the calling thread holds the run token. A wait
// that can't succeed right away gives the token up until its objects are signalled (`signalled`
// peeks without consuming), its guest deadline passes or an APC arrives, and then retries.
template <typename WaitFn, typename SignalledFn>
auto WaitUntilGuestDeadline(const uint64_t* opt_timeout, std::chrono::milliseconds host_timeout,
                            bool alertable, WaitFn wait, SignalledFn signalled) {
  if (auto* scheduler = GuestScheduler::ForCurrentThread()) {
    auto result = wait(std::chrono::milliseconds(0));
    if (!IsTimeout(result)) {
      scheduler->Tick(XThread::GetCurrentThread());
      return result;
    }
    if (opt_timeout && !*opt_timeout) {
      // A poll that fails: whoever would signal the object needs the token to do it, so let
      // every other thread that can run go first (guest code that polls in a loop spins forever
      // otherwise).
      scheduler->YieldThread(XThread::GetCurrentThread());
      return result;
    }
    const bool has_deadline = opt_timeout != nullptr;
    const uint64_t deadline =
        has_deadline ? chrono::Clock::GuestDeadline(int64_t(*opt_timeout)) : 0;
    XThread* thread = XThread::GetCurrentThread();
    const auto passed = [&]() {
      return has_deadline && chrono::Clock::GuestDeadlinePassed(deadline);
    };
    scheduler->Block(
        thread,
        [&]() { return signalled() || passed() || (alertable && thread->HasPendingApcs()); },
        [&]() {
          result = wait(std::chrono::milliseconds(0));
          return !IsTimeout(result) || passed();
        });
    return result;
  }
  if (!opt_timeout || !*opt_timeout || !chrono::Clock::GuestTimedWaits()) {
    return wait(host_timeout);
  }
  const uint64_t deadline = chrono::Clock::GuestDeadline(int64_t(*opt_timeout));
  while (true) {
    auto result = wait(kGuestDeadlineSlice);
    if (!IsTimeout(result) || chrono::Clock::GuestDeadlinePassed(deadline)) {
      return result;
    }
    XThread::CheckTitleTermination();
  }
}

}  // namespace

uint32_t XObject::TimeoutTicksToMs(int64_t timeout_ticks) {
  if (timeout_ticks > 0) {
    // Absolute time, based on January 1, 1601.
    // TODO(benvanik): convert time to relative time.
    assert_always();
    return 0;
  } else if (timeout_ticks < 0) {
    // Relative time.
    return (uint32_t)(-timeout_ticks / 10000);  // Ticks -> MS
  } else {
    return 0;
  }
}

X_STATUS XObject::Wait(uint32_t wait_reason, uint32_t processor_mode, uint32_t alertable,
                       uint64_t* opt_timeout) {
  const X_STATUS status = [&]() -> X_STATUS {
    auto wait_handle = GetWaitHandle();
    if (!wait_handle) {
      // Object doesn't support waiting.
      return X_STATUS_SUCCESS;
    }

    auto timeout_ms =
        opt_timeout ? std::chrono::milliseconds(
                          chrono::Clock::ScaleGuestDurationMillis(TimeoutTicksToMs(*opt_timeout)))
                    : std::chrono::milliseconds::max();

    XThread::CheckTitleTermination();
    auto result = WaitUntilGuestDeadline(
        opt_timeout, timeout_ms, alertable,
        [&](auto slice) { return rex::thread::Wait(wait_handle, alertable ? true : false, slice); },
        [&]() { return rex::thread::IsSignaled(wait_handle); });
    XThread::CheckTitleTermination();
    switch (result) {
      case rex::thread::WaitResult::kSuccess:
        WaitCallback();
        return X_STATUS_SUCCESS;
      case rex::thread::WaitResult::kUserCallback:
        // Or X_STATUS_ALERTED?
        return X_STATUS_USER_APC;
      case rex::thread::WaitResult::kTimeout:
        rex::thread::MaybeYield();
        return X_STATUS_TIMEOUT;
      default:
      case rex::thread::WaitResult::kAbandoned:
      case rex::thread::WaitResult::kFailed:
        return X_STATUS_ABANDONED_WAIT_0;
    }
  }();
  if (SyncTraceEnabled()) {
    TraceSync("W",
              fmt::format("{} {}", SyncTraceName(),
                          opt_timeout ? fmt::format("{}", int64_t(*opt_timeout)) : "-"),
              status);
  }
  return status;
}

X_STATUS XObject::SignalAndWait(XObject* signal_object, XObject* wait_object, uint32_t wait_reason,
                                uint32_t processor_mode, uint32_t alertable,
                                uint64_t* opt_timeout) {
  const X_STATUS status = [&]() -> X_STATUS {
    auto timeout_ms =
        opt_timeout ? std::chrono::milliseconds(
                          chrono::Clock::ScaleGuestDurationMillis(TimeoutTicksToMs(*opt_timeout)))
                    : std::chrono::milliseconds::max();

    // Signal once; later slices of a guest-time timeout only wait.
    bool signalled = false;
    auto result = WaitUntilGuestDeadline(
        opt_timeout, timeout_ms, alertable,
        [&](auto slice) {
          if (std::exchange(signalled, true)) {
            return rex::thread::Wait(wait_object->GetWaitHandle(), alertable ? true : false, slice);
          }
          return rex::thread::SignalAndWait(signal_object->GetWaitHandle(),
                                            wait_object->GetWaitHandle(), alertable ? true : false,
                                            slice);
        },
        [&]() { return rex::thread::IsSignaled(wait_object->GetWaitHandle()); });
    switch (result) {
      case rex::thread::WaitResult::kSuccess:
        wait_object->WaitCallback();
        return X_STATUS_SUCCESS;
      case rex::thread::WaitResult::kUserCallback:
        // Or X_STATUS_ALERTED?
        return X_STATUS_USER_APC;
      case rex::thread::WaitResult::kTimeout:
        rex::thread::MaybeYield();
        return X_STATUS_TIMEOUT;
      default:
      case rex::thread::WaitResult::kAbandoned:
      case rex::thread::WaitResult::kFailed:
        return X_STATUS_ABANDONED_WAIT_0;
    }
  }();
  if (SyncTraceEnabled()) {
    TraceSync("S",
              fmt::format("{} {} {}", signal_object->SyncTraceName(), wait_object->SyncTraceName(),
                          opt_timeout ? fmt::format("{}", int64_t(*opt_timeout)) : "-"),
              status);
  }
  return status;
}

X_STATUS XObject::WaitMultiple(uint32_t count, XObject** objects, uint32_t wait_type,
                               uint32_t wait_reason, uint32_t processor_mode, uint32_t alertable,
                               uint64_t* opt_timeout) {
  const X_STATUS status = [&]() -> X_STATUS {
    std::vector<rex::thread::WaitHandle*> wait_handles(count);
    for (size_t i = 0; i < count; ++i) {
      wait_handles[i] = objects[i]->GetWaitHandle();
      assert_not_null(wait_handles[i]);
    }

    auto timeout_ms =
        opt_timeout ? std::chrono::milliseconds(
                          chrono::Clock::ScaleGuestDurationMillis(TimeoutTicksToMs(*opt_timeout)))
                    : std::chrono::milliseconds::max();

    XThread::CheckTitleTermination();
    if (wait_type) {
      auto result = WaitUntilGuestDeadline(
          opt_timeout, timeout_ms, alertable,
          [&](auto slice) {
            return rex::thread::WaitAny(wait_handles.data(), wait_handles.size(),
                                        alertable ? true : false, slice);
          },
          [&]() {
            return std::any_of(wait_handles.begin(), wait_handles.end(),
                               [](auto* handle) { return rex::thread::IsSignaled(handle); });
          });
      XThread::CheckTitleTermination();
      switch (result.first) {
        case rex::thread::WaitResult::kSuccess:
          objects[result.second]->WaitCallback();

          return X_STATUS(result.second);
        case rex::thread::WaitResult::kUserCallback:
          // Or X_STATUS_ALERTED?
          return X_STATUS_USER_APC;
        case rex::thread::WaitResult::kTimeout:
          rex::thread::MaybeYield();
          return X_STATUS_TIMEOUT;
        default:
        case rex::thread::WaitResult::kAbandoned:
          return X_STATUS(X_STATUS_ABANDONED_WAIT_0 + result.second);
        case rex::thread::WaitResult::kFailed:
          return X_STATUS_UNSUCCESSFUL;
      }
    } else {
      auto result = WaitUntilGuestDeadline(
          opt_timeout, timeout_ms, alertable,
          [&](auto slice) {
            return rex::thread::WaitAll(wait_handles.data(), wait_handles.size(),
                                        alertable ? true : false, slice);
          },
          [&]() {
            return std::all_of(wait_handles.begin(), wait_handles.end(),
                               [](auto* handle) { return rex::thread::IsSignaled(handle); });
          });
      XThread::CheckTitleTermination();
      switch (result) {
        case rex::thread::WaitResult::kSuccess:
          for (uint32_t i = 0; i < count; i++) {
            objects[i]->WaitCallback();
          }

          return X_STATUS_SUCCESS;
        case rex::thread::WaitResult::kUserCallback:
          // Or X_STATUS_ALERTED?
          return X_STATUS_USER_APC;
        case rex::thread::WaitResult::kTimeout:
          rex::thread::MaybeYield();
          return X_STATUS_TIMEOUT;
        default:
        case rex::thread::WaitResult::kAbandoned:
        case rex::thread::WaitResult::kFailed:
          return X_STATUS_ABANDONED_WAIT_0;
      }
    }
  }();
  if (SyncTraceEnabled()) {
    std::string names;
    for (uint32_t i = 0; i < count; ++i) {
      names += (i ? "," : "") + objects[i]->SyncTraceName();
    }
    TraceSync(
        wait_type ? "WA" : "WL",
        fmt::format("{} {}", names, opt_timeout ? fmt::format("{}", int64_t(*opt_timeout)) : "-"),
        status);
  }
  return status;
}

uint8_t* XObject::CreateNative(uint32_t size) {
  auto global_lock = rex::thread::global_critical_region::AcquireDirect();

  uint32_t total_size = size + sizeof(X_OBJECT_HEADER);

  auto mem = memory()->SystemHeapAlloc(total_size);
  if (!mem) {
    // Out of memory!
    return nullptr;
  }

  allocated_guest_object_ = true;
  memory()->Zero(mem, total_size);
  SetNativePointer(mem + sizeof(X_OBJECT_HEADER), true);

  auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(mem);

  auto object_type = memory()->SystemHeapAlloc(sizeof(X_OBJECT_TYPE));
  if (object_type) {
    // Set it up in the header.
    // Some kernel method is accessing this struct and dereferencing a member
    // @ offset 0x14
    header->object_type_ptr = object_type;
  }

  return memory()->TranslateVirtual(guest_object_ptr_);
}

void XObject::SetNativePointer(uint32_t native_ptr, bool uninitialized) {
  auto global_lock = rex::thread::global_critical_region::AcquireDirect();

  // If hit: We've already setup the native ptr with CreateNative!
  assert_zero(guest_object_ptr_);

  auto header = kernel_state_->memory()->TranslateVirtual<X_DISPATCH_HEADER*>(native_ptr);

  // Memory uninitialized, so don't bother with the check.
  if (!uninitialized) {
    assert_true(!(header->wait_list_blink & 0x1));
  }

  // Stash pointer in struct.
  // FIXME: This assumes the object has a dispatch header (some don't!)
  StashHandle(header, handle());

  guest_object_ptr_ = native_ptr;
}

object_ref<XObject> XObject::GetNativeObject(KernelState* kernel_state, void* native_ptr,
                                             int32_t as_type) {
  assert_not_null(native_ptr);

  // Unfortunately the XDK seems to inline some KeInitialize calls, meaning
  // we never see it and just randomly start getting passed events/timers/etc.
  // Luckily it seems like all other calls (Set/Reset/Wait/etc) are used and
  // we don't have to worry about PPC code poking the struct. Because of that,
  // we init on first use, store our handle in the struct, and dereference it
  // each time.
  // We identify this by setting wait_list_flink to a magic value. When set,
  // wait_list_blink will hold a handle to our object.

  auto global_lock = rex::thread::global_critical_region::AcquireDirect();

  auto header = reinterpret_cast<X_DISPATCH_HEADER*>(native_ptr);

  if (as_type == -1) {
    as_type = header->type;
  }

  if (header->wait_list_flink == kXObjSignature) {
    // Already initialized.
    // TODO: assert if the type of the object != as_type
    uint32_t handle = header->wait_list_blink;
    auto object = kernel_state->object_table()->LookupObject<XObject>(handle);

    // TODO(benvanik): assert nothing has been changed in the struct.
    return object;
  } else {
    // First use, create new.
    // https://www.nirsoft.net/kernel_struct/vista/KOBJECTS.html
    XObject* object = nullptr;
    switch (as_type) {
      case 0:  // EventNotificationObject
      case 1:  // EventSynchronizationObject
      {
        auto ev = new XEvent(kernel_state);
        ev->InitializeNative(native_ptr, header);
        object = ev;
      } break;
      case 2:  // MutantObject
      {
        auto mutant = new XMutant(kernel_state);
        mutant->InitializeNative(native_ptr, header);
        object = mutant;
      } break;
      case 5:  // SemaphoreObject
      {
        auto sem = new XSemaphore(kernel_state);
        auto success = sem->InitializeNative(native_ptr, header);
        // Can't report failure to the guest at late initialization:
        assert_true(success);
        object = sem;
      } break;
      case 3:   // ProcessObject
      case 4:   // QueueObject
      case 6:   // ThreadObject
      case 7:   // GateObject
      case 8:   // TimerNotificationObject
      case 9:   // TimerSynchronizationObject
      case 18:  // ApcObject
      case 19:  // DpcObject
      case 20:  // DeviceQueueObject
      case 21:  // EventPairObject
      case 22:  // InterruptObject
      case 23:  // ProfileObject
      case 24:  // ThreadedDpcObject
      default:
        assert_always();
        return NULL;
    }

    // Stash pointer in struct.
    // FIXME: This assumes the object contains a dispatch header (some don't!)
    StashHandle(header, object->handle());

    return object_ref<XObject>(object);
  }
}

}  // namespace rex::system
