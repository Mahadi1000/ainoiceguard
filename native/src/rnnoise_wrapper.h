/**
 * RNNoise wrapper for real-time noise suppression.
 *
 * RNNoise processes exactly 480 float samples per frame (10ms @ 48kHz).
 * This wrapper manages the DenoiseState lifecycle and provides a thread-safe
 * noise suppression level control via atomic<float>.
 *
 * REAL-TIME RULES:
 * - processFrame() does NO allocations -- RNNoise itself is allocation-free per frame.
 * - setSuppressionLevel() is lock-free (atomic store).
 * - init() and destroy() are NOT real-time safe -- call them outside audio callbacks.
 */

#ifndef NOISEGUARD_RNNOISE_WRAPPER_H
#define NOISEGUARD_RNNOISE_WRAPPER_H

#include <atomic>
#include <cstddef>

/* Forward-declare RNNoise opaque type to avoid including rnnoise.h in the header. */
struct DenoiseState;

namespace noiseguard {

/* RNNoise operates on exactly 480 samples per frame (10ms at 48kHz). */
static constexpr size_t kRNNoiseFrameSize = 480;

class RNNoiseWrapper {
 public:
  RNNoiseWrapper();
  ~RNNoiseWrapper();

  RNNoiseWrapper(const RNNoiseWrapper&) = delete;
  RNNoiseWrapper& operator=(const RNNoiseWrapper&) = delete;

  /** Initialize the RNNoise state. Call before any processing. Returns true on success. */
  bool init();

  /** Destroy the RNNoise state. Call after all processing is complete. */
  void destroy();

  /**
   * Process a single frame IN-PLACE. frame must point to exactly kRNNoiseFrameSize floats.
   *
   * Input: float samples in [-1.0, 1.0] range (PortAudio float32 format).
   * The wrapper handles the conversion to/from RNNoise's [-32768, 32767] internal range.
   *
   * Returns the RNNoise VAD (voice activity) probability [0.0, 1.0].
   * When suppression_level < 1.0, the output is blended between original and denoised.
   */
  float processFrame(float* frame);

  /**
   * Set suppression level [0.0 = bypass, 1.0 = full suppression].
   * Thread-safe: can be called from any thread while processing is active.
   */
  void setSuppressionLevel(float level);

  /** Get current suppression level. */
  float getSuppressionLevel() const;

  /** Check if the state is initialized. */
  bool isInitialized() const { return state_ != nullptr; }

 private:
  DenoiseState* state_ = nullptr;

  /**
   * Suppression level [0.0 .. 1.0].
   * 0.0 = no suppression (passthrough), 1.0 = full suppression.
   * Atomic for lock-free updates from the UI thread.
   */
  std::atomic<float> suppression_level_{1.0f};
};

}  // namespace noiseguard

#endif  // NOISEGUARD_RNNOISE_WRAPPER_H
