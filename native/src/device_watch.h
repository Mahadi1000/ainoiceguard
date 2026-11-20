/**
 * DeviceWatcher -- notices audio endpoints being added, removed, enabled,
 * disabled, or the OS default changing, while the app runs.
 *
 * PortAudio only re-enumerates devices on Pa_Initialize, so on its own the
 * engine cannot tell that a headset was plugged back in. On Windows this uses
 * IMMNotificationClient on a dedicated COM (MTA) thread. Notifications come in
 * bursts (one physical plug fires several), so they are debounced (300 ms of
 * quiet) into one "devices changed" signal:
 *   - changeCount() increments once per burst (polled by the engine's
 *     watchdog, which then decides via device_policy.h), and
 *   - the callback fires once per burst (the addon forwards it to JS so the
 *     device lists refresh).
 *
 * snapshot() lists active endpoints and the default devices by the same
 * friendly names PortAudio's WASAPI host uses, so ids match getDevices().
 *
 * Other platforms: stub (start() returns false, snapshot() returns false).
 */

#ifndef AINOICEGUARD_DEVICE_WATCH_H
#define AINOICEGUARD_DEVICE_WATCH_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include "device_policy.h"

namespace ainoiceguard {

class DeviceWatcher {
 public:
  using Callback = std::function<void()>;

  static DeviceWatcher& instance();

  /** Start watching (idempotent). Returns false if unsupported or failed. */
  bool start();
  /** Stop watching and join the thread (idempotent). */
  void stop();

  /** Called once per debounced burst, on the watcher thread. */
  void setCallback(Callback cb);

  /** Increments once per debounced burst of endpoint changes. */
  uint64_t changeCount() const { return changes_.load(std::memory_order_acquire); }

  /**
   * Active endpoints + defaults of the WASAPI host. Initialises COM on the
   * calling thread if needed. Returns false where unsupported or on error.
   */
  static bool snapshot(EndpointSnapshot& out);

  /** Called by the COM notification object: something changed. */
  void notifyRaw();

 private:
  DeviceWatcher() = default;
  void run();

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> changes_{0};
  std::mutex cbMutex_;
  Callback cb_;
  void* stopEvent_ = nullptr;    /* HANDLE (Windows) */
  void* changeEvent_ = nullptr;  /* HANDLE (Windows) */
};

}  // namespace ainoiceguard

#endif  // AINOICEGUARD_DEVICE_WATCH_H
