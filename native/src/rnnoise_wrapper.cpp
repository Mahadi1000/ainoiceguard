/**
 * RNNoise wrapper implementation.
 *
 * RNNoise expects audio in int16 range [-32768, 32767] as floats.
 * PortAudio gives us float32 in [-1.0, 1.0].
 * We convert on the fly -- no extra buffer allocation needed.
 */

#include "rnnoise_wrapper.h"

#include <algorithm>
#include <cmath>
#include <cstring>

/* RNNoise public C API. */
#include "rnnoise.h"

namespace noiseguard {

RNNoiseWrapper::RNNoiseWrapper() = default;

RNNoiseWrapper::~RNNoiseWrapper() { destroy(); }

bool RNNoiseWrapper::init() {
  if (state_) destroy();
  state_ = rnnoise_create(nullptr);
  return state_ != nullptr;
}

void RNNoiseWrapper::destroy() {
  if (state_) {
    rnnoise_destroy(state_);
    state_ = nullptr;
  }
}

float RNNoiseWrapper::processFrame(float* frame) {
  if (!state_) return 0.0f;

  float level = suppression_level_.load(std::memory_order_relaxed);

  /*
   * Fast path: if suppression is fully off, skip processing entirely.
   * This avoids burning CPU when the user has toggled noise cancellation off.
   */
  if (level <= 0.0f) return 0.0f;

  /*
   * RNNoise expects float samples in int16 range [-32768, 32767].
   * Convert from PortAudio's [-1.0, 1.0] normalized range.
   * We do this in-place to avoid allocating a temp buffer.
   *
   * REAL-TIME SAFETY: No allocation, pure arithmetic, fixed loop count.
   */
  float original[kRNNoiseFrameSize];

  for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
    original[i] = frame[i];
    frame[i] *= 32767.0f;
  }

  /* Process frame in-place. Returns VAD probability [0.0, 1.0]. */
  float vad = rnnoise_process_frame(state_, frame, frame);

  /* Convert back to [-1.0, 1.0] range. */
  constexpr float kInvScale = 1.0f / 32767.0f;
  for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
    frame[i] *= kInvScale;
  }

  /*
   * Blend between original and denoised based on suppression level.
   * level = 1.0 -> fully denoised (frame stays as-is after RNNoise).
   * level = 0.5 -> 50/50 blend.
   * level = 0.0 -> original (handled by fast path above).
   */
  if (level < 1.0f) {
    float dry = 1.0f - level;
    for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
      frame[i] = frame[i] * level + original[i] * dry;
    }
  }

  return vad;
}

void RNNoiseWrapper::setSuppressionLevel(float level) {
  suppression_level_.store(std::clamp(level, 0.0f, 1.0f),
                           std::memory_order_relaxed);
}

float RNNoiseWrapper::getSuppressionLevel() const {
  return suppression_level_.load(std::memory_order_relaxed);
}

}  // namespace noiseguard
