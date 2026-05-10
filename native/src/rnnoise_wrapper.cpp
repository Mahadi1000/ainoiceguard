/**
 * RNNoise wrapper - noise suppression pipeline.
 *
 * Processing chain per 10ms frame:
 *   HPF 80Hz → RNNoise (single pass) → VAD-proportional gate → comfort noise
 *
 * Key design decisions:
 *   - Single RNNoise pass: double-pass causes robotic artifacts on voice.
 *   - No LPF: cutting at 8kHz makes voices muffled; RNNoise handles HF noise.
 *   - Soft VAD gate with hold timer: smooth suppression, no pumping.
 *   - Non-speech frames: deeper attenuation. Speech frames: fully unaffected.
 */

#include "rnnoise_wrapper.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "rnnoise.h"

namespace ainoiceguard {

/* ═══════════════════════════════════════════════════════════════════════════
 *  TUNING CONSTANTS  (all for 10ms frames, 480 samples @ 48kHz)
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Gate OPEN speed (release).
 * 0.5 → gate reaches ~97% open in ~6 frames (~60ms).
 * Fast enough that the first syllable is never clipped.
 */
static constexpr float kGateOpenCoeff = 0.50f;

/*
 * Gate CLOSE speed (attack).
 * 0.10 → gate fades over ~25 frames (~250ms) after the hold expires.
 * Slow fade prevents audible "click" when transitioning into silence.
 */
static constexpr float kGateCloseCoeff = 0.10f;

/*
 * Minimum gate gain applied during non-speech.
 * 0.012 ≈ -38 dBFS. Leaves a very faint residual that prevents:
 *   - Harsh "gate chatter" clicks during plosives (p, t, k)
 *   - Conferencing apps misreading "no signal"
 * The residual is far below audible noise level in any real environment.
 */
static constexpr float kMinGateGain = 0.012f;

/*
 * HOLD TIME after last speech frame before gate starts closing.
 * 20 frames × 10ms = 200ms.
 * Catches: trailing consonants, sentence-final breaths, short pauses.
 */
static constexpr int kHoldFrames = 20;

/*
 * VAD thresholds for the soft gate curve.
 * Gate is fully closed below kVadLow, fully open above kVadHigh.
 * Between them: smooth interpolation (avoids hard on/off switching).
 */
static constexpr float kVadLow  = 0.10f;
static constexpr float kVadHigh = 0.50f;

/* ── Adaptive Noise Floor ────────────────────────────────────────────────── */

/* Initial calibration period: 150 frames = 1.5 seconds. */
static constexpr uint64_t kCalibrationPeriod = 150;

/* Fast EMA alpha during calibration. */
static constexpr float kCalibrationAlpha = 0.10f;

/*
 * Slow tracking alpha after calibration.
 * Asymmetric: noise floor rises quickly (fan turns on), falls slowly.
 * Rise: 0.015 → adapts to louder environment in ~3 seconds.
 * Fall: 0.003 → forgets a quiet period only after ~30 seconds.
 * This prevents the gate from becoming too aggressive after a momentary quiet.
 */
static constexpr float kTrackingAlphaRise = 0.015f;
static constexpr float kTrackingAlphaFall = 0.003f;

/*
 * Gate energy threshold multiplier.
 * gateThreshold = noiseFloor × kFloorMultiplier.
 * 2.0 = close gate only when signal is at most 2× the noise floor.
 * Higher → more aggressive silencing. Lower → more noise leaks through.
 */
static constexpr float kFloorMultiplier = 2.0f;

/* Absolute minimum noise floor (~-70 dBFS). */
static constexpr float kAbsoluteMinFloor = 0.0003f;

/* Fallback threshold used during calibration. */
static constexpr float kFallbackThreshold = 0.002f;

/*
 * Makeup gain to compensate for the ~2dB level reduction RNNoise applies.
 * 1.26 ≈ +2dB. Keeps processed voice at roughly the same perceived level.
 */
static constexpr float kMakeupGain = 1.26f;

/* ── Soft Silence (Comfort Noise) ────────────────────────────────────────── */

/* Comfort noise amplitude: ~-70 dBFS. Inaudible, just prevents dead silence. */
static constexpr float kSoftSilenceLevel = 0.0003f;

/* Strong lowpass for comfort noise shaping (keeps it sub-bass, not hiss). */
static constexpr float kNoiseShapeCoeff = 0.95f;

/* Gate gain below which comfort noise is mixed in. */
static constexpr float kSoftSilenceGateThresh = 0.08f;

/* ═══════════════════════════════════════════════════════════════════════════
 *  LIFECYCLE
 * ═══════════════════════════════════════════════════════════════════════════ */

RNNoiseWrapper::RNNoiseWrapper() = default;
RNNoiseWrapper::~RNNoiseWrapper() { destroy(); }

bool RNNoiseWrapper::init() {
  if (state_) destroy();

  state_ = rnnoise_create(nullptr);
  if (!state_) return false;

  smoothGain_          = 1.0f;
  holdCounter_         = 0;
  noiseFloorEstimate_  = 0.0f;
  calibrationFrames_   = 0;
  noiseState_          = 0x12345678;
  prevNoise_           = 0.0f;

  initFilters();

  metrics_.framesProcessed.store(0,    std::memory_order_relaxed);
  metrics_.inputRms.store(0.0f,        std::memory_order_relaxed);
  metrics_.outputRms.store(0.0f,       std::memory_order_relaxed);
  metrics_.vadProbability.store(0.0f,  std::memory_order_relaxed);
  metrics_.currentGain.store(1.0f,     std::memory_order_relaxed);
  metrics_.noiseFloor.store(0.0f,      std::memory_order_relaxed);

  return true;
}

void RNNoiseWrapper::destroy() {
  if (state_) { rnnoise_destroy(state_); state_ = nullptr; }
}

/* Only HPF — no LPF. RNNoise already handles HF noise internally.
 * Adding a LPF at 8kHz makes sibilants (s, sh, f) disappear, which
 * makes every voice sound muffled like a low-bitrate phone call. */
void RNNoiseWrapper::initFilters() {
  /* HIGH-PASS at 80 Hz (2nd-order Butterworth @ 48kHz).
   * Removes: DC offset, 50/60Hz mains hum, handling rumble, HVAC vibration.
   *   w0    = 2π × 80 / 48000 = 0.01047
   *   alpha = sin(w0) / (2 × 0.7071) = 0.00741  */
  hpf_.b0 =  0.992631f;
  hpf_.b1 = -1.985261f;
  hpf_.b2 =  0.992631f;
  hpf_.a1 = -1.985199f;
  hpf_.a2 =  0.985323f;
  hpf_.reset();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  CORE PROCESSING PIPELINE
 * ═══════════════════════════════════════════════════════════════════════════ */

float RNNoiseWrapper::processFrame(float* frame) {
  if (!state_) return 0.0f;

  float level = suppressionLevel_.load(std::memory_order_relaxed);

  /* Fast path: suppression off → measure and passthrough. */
  if (level <= 0.0f) {
    float rms = computeRms(frame, kRNNoiseFrameSize);
    metrics_.inputRms.store(rms,  std::memory_order_relaxed);
    metrics_.outputRms.store(rms, std::memory_order_relaxed);
    metrics_.vadProbability.store(0.0f, std::memory_order_relaxed);
    metrics_.currentGain.store(1.0f,   std::memory_order_relaxed);
    metrics_.framesProcessed.fetch_add(1, std::memory_order_relaxed);
    return 0.0f;
  }

  /* ── 1. Measure input RMS ── */
  float inputRms = computeRms(frame, kRNNoiseFrameSize);
  metrics_.inputRms.store(inputRms, std::memory_order_relaxed);

  /* ── 2. HPF: remove hum / DC before sending to RNNoise ── */
  for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
    frame[i] = hpf_.process(frame[i]);
  }

  /* ── 3. Save filtered copy for dry/wet blend at partial suppression ── */
  float filtered[kRNNoiseFrameSize];
  std::memcpy(filtered, frame, kRNNoiseFrameSize * sizeof(float));

  /* ── 4. RNNoise (single pass) ──
   *  RNNoise expects int16 range (~±32767), returns the same range,
   *  and returns a VAD probability [0.0, 1.0].
   *  Single pass preserves voice naturalness — double-pass causes
   *  robotic metallic artifacts by over-processing already-clean audio. */
  for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
    frame[i] *= 32767.0f;
  }

  float vad = rnnoise_process_frame(state_, frame, frame);
  metrics_.vadProbability.store(vad, std::memory_order_relaxed);

  constexpr float kInvScale = 1.0f / 32767.0f;
  for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
    frame[i] *= kInvScale;
  }

  /* ── 5. Blend with pre-RNNoise (HPF'd) signal for partial suppression ──
   *  level=1.0 → fully denoised; level=0.5 → 50/50 blend.
   *  Using the HPF'd signal (not raw mic) keeps the blend artifact-free. */
  if (level < 1.0f) {
    float dry = 1.0f - level;
    for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
      frame[i] = frame[i] * level + filtered[i] * dry;
    }
  }

  /* ── 6. Makeup gain (+2dB): compensate for RNNoise level reduction ── */
  for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
    frame[i] = std::clamp(frame[i] * kMakeupGain, -1.0f, 1.0f);
  }

  /* ── 7. Measure post-RNNoise RMS for adaptive floor ── */
  float postRms = computeRms(frame, kRNNoiseFrameSize);

  /* ── 8. Update adaptive noise floor ── */
  updateNoiseFloor(postRms, vad);

  /* ── 9. Compute gate target: VAD-proportional soft gate ── */
  float targetGain = computeGateTarget(vad, postRms);

  /* ── 10. Asymmetric smoothing: fast open (speech appears immediately),
   *         slow close (natural fade into silence after hold expires) ── */
  float coeff = (targetGain > smoothGain_) ? kGateOpenCoeff : kGateCloseCoeff;
  smoothGain_ += coeff * (targetGain - smoothGain_);
  smoothGain_ = std::clamp(smoothGain_, kMinGateGain, 1.0f);
  metrics_.currentGain.store(smoothGain_, std::memory_order_relaxed);

  /* ── 11. Apply gate gain ── */
  for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
    frame[i] *= smoothGain_;
  }

  /* ── 12. Comfort noise: very faint shaped noise during closed gate.
   *         Prevents "dead channel" sensation and click on re-open. ── */
  applySoftSilence(frame);

  /* ── 13. Output RMS + frame count ── */
  float outputRms = computeRms(frame, kRNNoiseFrameSize);
  metrics_.outputRms.store(outputRms, std::memory_order_relaxed);
  metrics_.framesProcessed.fetch_add(1, std::memory_order_relaxed);

  return vad;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  ADAPTIVE NOISE FLOOR
 *
 *  Tracks the background noise level using an asymmetric EMA.
 *  The floor rises quickly (new louder noise = adapt fast) and falls
 *  slowly (momentary quiet doesn't make the gate hyper-aggressive).
 *
 *  Only updates from frames where VAD is clearly non-speech (< kVadLow),
 *  so speaking doesn't corrupt the noise estimate.
 * ═══════════════════════════════════════════════════════════════════════════ */

void RNNoiseWrapper::updateNoiseFloor(float postRms, float vad) {
  bool isNoise = (vad < kVadLow);

  if (!isNoise) {
    metrics_.noiseFloor.store(noiseFloorEstimate_, std::memory_order_relaxed);
    return;
  }

  float alpha;
  if (calibrationFrames_ < kCalibrationPeriod) {
    alpha = kCalibrationAlpha;
    calibrationFrames_++;
  } else {
    /* Asymmetric: rise faster than fall. */
    alpha = (postRms > noiseFloorEstimate_) ? kTrackingAlphaRise
                                            : kTrackingAlphaFall;
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
 *  GATE TARGET (VAD-proportional soft gate)
 *
 *  Returns a target gain [kMinGateGain, 1.0].
 *
 *  Unlike a hard binary gate, this uses the VAD probability as a smooth
 *  gain curve so transitions sound natural:
 *
 *    vad >= kVadHigh (0.50) → gate = 1.0  (confident speech: full pass)
 *    vad in (kVadLow, kVadHigh) → gate interpolated (uncertain: partial)
 *    vad <= kVadLow  (0.10) → gate = kMinGateGain (clear non-speech: close)
 *
 *  A HOLD TIMER keeps the gate open for 200ms after the last confident
 *  speech frame, ensuring trailing consonants and breaths are not clipped.
 *
 *  Energy backup: if postRms is well above the noise floor, the gate stays
 *  partially open even when VAD is low — catches quiet speech that RNNoise's
 *  VAD is uncertain about (whispers, far-field mic, etc.).
 * ═══════════════════════════════════════════════════════════════════════════ */

float RNNoiseWrapper::computeGateTarget(float vad, float postRms) {
  /* During calibration keep gate open: let floor converge on real noise. */
  if (calibrationFrames_ < kCalibrationPeriod) {
    holdCounter_ = kHoldFrames;
    return 1.0f;
  }

  float vadThresh = vadThreshold_.load(std::memory_order_relaxed);

  /* Compute per-frame VAD gain using a smooth ramp.
   * Remap vad from [kVadLow, vadThresh] → [kMinGateGain, 1.0].
   * Clamp outside that range to the endpoints. */
  float vadGain;
  if (vad >= vadThresh) {
    vadGain = 1.0f;
  } else if (vad <= kVadLow) {
    vadGain = kMinGateGain;
  } else {
    float t = (vad - kVadLow) / std::max(vadThresh - kVadLow, 0.001f);
    /* Smoothstep for a gentle S-curve (no sharp kink). */
    t = t * t * (3.0f - 2.0f * t);
    vadGain = kMinGateGain + t * (1.0f - kMinGateGain);
  }

  /* Update hold counter when speech is clearly detected. */
  if (vad >= vadThresh) {
    holdCounter_ = kHoldFrames;
  }

  /* Energy backup: if signal is significantly above noise floor AND VAD is in
   * the uncertain range, treat it as speech to avoid clipping quiet voice. */
  float gateThresh = (noiseFloorEstimate_ > kAbsoluteMinFloor)
      ? noiseFloorEstimate_ * kFloorMultiplier
      : kFallbackThreshold;

  bool energyAboveNoise = (postRms > gateThresh * 1.8f);
  if (energyAboveNoise && vad > kVadLow) {
    holdCounter_ = std::max(holdCounter_, kHoldFrames / 2);
    vadGain = std::max(vadGain, 0.6f);
  }

  /* Hold: gate stays fully open for kHoldFrames after last speech. */
  if (holdCounter_ > 0) {
    holdCounter_--;
    return std::max(vadGain, 1.0f);
  }

  return vadGain;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SOFT SILENCE (Comfort Noise)
 *
 *  Injects barely-audible shaped noise when the gate is closed.
 *  Prevents "dead channel" sensation in headphones and click on re-open.
 *  The noise is strongly lowpass-shaped (sub-bass only) so it sounds
 *  like distant room ambience, not hiss or buzz.
 * ═══════════════════════════════════════════════════════════════════════════ */

void RNNoiseWrapper::applySoftSilence(float* frame) {
  if (!comfortNoiseEnabled_.load(std::memory_order_relaxed)) return;
  if (smoothGain_ >= kSoftSilenceGateThresh) return;

  float scale = (kSoftSilenceGateThresh - smoothGain_) / kSoftSilenceGateThresh;

  for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
    frame[i] += comfortNoiseSample() * scale;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SETTINGS
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

/* LFSR-based comfort noise with 1-pole lowpass shaping.
 * Strong lowpass keeps energy in sub-bass; amplitude is ~-70 dBFS. */
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
