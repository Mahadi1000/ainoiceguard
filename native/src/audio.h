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

#ifndef NOISEGUARD_AUDIO_H
#define NOISEGUARD_AUDIO_H

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ringbuffer.h"
#include "rnnoise_wrapper.h"

/* Forward-declare PortAudio types to avoid including portaudio.h in this header. */
typedef void PaStream;

namespace noiseguard {

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
  int outputDeviceIndex = -1;  /* -1 = default output */
  double sampleRate = 48000.0;
  unsigned long framesPerBuffer = 480;  /* 10ms @ 48kHz = RNNoise frame size */
  bool tryExclusiveMode = true;
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

 private:
  /**
   * PortAudio capture callback (static C function).
   * REAL-TIME SAFE: Only writes to captureRing_. No allocations/locks.
   */
  static int captureCallback(const void* input, void* output,
                             unsigned long frameCount,
                             const void* timeInfo,
                             unsigned long statusFlags,
                             void* userData);

  /**
   * PortAudio output callback (static C function).
   * REAL-TIME SAFE: Only reads from outputRing_. Outputs silence if underrun.
   */
  static int outputCallback(const void* input, void* output,
                            unsigned long frameCount,
                            const void* timeInfo,
                            unsigned long statusFlags,
                            void* userData);

  /** Processing thread entry point. Reads capture -> RNNoise -> output ring. */
  void processingLoop();

  /** Attempt to restart audio after a device disconnect. */
  void attemptRestart();

  /** Open PortAudio streams with current config_. */
  std::string openStreams();

  /** Close PortAudio streams. */
  void closeStreams();

  /* State */
  std::atomic<bool> running_{false};
  std::atomic<bool> shouldRestart_{false};
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

  /* Processing thread */
  std::thread processingThread_;
};

}  // namespace noiseguard

#endif  // NOISEGUARD_AUDIO_H
