/**
 * WakeEvent -- lets the capture callback wake the processing thread when a
 * frame is ready, instead of the processing thread polling with sleeps.
 *
 * signal() is called from the real-time capture callback. It must not block
 * or allocate; the primitives used here are the ones host audio APIs use for
 * this purpose (WASAPI itself is event-driven):
 *   Windows : auto-reset event  (SetEvent / WaitForSingleObject)
 *   macOS   : dispatch semaphore (dispatch_semaphore_signal)
 *   Linux   : POSIX semaphore    (sem_post is async-signal-safe)
 *
 * wait() always has a timeout so the processing thread still services the
 * watchdog and the running flag when no audio arrives (e.g. device lost).
 */

#ifndef AINOICEGUARD_WAKE_EVENT_H
#define AINOICEGUARD_WAKE_EVENT_H

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <dispatch/dispatch.h>
#else
#include <errno.h>
#include <semaphore.h>
#include <time.h>
#endif

namespace ainoiceguard {

class WakeEvent {
 public:
  WakeEvent() {
#if defined(_WIN32)
    h_ = CreateEventW(nullptr, FALSE /* auto-reset */, FALSE, nullptr);
#elif defined(__APPLE__)
    s_ = dispatch_semaphore_create(0);
#else
    ok_ = sem_init(&s_, 0, 0) == 0;
#endif
  }

  ~WakeEvent() {
#if defined(_WIN32)
    if (h_) CloseHandle(h_);
#elif defined(__APPLE__)
    if (s_) dispatch_release(s_);
#else
    if (ok_) sem_destroy(&s_);
#endif
  }

  WakeEvent(const WakeEvent&) = delete;
  WakeEvent& operator=(const WakeEvent&) = delete;

  /** Real-time safe: never blocks. */
  void signal() {
#if defined(_WIN32)
    if (h_) SetEvent(h_);
#elif defined(__APPLE__)
    if (s_) dispatch_semaphore_signal(s_);
#else
    if (ok_) sem_post(&s_);
#endif
  }

  /** Block until signalled or `timeoutMs` elapses. */
  void wait(unsigned timeoutMs) {
#if defined(_WIN32)
    if (h_) WaitForSingleObject(h_, timeoutMs);
#elif defined(__APPLE__)
    if (s_) dispatch_semaphore_wait(s_, dispatch_time(DISPATCH_TIME_NOW, int64_t(timeoutMs) * 1000000));
#else
    if (!ok_) return;
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += long(timeoutMs % 1000) * 1000000L;
    ts.tv_sec += timeoutMs / 1000 + ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    while (sem_timedwait(&s_, &ts) == -1 && errno == EINTR) {
    }
#endif
  }

 private:
#if defined(_WIN32)
  HANDLE h_ = nullptr;
#elif defined(__APPLE__)
  dispatch_semaphore_t s_ = nullptr;
#else
  sem_t s_;
  bool ok_ = false;
#endif
};

}  // namespace ainoiceguard

#endif  // AINOICEGUARD_WAKE_EVENT_H
