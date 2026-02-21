/**
 * Production-grade RNNoise wrapper with multi-stage post-processing.
 *
 * Processing chain (per 10ms frame):
 *   RNNoise (×2 passes) → HPF 80Hz → LPF 8kHz → Adaptive Noise Gate
 *   → Spectral Floor Clamp → Soft Silence Injection
 *
 * Design goals:
 *   - Keyboard / fan / environmental noise: gated to true silence.
 *   - Speech: passes through with minimal coloring.
 *   - Transitions: smooth (hold timer + asymmetric gain smoothing).
 *   - Silence periods: shaped comfort noise at -60 dBFS (not dead air).
 *   - No allocations, no locks, no syscalls in the processing path.
 */

#include "rnnoise_wrapper.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "rnnoise.h"

namespace ainoiceguard {

/* ═══════════════════════════════════════════════════════════════════════════
 *  TUNING CONSTANTS
 *
 *  All values are tuned for 10ms frames (480 samples @ 48kHz).
 *  To adjust behavior, modify these constants and rebuild.
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Gate CLOSE coefficient (attack in compressor terms).
 * 0.30 → gate closes over ~3-4 frames (~35ms).
 * Moderate close speed avoids cutting trailing speech.
 */
static constexpr float kGateCloseCoeff = 0.30f;

/*
 * Gate OPEN coefficient (release in compressor terms).
 * 0.25 → gate opens over ~3-4 frames (~35ms).
 * Fast enough that the first syllable isn't noticeably clipped.
 */
static constexpr float kGateOpenCoeff = 0.25f;

/*
 * Minimum gate gain (~-66 dBFS).
 * Very low to avoid audible buzz from residual mic/electrical noise;
 * still non-zero to prevent hard clicks and "no audio" detection.
 */
static constexpr float kMinGateGain = 0.0005f;

/*
 * HOLD TIME: frames to keep the gate open after the last speech frame.
 * 15 frames × 10ms = 150ms.
 * Catches trailing consonants, breaths, and short inter-word pauses.
 * Prevents gate "chattering" on natural speech rhythm.
 */
static constexpr int kHoldFrames = 15;

/*
 * VAD hysteresis band.
 * Gate opens at vadThreshold, closes at (vadThreshold - kVadHysteresis).
 * 0.12 gives a comfortable margin against VAD flicker.
 */
static constexpr float kVadHysteresis = 0.12f;

/* ── Adaptive Noise Floor ────────────────────────────────────────────────── */

/*
 * Calibration period: 200 frames = 2 seconds.
 * During startup, the noise floor is learned quickly.
 * After calibration, tracking continues at a much slower rate so the
 * gate adapts to gradual room-noise changes (fan on/off, etc.).
 */
static constexpr uint64_t kCalibrationPeriod = 200;

/* Fast EMA alpha during calibration. */
static constexpr float kCalibrationAlpha = 0.08f;

/* Slow EMA alpha after calibration. */
static constexpr float kTrackingAlpha = 0.005f;

/*
 * Gate threshold = noiseFloor × kFloorMultiplier.
 * Signals below this (AND low VAD) get gated out.
 * 1.3 = gate closes when signal is only 30% above the noise floor.
 * Increase to 1.5-2.0 for more aggressive silencing; decrease to 1.1 for
 * more sensitivity (at the cost of occasionally letting noise through).
 */
static constexpr float kFloorMultiplier = 1.3f;

/*
 * Absolute minimum noise floor (~-70 dBFS).
 * Prevents the floor from collapsing to zero in a perfectly silent room,
 * which would make the gate hyper-sensitive to any tiny signal.
 */
static constexpr float kAbsoluteMinFloor = 0.0003f;

/*
 * Fallback gate threshold when the floor hasn't been calibrated yet.
 * ~-54 dBFS. Used during the first few frames before the EMA stabilizes.
 */
static constexpr float kFallbackThreshold = 0.002f;

/* ── Spectral Floor Clamp ────────────────────────────────────────────────── */

/*
 * Clamp multiplier: samples below (noiseFloor × kSpectralClampMult)
 * are forced to exact zero. Applied ONLY when VAD is low and the gate
 * is fully closed, so speech harmonics are never touched.
 */
static constexpr float kSpectralClampMult = 1.5f;

/*
 * Gate gain threshold for applying the spectral clamp.
 * Clamp is active only when smoothGain_ < this value.
 * 0.05 = only clamp when the gate is nearly fully closed.
 */
static constexpr float kClampGateThreshold = 0.05f;

/* ── Soft Silence (Comfort Noise) ────────────────────────────────────────── */

/*
 * Comfort noise amplitude: -70 dBFS = 0.0003.
 * Very low to avoid audible buzz/hiss; still prevents dead silence in headphones.
 */
static constexpr float kSoftSilenceLevel = 0.0003f;

/*
 * 1-pole lowpass shaping coefficient for comfort noise.
 * 0.92 → strong lowpass so noise is sub-bass rumble, not mid/high buzz or hiss.
 */
static constexpr float kNoiseShapeCoeff = 0.92f;

/*
 * Gate gain below which soft silence is injected.
 * 0.1 = inject only when the gate is nearly or fully closed.
 */
static constexpr float kSoftSilenceGateThresh = 0.1f;

/* ═══════════════════════════════════════════════════════════════════════════
 *  LIFECYCLE
 * ═══════════════════════════════════════════════════════════════════════════ */

RNNoiseWrapper::RNNoiseWrapper() = default;

RNNoiseWrapper::~RNNoiseWrapper() { destroy(); }

bool RNNoiseWrapper::init() {
  if (state_) destroy();

  state_  = rnnoise_create(nullptr);
  state2_ = rnnoise_create(nullptr);

  smoothGain_ = 1.0f;
  holdCounter_ = 0;
  noiseFloorEstimate_ = 0.0f;
  calibrationFrames_ = 0;
  noiseState_ = 0x12345678;
  prevNoise_ = 0.0f;

  initFilters();

  metrics_.framesProcessed.store(0, std::memory_order_relaxed);
  metrics_.inputRms.store(0.0f, std::memory_order_relaxed);
  metrics_.outputRms.store(0.0f, std::memory_order_relaxed);
  metrics_.vadProbability.store(0.0f, std::memory_order_relaxed);
  metrics_.currentGain.store(1.0f, std::memory_order_relaxed);
  metrics_.noiseFloor.store(0.0f, std::memory_order_relaxed);

  return state_ != nullptr && state2_ != nullptr;
}

void RNNoiseWrapper::destroy() {
  if (state_)  { rnnoise_destroy(state_);  state_  = nullptr; }
  if (state2_) { rnnoise_destroy(state2_); state2_ = nullptr; }
}

/*
 * Set biquad coefficients for 48 kHz sample rate.
 * Computed offline using the Audio EQ Cookbook (Robert Bristow-Johnson)
 * with Butterworth Q = 1/sqrt(2) ≈ 0.7071.
 */
void RNNoiseWrapper::initFilters() {
  /*
   * HIGH-PASS at 80 Hz (2nd order Butterworth).
   * Removes: DC offset, mains hum (50/60 Hz), low-frequency rumble,
   *          handling noise, HVAC vibration.
   *
   *   w0    = 2π × 80 / 48000 = 0.01047
   *   alpha = sin(w0) / (2 × Q) = 0.00741
   */
  hpf_.b0 =  0.992631f;
  hpf_.b1 = -1.985261f;
  hpf_.b2 =  0.992631f;
  hpf_.a1 = -1.985199f;
  hpf_.a2 =  0.985323f;
  hpf_.reset();

  /*
   * LOW-PASS at 8000 Hz (2nd order Butterworth).
   * Removes: high-frequency residual hiss that RNNoise misses,
   *          aliasing artifacts, and electrical noise above speech band.
   * Speech fundamental + formants are below ~4 kHz; sibilants (s, sh, t)
   * peak around 4-8 kHz. 8 kHz preserves sibilant clarity while
   * cutting HF noise.
   *
   *   w0    = 2π × 8000 / 48000 = π/3
   *   alpha = sin(w0) / (2 × Q) = 0.6124
   */
  lpf_.b0 = 0.155029f;
  lpf_.b1 = 0.310059f;
  lpf_.b2 = 0.155029f;
  lpf_.a1 = -0.620209f;
  lpf_.a2 =  0.240326f;
  lpf_.reset();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  CORE PROCESSING PIPELINE
 * ═══════════════════════════════════════════════════════════════════════════ */

float RNNoiseWrapper::processFrame(float* frame) {
  if (!state_ || !state2_) return 0.0f;

  float level = suppressionLevel_.load(std::memory_order_relaxed);

  /* Fast path: suppression fully off → passthrough. */
  if (level <= 0.0f) {
    float rms = computeRms(frame, kRNNoiseFrameSize);
    metrics_.inputRms.store(rms, std::memory_order_relaxed);
    metrics_.outputRms.store(rms, std::memory_order_relaxed);
    metrics_.vadProbability.store(0.0f, std::memory_order_relaxed);
    metrics_.currentGain.store(1.0f, std::memory_order_relaxed);
    metrics_.framesProcessed.fetch_add(1, std::memory_order_relaxed);
    return 0.0f;
  }

  /* ── 1. Measure input RMS (raw mic level) ── */
  float inputRms = computeRms(frame, kRNNoiseFrameSize);
  metrics_.inputRms.store(inputRms, std::memory_order_relaxed);

  /* ── 2. Save original for blending at partial suppression ── */
  float original[kRNNoiseFrameSize];
  for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
    original[i] = frame[i];
    frame[i] *= 32767.0f;   /* RNNoise expects int16 range. */
  }

  /* ── 3. Double-pass RNNoise ── */
  float vad1 = rnnoise_process_frame(state_,  frame, frame);
  float vad2 = rnnoise_process_frame(state2_, frame, frame);
  float vad = std::max(vad1, vad2);
  metrics_.vadProbability.store(vad, std::memory_order_relaxed);

  /* Convert back to [-1.0, 1.0] float range. */
  constexpr float kInvScale = 1.0f / 32767.0f;
  for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
    frame[i] *= kInvScale;
  }

  /* ── 4. Blend with original based on suppression level ── */
  if (level < 1.0f) {
    float dry = 1.0f - level;
    for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
      frame[i] = frame[i] * level + original[i] * dry;
    }
  }

  /* ── 5. Biquad filters: HPF (80 Hz) then LPF (8 kHz) ── */
  for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
    frame[i] = hpf_.process(frame[i]);
    frame[i] = lpf_.process(frame[i]);
  }

  /* ── 6. Post-filter RMS (used for adaptive gate threshold) ── */
  float postRms = computeRms(frame, kRNNoiseFrameSize);

  /* ── 7. Update adaptive noise floor ── */
  updateNoiseFloor(postRms, vad);

  /* ── 8. Gate decision + hold timer ── */
  float targetGain = computeGateTarget(vad, postRms);

  /* ── 9. Asymmetric gain smoothing (fast close, slow open) ── */
  float coeff = (targetGain < smoothGain_) ? kGateCloseCoeff : kGateOpenCoeff;
  smoothGain_ += coeff * (targetGain - smoothGain_);
  smoothGain_ = std::clamp(smoothGain_, kMinGateGain, 1.0f);
  metrics_.currentGain.store(smoothGain_, std::memory_order_relaxed);

  /* ── 10. Apply gate gain ── */
  for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
    frame[i] *= smoothGain_;
  }

  /* ── 11. Spectral floor clamp (when VAD low + gate closing) ── */
  spectralClamp(frame, vad);

  /* ── 12. Soft silence (inject comfort noise when gate closed) ── */
  applySoftSilence(frame);

  /* ── 13. Output RMS + metrics ── */
  float outputRms = computeRms(frame, kRNNoiseFrameSize);
  metrics_.outputRms.store(outputRms, std::memory_order_relaxed);
  metrics_.framesProcessed.fetch_add(1, std::memory_order_relaxed);

  return vad;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  ADAPTIVE NOISE FLOOR
 *
 *  Learns the baseline noise level from non-speech frames using an
 *  exponential moving average (EMA). During the first ~2 seconds the
 *  learning rate is fast; afterwards it tracks slowly to adapt to
 *  gradual environmental changes (fan turning on/off, etc.).
 * ═══════════════════════════════════════════════════════════════════════════ */

void RNNoiseWrapper::updateNoiseFloor(float postRms, float vad) {
  float vadThresh = vadThreshold_.load(std::memory_order_relaxed);

  /*
   * Only learn from frames that are very likely pure noise.
   * Use half the user's VAD threshold to be conservative: we don't
   * want speech leaking into the floor estimate.
   */
  bool isNoise = (vad < vadThresh * 0.5f);

  if (!isNoise) {
    metrics_.noiseFloor.store(noiseFloorEstimate_, std::memory_order_relaxed);
    return;
  }

  float alpha;
  if (calibrationFrames_ < kCalibrationPeriod) {
    alpha = kCalibrationAlpha;
    calibrationFrames_++;
  } else {
    alpha = kTrackingAlpha;
  }

  if (noiseFloorEstimate_ <= 0.0f) {
    noiseFloorEstimate_ = postRms;
  } else {
    noiseFloorEstimate_ += alpha * (postRms - noiseFloorEstimate_);
  }

  noiseFloorEstimate_ = std::max(noiseFloorEstimate_, kAbsoluteMinFloor);
  metrics_.noiseFloor.store(noiseFloorEstimate_, std::memory_order_relaxed);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  GATE STATE MACHINE
 *
 *  Decision logic combining RNNoise VAD and adaptive energy threshold:
 *    Speech detected → hold counter reset, gate opens.
 *    Hold period     → gate stays open to catch trailing sounds.
 *    Silence         → gate closes (target gain = 0).
 *
 *  Speech is detected when:
 *    (a) VAD >= threshold, OR
 *    (b) VAD is in the hysteresis band AND energy is well above the
 *        learned noise floor (catches breathy/quiet speech).
 * ═══════════════════════════════════════════════════════════════════════════ */

float RNNoiseWrapper::computeGateTarget(float vad, float postRms) {
  /*
   * During calibration the noise floor is still being learned.
   * Keep the gate fully open so the user hears audio immediately
   * and the floor converges on actual ambient noise, not silence.
   */
  if (calibrationFrames_ < kCalibrationPeriod) {
    holdCounter_ = kHoldFrames;
    return 1.0f;
  }

  float vadThresh = vadThreshold_.load(std::memory_order_relaxed);

  float gateThresh = (noiseFloorEstimate_ > kAbsoluteMinFloor)
      ? noiseFloorEstimate_ * kFloorMultiplier
      : kFallbackThreshold;

  /* Condition (a): strong VAD confidence. */
  bool speechByVad = (vad >= vadThresh);

  /*
   * Condition (b): moderate VAD + energy clearly above noise floor.
   * This catches quiet or breathy speech that has a VAD just below
   * the threshold but is obviously not noise based on energy.
   */
  bool speechByEnergy = (vad >= vadThresh - kVadHysteresis)
                     && (postRms > gateThresh * 1.5f);

  if (speechByVad || speechByEnergy) {
    holdCounter_ = kHoldFrames;
    return 1.0f;
  }

  if (holdCounter_ > 0) {
    holdCounter_--;
    return 1.0f;
  }

  /*
   * No speech, hold expired. Close the gate.
   * If energy is well below the threshold: close to minimum.
   * If energy is near the threshold: partial gain for smooth transition.
   */
  if (postRms < gateThresh) {
    return kMinGateGain;
  }

  float ratio = (postRms - gateThresh) / std::max(gateThresh, 0.0001f);
  return std::clamp(ratio, kMinGateGain, 0.5f);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SPECTRAL FLOOR CLAMP
 *
 *  After the gate has been applied, any sample whose magnitude is below
 *  an adaptive threshold is forced to exact zero. This eliminates the
 *  faint hiss / buzz that survives RNNoise + gating.
 *
 *  Only active when VAD is low and the gate is mostly closed, so it
 *  never touches speech harmonics.
 * ═══════════════════════════════════════════════════════════════════════════ */

void RNNoiseWrapper::spectralClamp(float* frame, float vad) {
  /* Never clamp during calibration -- floor is unreliable. */
  if (calibrationFrames_ < kCalibrationPeriod) return;

  float vadThresh = vadThreshold_.load(std::memory_order_relaxed);

  if (vad >= vadThresh || smoothGain_ > kClampGateThreshold) return;

  float clampThresh = std::max(
      noiseFloorEstimate_ * kSpectralClampMult,
      kAbsoluteMinFloor * 2.0f
  );

  for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
    if (std::abs(frame[i]) < clampThresh) {
      frame[i] = 0.0f;
    }
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SOFT SILENCE
 *
 *  When the gate is closed, inject very low-level shaped noise instead
 *  of pure digital zero. This prevents:
 *    - The "dead channel" sensation in headphones.
 *    - Click artifacts from sudden zero-to-signal transitions.
 *    - Some conferencing apps detecting "no audio" and muting the channel.
 * ═══════════════════════════════════════════════════════════════════════════ */

void RNNoiseWrapper::applySoftSilence(float* frame) {
  if (!comfortNoiseEnabled_.load(std::memory_order_relaxed)) return;
  if (smoothGain_ >= kSoftSilenceGateThresh) return;

  /* Scale comfort noise proportionally: more as gate approaches zero. */
  float scale = (kSoftSilenceGateThresh - smoothGain_) / kSoftSilenceGateThresh;

  for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
    frame[i] += comfortNoiseSample() * scale;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SETTINGS (lock-free, called from any thread)
 * ═══════════════════════════════════════════════════════════════════════════ */

void RNNoiseWrapper::setSuppressionLevel(float level) {
  suppressionLevel_.store(std::clamp(level, 0.0f, 1.0f),
                          std::memory_order_relaxed);
}

float RNNoiseWrapper::getSuppressionLevel() const {
  return suppressionLevel_.load(std::memory_order_relaxed);
}

void RNNoiseWrapper::setVadThreshold(float threshold) {
  vadThreshold_.store(std::clamp(threshold, 0.0f, 1.0f),
                      std::memory_order_relaxed);
}

float RNNoiseWrapper::getVadThreshold() const {
  return vadThreshold_.load(std::memory_order_relaxed);
}

void RNNoiseWrapper::setComfortNoise(bool enabled) {
  comfortNoiseEnabled_.store(enabled, std::memory_order_relaxed);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  HELPERS
 * ═══════════════════════════════════════════════════════════════════════════ */

float RNNoiseWrapper::computeRms(const float* buf, size_t len) {
  float sum = 0.0f;
  for (size_t i = 0; i < len; i++) {
    sum += buf[i] * buf[i];
  }
  return std::sqrt(sum / static_cast<float>(len));
}

/**
 * LFSR-based comfort noise with 1-pole lowpass shaping.
 * Strong lowpass (kNoiseShapeCoeff) keeps energy in sub-bass to avoid
 * audible buzz/hiss. Final amplitude is kSoftSilenceLevel (~-70 dBFS).
 */
float RNNoiseWrapper::comfortNoiseSample() {
  noiseState_ ^= noiseState_ << 13;
  noiseState_ ^= noiseState_ >> 17;
  noiseState_ ^= noiseState_ << 5;

  float white = static_cast<float>(static_cast<int32_t>(noiseState_)) /
                2147483648.0f;

  float shaped = kNoiseShapeCoeff * prevNoise_
               + (1.0f - kNoiseShapeCoeff) * white;
  prevNoise_ = shaped;

  return shaped * kSoftSilenceLevel;
}

}  // namespace ainoiceguard
