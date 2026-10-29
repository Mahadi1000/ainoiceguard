/**
 * RNNoise wrapper — noise suppression pipeline.
 *
 * Processing chain per 10ms frame (480 samples @ 48kHz):
 *   1. HPF 80Hz  — removes mains hum, DC offset, handling rumble
 *   2. RNNoise   — single-pass neural noise suppression (VAD + spectral)
 *   3. Blend     — dry/wet mix for partial suppression levels
 *   4. Gain      — speech AGC (default; VAD-gated, target -18 dBFS) or the
 *                  former fixed +2 dB makeup gain when AGC is disabled
 *   5. Soft gate — VAD-proportional gain with hold timer and energy backup
 *   6. Comfort noise — sub-bass shaped noise during closed gate
 *   7. Limiter   — zero-latency peak limiter, -1 dBFS ceiling (replaces the
 *                  former hard clamp at +/-1.0, which clipped)
 *   8. Metrics   — lock-free atomic RMS / VAD / gain reporting to UI
 *
 * Why single pass:
 *   Running RNNoise twice causes robotic metallic artifacts. The second pass
 *   processes already-denoised audio, over-suppressing harmonics that are part
 *   of the voice. Single pass is the intended and correct usage.
 *
 * Why no LPF:
 *   A hard low-pass at 8kHz removes the sibilant frequencies (s, sh, f, th)
 *   that live at 8-16kHz, making every voice sound like a low-bitrate call.
 *   RNNoise handles HF noise internally via its 22-band spectral model.
 *
 * REAL-TIME RULES:
 *   processFrame() does NO allocations — pure arithmetic, fixed-size loops.
 *   setSuppressionLevel() and setVadThreshold() are lock-free (atomic store).
 */

#ifndef AINOICEGUARD_RNNOISE_WRAPPER_H
#define AINOICEGUARD_RNNOISE_WRAPPER_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "dynamics.h"

struct DenoiseState;

namespace ainoiceguard {

static constexpr size_t kRNNoiseFrameSize = 480;

struct AudioMetrics {
  std::atomic<float>    inputRms{0.0f};
  std::atomic<float>    outputRms{0.0f};
  std::atomic<float>    vadProbability{0.0f};
  std::atomic<float>    currentGain{1.0f};
  std::atomic<float>    noiseFloor{0.0f};
  std::atomic<uint64_t> framesProcessed{0};
  std::atomic<float>    agcGainDb{0.0f};         /* current AGC (or fixed makeup) gain */
  std::atomic<float>    limiterReductionDb{0.0f}; /* deepest limiting in the last frame (<= 0) */
  std::atomic<uint64_t> limitedFrames{0};        /* frames where the limiter engaged */
};

struct BiquadState {
  float b0 = 1.f, b1 = 0.f, b2 = 0.f;
  float a1 = 0.f, a2 = 0.f;
  float x1 = 0.f, x2 = 0.f;
  float y1 = 0.f, y2 = 0.f;

  void reset() { x1 = x2 = y1 = y2 = 0.f; }

  inline float process(float x) {
    float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = x;
    y2 = y1; y1 = y;
    return y;
  }
};

class RNNoiseWrapper {
 public:
  RNNoiseWrapper();
  ~RNNoiseWrapper();

  RNNoiseWrapper(const RNNoiseWrapper&) = delete;
  RNNoiseWrapper& operator=(const RNNoiseWrapper&) = delete;

  bool  init();
  void  destroy();

  float processFrame(float* frame);

  void  setSuppressionLevel(float level);
  float getSuppressionLevel() const;

  void  setVadThreshold(float threshold);
  float getVadThreshold() const;

  void  setComfortNoise(bool enabled);

  /** Speech AGC on (default) or fixed +2 dB makeup gain. Lock-free. */
  void  setAgcEnabled(bool enabled);
  bool  isAgcEnabled() const;

  bool  isInitialized() const { return state_ != nullptr; }

  const AudioMetrics& metrics() const { return metrics_; }

 private:
  DenoiseState* state_ = nullptr;

  std::atomic<float> suppressionLevel_{1.0f};
  std::atomic<float> vadThreshold_{0.50f};   /* lowered from 0.65: catches breathy speech */
  std::atomic<bool>  comfortNoiseEnabled_{true};
  std::atomic<bool>  agcEnabled_{true};

  SpeechAgc   agc_;
  PeakLimiter limiter_;

  float    smoothGain_         = 1.0f;
  int      holdCounter_        = 0;
  float    noiseFloorEstimate_ = 0.0f;
  uint64_t calibrationFrames_  = 0;

  BiquadState hpf_;   /* High-pass at 80 Hz — only filter needed */

  uint32_t noiseState_ = 0x12345678;
  float    prevNoise_  = 0.0f;

  AudioMetrics metrics_;

  void  initFilters();
  void  updateNoiseFloor(float postRms, float vad);
  float computeGateTarget(float vad, float postRms);
  void  applySoftSilence(float* frame);
  float comfortNoiseSample();

  static float computeRms(const float* buf, size_t len);
};

}  // namespace ainoiceguard

#endif  // AINOICEGUARD_RNNOISE_WRAPPER_H
