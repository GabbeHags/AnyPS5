#include "prx/libkernel/Semaphore/include/Semaphore.hpp"
#include "prx/libkernel/include/Api.hpp"
#include "prx/libkernel/Time/include/Time.hpp"
#include "prx/libc/include/General.hpp"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include <stdexcept>
#include <string>
#include <utility>

KernelSemaPrivate::KernelSemaPrivate(std::int32_t initCount, std::int32_t maxCount, std::string name, bool isFifo)
 : name(std::move(name)), tokenCount(initCount), initCount(initCount), maxCount(maxCount), isFifo(isFifo) {
}

static bool TraceSync() {
 static const bool enabled = std::getenv("APS5_TRACE_SYNC") != nullptr;
 return enabled;
}

extern "C" {

int APS5_VABI sceKernelCreateSema(KernelSema* sem, const char* name, uint32_t attr, int init, int max, void* opt) {
 (void)opt;
 if (sem == nullptr) {
  APS5_INVALID_ARG_EX;
 }
 if ((attr & ~0x103u) != 0 || (attr & 3u) == 3u || name == nullptr || init < 0 || max <= 0 || init > max) {
  return SCE_KERNEL_ERROR_EINVAL;
 }
 try {
  std::string nameCopy;
  const auto maximum = nameCopy.max_size();
  for (std::size_t capacity = 32;; capacity += std::min(capacity, maximum - capacity)) {
   const int error = CopyGuestString_nid_no_patch(name, capacity, nameCopy);
   if (error == 0) break;
   if (error != 63) return SceKernelError(error);
   if (capacity == maximum) return SCE_KERNEL_ERROR_ENOMEM;
  }
  auto semaphore = std::make_unique<KernelSemaPrivate>(init, max, nameCopy, (attr & 3u) == 1u);
  semaphore->priorityQueue = (attr & 3u) == 2u;
  const KernelSema handle = semaphore.get();
  if (!GuestCopyOut_nid_no_patch(sem, &handle, sizeof(handle))) return SCE_KERNEL_ERROR_EFAULT;
  semaphore.release();
  if (TraceSync()) std::fprintf(stderr, "[sema] create %p '%s' attr=0x%x init=%d max=%d\n", static_cast<void*>(handle), nameCopy.c_str(), attr, init, max);
 } catch (const std::bad_alloc&) {
  return SCE_KERNEL_ERROR_ENOMEM;
 }
 return KERNEL_SEMA_OK;
}

int APS5_VABI sceKernelPollSema(KernelSema sem, int need) {
 if (sem == nullptr) {
  return SCE_KERNEL_ERROR_ESRCH;
 }
 if (need <= 0 || need > sem->maxCount) {
  return SCE_KERNEL_ERROR_EINVAL;
 }

 std::lock_guard<std::mutex> lock(sem->mutex);
 static const bool traceLong = std::getenv("APS5_TRACE_LONG_WAITS") != nullptr;
 if (sem->tokenCount < need) {
  if (traceLong && sem->pollFailingSince == std::chrono::steady_clock::time_point{}) sem->pollFailingSince = std::chrono::steady_clock::now();
  return SCE_KERNEL_ERROR_EBUSY;
 }
 if (traceLong && sem->pollFailingSince != std::chrono::steady_clock::time_point{}) {
  const auto polled = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - sem->pollFailingSince).count();
  if (polled >= 300) std::fprintf(stderr, "[long-poll] sema %p '%s' polled %lld ms by %p thread %lu, signaled by thread %lu stack %p %p %p %p %p %p %p %p\n", static_cast<void*>(sem), sem->name.c_str(), static_cast<long long>(polled), __builtin_return_address(0), GetCurrentThreadId(), sem->lastSignalThread, sem->lastSignalStack[0], sem->lastSignalStack[1], sem->lastSignalStack[2], sem->lastSignalStack[3], sem->lastSignalStack[4], sem->lastSignalStack[5], sem->lastSignalStack[6], sem->lastSignalStack[7]);
 }
 sem->pollFailingSince = {};
 sem->tokenCount -= need;
 return KERNEL_SEMA_OK;
}

int APS5_VABI sceKernelSignalSema(KernelSema sem, int count) {
 const void* signalCaller = __builtin_return_address(0);
 const void* signalStack[8] = {};
 {
  auto* frame = static_cast<std::uintptr_t*>(__builtin_frame_address(0));
  for (int i = 0; i < 8 && frame != nullptr && reinterpret_cast<std::uintptr_t>(frame) % 8 == 0; ++i) {
   signalStack[i] = reinterpret_cast<const void*>(frame[1]);
   auto* next = reinterpret_cast<std::uintptr_t*>(frame[0]);
   if (next <= frame || reinterpret_cast<std::uintptr_t>(next) - reinterpret_cast<std::uintptr_t>(frame) > 0x100000) break;
   frame = next;
  }
 }
 if (sem == nullptr) {
  return SCE_KERNEL_ERROR_ESRCH;
 }
 if (count <= 0) {
  return SCE_KERNEL_ERROR_EINVAL;
 }

 std::lock_guard<std::mutex> lock(sem->mutex);
 if (count > sem->maxCount - sem->tokenCount) {
  return SCE_KERNEL_ERROR_EINVAL;
 }
 const auto before = sem->tokenCount;
 std::copy(std::begin(signalStack), std::end(signalStack), std::begin(sem->lastSignalStack));
 sem->lastSignalThread = GetCurrentThreadId();
 sem->tokenCount += count;
 if (TraceSync()) std::fprintf(stderr, "[sema] signal %p '%s' count=%d have=%d->%d\n", static_cast<void*>(sem), sem->name.c_str(), count, before, sem->tokenCount);
 for (auto it = sem->queue.begin(); it != sem->queue.end();) {
  KernelSemaWaiter* waiter = *it;
  if (waiter->need > sem->tokenCount) {
   ++it;
   continue;
  }
  sem->tokenCount -= waiter->need;
  waiter->released = true;
  waiter->releasedBy = signalCaller;
  std::copy(std::begin(signalStack), std::end(signalStack), std::begin(waiter->releasedStack));
  waiter->releasedThread = GetCurrentThreadId();
  it = sem->queue.erase(it);
 }
 sem->condition.NotifyAll();
 return KERNEL_SEMA_OK;
}

int APS5_VABI sceKernelWaitSema(KernelSema sem, int need, KernelUseconds* time) {
 if (sem == nullptr) {
  return SCE_KERNEL_ERROR_ESRCH;
 }
 if (need <= 0 || need > sem->maxCount) {
  return SCE_KERNEL_ERROR_EINVAL;
 }

 std::unique_lock<std::mutex> lock(sem->mutex);
 const void* caller = __builtin_return_address(0);
 if (TraceSync()) std::fprintf(stderr, "[sema] wait begin %p '%s' need=%d have=%d timeout=%lld caller=%p\n", static_cast<void*>(sem), sem->name.c_str(), need, sem->tokenCount, time ? static_cast<long long>(*time) : -1LL, caller);
 const auto traceSyncWaitEnd = [&](int result) {
  if (TraceSync()) std::fprintf(stderr, "[sema] wait end %p '%s' result=0x%x have=%d caller=%p\n", static_cast<void*>(sem), sem->name.c_str(), result, sem->tokenCount, caller);
 };
 if (sem->deleted) {
  traceSyncWaitEnd(SCE_KERNEL_ERROR_EACCES);
  return SCE_KERNEL_ERROR_EACCES;
 }
 if (sem->tokenCount >= need) {
  sem->tokenCount -= need;
  traceSyncWaitEnd(KERNEL_SEMA_OK);
  return KERNEL_SEMA_OK;
 }
 if (time != nullptr && *time == 0) {
  if (KernelTraceWaitEnabled()) KernelTraceWait_nid_postfix("sema", caller, 0, true);
  traceSyncWaitEnd(SCE_KERNEL_ERROR_ETIMEDOUT);
  return SCE_KERNEL_ERROR_ETIMEDOUT;
 }
 const std::uint64_t generation = sem->cancelGeneration;
 KernelSemaWaiter waiter{need};
 if (sem->priorityQueue && scePthreadGetprio(scePthreadSelf(), &waiter.priority) == 0) {
  const auto lower = std::find_if(sem->queue.begin(), sem->queue.end(), [&](const KernelSemaWaiter* queued) { return queued->priority > waiter.priority; });
  sem->queue.insert(lower, &waiter);
 } else {
  sem->queue.push_back(&waiter);
 }
 ++sem->waiterCount;
 struct WaiterGuard {
  KernelSemaPrivate* sem;
  KernelSemaWaiter* waiter;
  ~WaiterGuard() {
   --sem->waiterCount;
   if (!waiter->released) {
    sem->queue.remove(waiter);
   }
   sem->condition.NotifyAll();
  }
 } waiterGuard{sem, &waiter};

 const bool traceWaits = KernelTraceWaitEnabled();
 const auto waitStart = traceWaits ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
 const auto traceWait = [&](bool timedOut) {
  if (traceWaits) KernelTraceWait_nid_postfix("sema", caller, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count()), timedOut);
 };
 const auto released = [&] { return waiter.released || sem->deleted || sem->cancelGeneration != generation; };
 bool acquired = true;
 if (time == nullptr) {
  sem->condition.Wait(lock, released);
 } else {
  const std::uint64_t deadline = TimedWait::DeadlineNanos(*time);
  acquired = sem->condition.WaitUntil(lock, deadline, released);
  *time = static_cast<KernelUseconds>(TimedWait::RemainingMicros(deadline));
 }
 traceWait(!acquired);
 if (std::getenv("APS5_TRACE_LONG_WAITS") != nullptr && waiter.releasedBy != nullptr && std::chrono::steady_clock::now() - waitStart >= std::chrono::milliseconds(500)) std::fprintf(stderr, "[long-wait] sema %p '%s' released by caller %p thread waited from %p signaler thread %lu stack %p %p %p %p %p %p %p %p\n", static_cast<void*>(sem), sem->name.c_str(), waiter.releasedBy, caller, waiter.releasedThread, waiter.releasedStack[0], waiter.releasedStack[1], waiter.releasedStack[2], waiter.releasedStack[3], waiter.releasedStack[4], waiter.releasedStack[5], waiter.releasedStack[6], waiter.releasedStack[7]);
 if (waiter.released) {
  traceSyncWaitEnd(KERNEL_SEMA_OK);
  return KERNEL_SEMA_OK;
 }
 if (sem->deleted) {
  traceSyncWaitEnd(SCE_KERNEL_ERROR_EACCES);
  return SCE_KERNEL_ERROR_EACCES;
 }
 if (sem->cancelGeneration != generation) {
  traceSyncWaitEnd(SCE_KERNEL_ERROR_ECANCELED);
  return SCE_KERNEL_ERROR_ECANCELED;
 }
 traceSyncWaitEnd(SCE_KERNEL_ERROR_ETIMEDOUT);
 return SCE_KERNEL_ERROR_ETIMEDOUT;
}

int APS5_VABI sceKernelCancelSema(KernelSema sem, int count, int* threads) {
 if (sem == nullptr) {
  return SCE_KERNEL_ERROR_ESRCH;
 }

 std::lock_guard<std::mutex> lock(sem->mutex);
 if (count > sem->maxCount) {
  return SCE_KERNEL_ERROR_EINVAL;
 }
 if (threads != nullptr) {
  const int waiting = static_cast<int>(sem->queue.size());
  if (!GuestCopyOut_nid_no_patch(threads, &waiting, sizeof(waiting))) return SCE_KERNEL_ERROR_EFAULT;
 }
 sem->tokenCount = count < 0 ? sem->initCount : count;
 sem->queue.clear();
 ++sem->cancelGeneration;
 sem->condition.NotifyAll();
 return KERNEL_SEMA_OK;
}

int APS5_VABI sceKernelDeleteSema(KernelSema sem) {
 if (sem == nullptr) {
  return SCE_KERNEL_ERROR_ESRCH;
 }

 std::unique_lock<std::mutex> lock(sem->mutex);
 sem->deleted = true;
 sem->condition.NotifyAll();
 sem->condition.Wait(lock, [&] { return sem->waiterCount == 0; });
 lock.unlock();
 delete sem;
 return KERNEL_SEMA_OK;
}

}
