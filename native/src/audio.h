/**
 * AudioEngine -- PortAudio-based real-time capture/playback with RNNoise processing.
 *
 * Architecture:
 *   [Mic] -> CaptureCallback -> captureRing_ -> ProcessingThread -> outputRing_ -> OutputCallback -> [Speaker/VB-Cable]
 *
 * REAL-TIME RULES ENFORCED:
 * - Capture/Output callbacks: NO allocations, NO locks, NO syscalls.
 *   They only read/write the lock-free ring buffers.
 * - Processing thread: Allowed to call RNNoise (which is allocation-free per frame).
 *   Spins on captureRing_ with a short sleep to avoid burning CPU.
 *
 * WASAPI NOTES (Windows):
 * - We attempt exclusive mode for lowest latency. Falls back to shared if unavailable.
 * - WASAPI exclusive locks the device -- no other app can use it simultaneously.
 */

#ifndef AINOICEGUARD_AUDIO_H
#define AINOICEGUARD_AUDIO_H

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "device_select.h"
#include "drift.h"
#include "engine_stats.h"
#include "ringbuffer.h"
#include "rnnoise_wrapper.h"
#include "wake_event.h"

/* Forward-declare PortAudio types to avoid including portaudio.h in this header. */
typedef void PaStream;
struct PaStreamCallbackTimeInfo;
typedef unsigned long PaStreamCallbackFlags;

namespace ainoiceguard {

/** Audio device info exposed to JavaScript. */
struct DeviceInfo : DeviceEntry {
  double defaultSampleRate = 0.0;
  bool preferredHostApi = false;  /* on the host API the engine prefers (WASAPI on Windows) */
};

/** Configuration for the audio engine. */
struct AudioConfig {
  DeviceSpec input = DeviceSpec::def();   /* default = preferred host API's default */
  DeviceSpec output = DeviceSpec::def();  /* DeviceSpec::none() = output muted */
  double sampleRate = 48000.0;
  unsigned long framesPerBuffer = 480;  /* 10ms @ 48kHz = RNNoise frame size */
  /*
   * WASAPI exclusive mode is opt-in. Phase 3 measured that it was never
   * obtained on the test machine (silent fallback), and when it does succeed
   * it locks the microphone away from every other application.
   */
  bool tryExclusiveMode = false;
  /*
   * Compensate clock drift between the capture and output devices (e.g.
   * mic vs VB-Cable) by steering the output ring to a fixed level with a
   * tiny resampling correction (see drift.h). Only active with an output.
   */
  bool driftCompensation = true;
};

/** Live state of the drift compensation. */
struct DriftSnapshot {
  bool enabled = false;
  double ppm = 0.0;           /* applied correction; ~ the measured clock mismatch */
  double level = 0.0;         /* smoothed continuous output-ring level (samples) */
  double targetLevel = 0.0;   /* samples */
};

/** What the engine actually opened (resolved after defaults and fallbacks). */
struct StreamSnapshot {
  int inputDevice = -1;          /* PortAudio index, -1 = none */
  int outputDevice = -1;         /* -1 = none (output muted) */
  bool inputExclusive = false;   /* WASAPI exclusive mode obtained */
  bool outputExclusive = false;
  double sampleRate = 0.0;
  double inputLatencyMs = 0.0;   /* as reported by Pa_GetStreamInfo() */
  double outputLatencyMs = 0.0;
  size_t ringCapacity = 0;       /* samples, per ring */
  bool processingMmcss = false;  /* processing thread registered with MMCSS (Windows) */
};

/**
 * Callback for engine status changes. `type` is one of:
 *   "device-lost", "restarted", "restart-failed", "fallback"
 * Called from the processing thread -- keep it lightweight.
 */
using StatusCallback =
    std::function<void(const std::string& type, const std::string& message)>;

class AudioEngine {
 public:
  AudioEngine();
  ~AudioEngine();

  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  /**
   * Enumerate all available audio devices (fresh list when the engine is
   * stopped; while running, PortAudio keeps the list from the last open).
   * Safe to call anytime; serialised with PortAudio init/terminate.
   */
  static std::vector<DeviceInfo> enumerateDevices();

  /** Host API the engine prefers ("Windows WASAPI" on Windows, else PA default). */
  static std::string preferredHostApi();

  /** Test hook: make the watchdog report a device loss (exercises restart). */
  void debugForceDeviceLoss() { forceDeviceLoss_.store(true, std::memory_order_relaxed); }

  /** Name/host API/id of the opened devices (empty when not running). */
  struct OpenedDevice {
    std::string id, name, hostApi;
  };
  bool openedDevices(OpenedDevice& in, OpenedDevice& out) const;

  /**
   * Start the audio engine with given configuration.
   * Opens PortAudio streams and launches the processing thread.
   * Returns empty string on success, or an error message.
   */
  std::string start(const AudioConfig& config);

  /** Stop the audio engine. Blocks until processing thread exits. */
  void stop();

  /** Check if the engine is currently running. */
  bool isRunning() const { return running_.load(std::memory_order_acquire); }

  /** Set noise suppression level [0.0, 1.0]. Thread-safe. */
  void setSuppressionLevel(float level);

  /** Get current noise suppression level. */
  float getSuppressionLevel() const;

  /** Set status callback for device events. */
  void setStatusCallback(StatusCallback cb);

  /** Set VAD gate threshold [0..1]. Higher = more aggressive gating. */
  void setVadThreshold(float threshold);
  float getVadThreshold() const;

  /** Speech AGC on/off (off = fixed +2 dB). Thread-safe. */
  void setAgcEnabled(bool enabled) { rnnoise_.setAgcEnabled(enabled); }
  bool isAgcEnabled() const { return rnnoise_.isAgcEnabled(); }

  /** Access real-time metrics from the RNNoise wrapper (lock-free). */
  const AudioMetrics& metrics() const { return rnnoise_.metrics(); }

  /** Live engine counters: xruns, drops, ring fill, timing (lock-free). */
  const EngineStats& stats() const { return stats_; }

  /** Streams as actually opened. Valid after a successful start(). */
  StreamSnapshot streamSnapshot() const;

  /** Drift compensation state (lock-free reads). */
  DriftSnapshot driftSnapshot() const;

  /** Current ring fill levels in samples (lock-free reads). */
  size_t captureFill() const;
  size_t outputFill() const;

  /** Algorithmic delay of the suppression backend (RNNoise: one 10 ms frame). */
  static constexpr double kBackendDelayMs = 10.0;

 private:
  /**
   * PortAudio capture callback (static C function).
   * REAL-TIME SAFE: Only writes to captureRing_. No allocations/locks.
   */
  static int captureCallback(const void* input, void* output,
                             unsigned long frameCount,
                             const PaStreamCallbackTimeInfo* timeInfo,
                             PaStreamCallbackFlags statusFlags,
                             void* userData);

  /**
   * PortAudio output callback (static C function).
   * REAL-TIME SAFE: Only reads from outputRing_. Outputs silence if underrun.
   */
  static int outputCallback(const void* input, void* output,
                            unsigned long frameCount,
                            const PaStreamCallbackTimeInfo* timeInfo,
                            PaStreamCallbackFlags statusFlags,
                            void* userData);

  /** Processing thread entry point. Reads capture -> RNNoise -> output ring. */
  void processingLoop();

  /** Attempt to restart audio after a device disconnect. */
  void attemptRestart();

  /**
   * Device-loss watchdog (processing thread). Returns true if a stream has
   * stopped or the capture callback has stalled -- i.e. a real failure, as
   * opposed to an xrun, which is only counted.
   */
  bool deviceLost();

  /** Re-arm the watchdog after a restart attempt. */
  void resetWatchdog();

  /** Processing thread: write a processed frame to the output ring (with drift compensation). */
  void writeOutput(const float* frame, size_t n);

  /** Reset drift state (start, and after a restart: a new device is a new clock). */
  void resetDrift();

  /** Record resolved devices, exclusive-mode result and PA latencies. */
  void captureStreamSnapshot(int inputIdx, int outputIdx, bool inExcl, bool outExcl);

  /** Open PortAudio streams with current config_. */
  std::string openStreams();

  /** Close PortAudio streams. */
  void closeStreams();

  /* State */
  std::atomic<bool> running_{false};
  AudioConfig config_;
  StatusCallback statusCallback_;

  /* PortAudio streams */
  PaStream* captureStream_ = nullptr;
  PaStream* outputStream_ = nullptr;

  /* Lock-free ring buffers (allocated once in start(), not in callbacks) */
  std::unique_ptr<RingBuffer> captureRing_;
  std::unique_ptr<RingBuffer> outputRing_;

  /* RNNoise processor */
  RNNoiseWrapper rnnoise_;

  /* Drift compensation (resampler/controller used by the processing thread). */
  DriftResampler driftRs_;
  DriftController driftCtl_;
  std::atomic<int64_t> lastOutputReadNs_{0};    /* output cb: time of last read */
  std::atomic<uint32_t> lastOutputReadCount_{0}; /* output cb: samples requested */
  std::atomic<double> driftPpm_{0.0};
  std::atomic<double> driftLevel_{0.0};
  std::atomic<bool> driftActive_{false};

  /* Capture callback -> processing thread wake-up (replaces sleep polling). */
  WakeEvent wake_;
  std::atomic<bool> forceDeviceLoss_{false};

  /* Opened devices, by identity (written in openStreams under the PA mutex). */
  OpenedDevice openedIn_, openedOut_;
  std::atomic<bool> mmcssActive_{false};

  /* Live counters + resolved stream info. */
  EngineStats stats_;
  std::atomic<bool> outputPrimed_{false};  /* output ring has delivered audio once */
  std::atomic<int> snapInputDevice_{-1};
  std::atomic<int> snapOutputDevice_{-1};
  std::atomic<bool> snapInputExclusive_{false};
  std::atomic<bool> snapOutputExclusive_{false};
  std::atomic<double> snapInputLatencyMs_{0.0};
  std::atomic<double> snapOutputLatencyMs_{0.0};

  /* Restart state (processing thread only). */
  bool inOutage_ = false;

  /* Watchdog state (processing thread only). */
  uint64_t wdLastCaptureCallbacks_ = 0;
  int64_t wdLastProgressMs_ = 0;
  int64_t wdLastCheckMs_ = 0;

  /* Processing thread */
  std::thread processingThread_;
};

}  // namespace ainoiceguard

#endif  // AINOICEGUARD_AUDIO_H
