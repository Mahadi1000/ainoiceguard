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
struct DeviceInfo {
  int index;
  std::string name;
  int maxInputChannels;
  int maxOutputChannels;
  double defaultSampleRate;
};

/** Configuration for the audio engine. */
struct AudioConfig {
  int inputDeviceIndex = -1;   /* -1 = default input */
  int outputDeviceIndex = -1;  /* -1 = default output, -2 = disable output (mute) */
  double sampleRate = 48000.0;
  unsigned long framesPerBuffer = 480;  /* 10ms @ 48kHz = RNNoise frame size */
  bool tryExclusiveMode = true;
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
 * Callback for engine status changes (e.g., device disconnected, restarted).
 * Called from the processing thread -- keep it lightweight.
 */
using StatusCallback = std::function<void(const std::string& status)>;

class AudioEngine {
 public:
  AudioEngine();
  ~AudioEngine();

  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  /** Enumerate all available audio devices. Safe to call anytime. */
  static std::vector<DeviceInfo> enumerateDevices();

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

  /* Capture callback -> processing thread wake-up (replaces sleep polling). */
  WakeEvent wake_;
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

  /* Watchdog state (processing thread only). */
  uint64_t wdLastCaptureCallbacks_ = 0;
  int64_t wdLastProgressMs_ = 0;
  int64_t wdLastCheckMs_ = 0;

  /* Processing thread */
  std::thread processingThread_;
};

}  // namespace ainoiceguard

#endif  // AINOICEGUARD_AUDIO_H
