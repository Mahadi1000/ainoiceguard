/**
 * Dynamics processing for the output chain: speech AGC + peak limiter.
 *
 * Both are allocation-free, per-frame, and own all their state, so they are
 * real-time safe and unit-testable without RNNoise (see
 * native/tests/engine_tests.cpp).
 *
 * SpeechAgc
 *   Slow automatic gain control that only adapts on speech frames, so
 *   background noise and pauses never pump the gain. A frame counts as speech
 *   only if BOTH hold:
 *     - VAD >= speechVad, and
 *     - the *input* frame level (before denoising) is >= minSnrDb above a
 *       minimum-statistics noise floor (lowest input frame level, ~1.6 s).
 *   The second condition is needed because RNNoise's VAD reports ~0.85 on
 *   stationary pink noise (measured, Phase 4); VAD alone let the AGC boost
 *   pure noise by +14.6 dB. The floor must be tracked on the input: RNNoise's
 *   own output on noise swells and dips by >9 dB, which looked like speech
 *   (still +12 dB of boost). On the input, stationary noise stays within a
 *   dB or two of its floor, while speech rises above it.
 *   The speech level is tracked in dB; the gain moves towards (target - level)
 *   with a slew limit and is ramped linearly across each frame (no zipper).
 *
 * PeakLimiter
 *   Zero-latency peak limiter: per-sample gain with instant attack and a
 *   smooth release. Gain never exceeds ceiling/|x|, so the output can never
 *   exceed the ceiling -- unlike the former hard clamp at +/-1.0, which
 *   produced clipping distortion.
 */

#ifndef AINOICEGUARD_DYNAMICS_H
#define AINOICEGUARD_DYNAMICS_H

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ainoiceguard {

inline float dbToLin(float db) { return std::pow(10.0f, db / 20.0f); }
inline float linToDb(float lin) { return 20.0f * std::log10(std::max(lin, 1e-9f)); }

class SpeechAgc {
 public:
  struct Config {
    float targetDb = -18.0f;       /* desired speech RMS (dBFS) */
    float maxGainDb = 18.0f;       /* boost ceiling: -35 dBFS speech reaches ~-17 */
    float minGainDb = -12.0f;      /* cut floor for very loud input */
    float initialGainDb = 2.0f;    /* = the former fixed makeup gain */
    float speechVad = 0.6f;        /* only frames this confident adapt the gain */
    float minSpeechDbfs = -60.0f;  /* ignore near-silent "speech" frames */
    float levelAlpha = 0.05f;      /* speech-level EMA per frame (~200 ms) */
    float slewUpDbPerFrame = 0.06f;    /* 6 dB/s at 10 ms frames */
    float slewDownDbPerFrame = 0.12f;  /* 12 dB/s: back off faster than boost */
    float minSnrDb = 6.0f;             /* input frame must be this far above the noise floor */
  };

  /* Minimum statistics: kFloorWindows sub-windows of kFloorSubFrames frames
   * (16 x 10 frames = 1.6 s at 10 ms). Fixed arrays: no allocation. */
  static constexpr int kFloorWindows = 16;
  static constexpr int kFloorSubFrames = 10;
  static constexpr int kFloorWarmupFrames = kFloorWindows * kFloorSubFrames;

  SpeechAgc() { reset(); }
  explicit SpeechAgc(const Config& c) : cfg_(c) { reset(); }

  void reset() {
    gainDb_ = cfg_.initialGainDb;
    levelDb_ = cfg_.targetDb - cfg_.initialGainDb; /* consistent with initial gain */
    haveLevel_ = false;
    for (float& m : floorMins_) m = kNoFloor;
    subMin_ = kNoFloor;
    subCount_ = 0;
    floorIdx_ = 0;
    floorFrames_ = 0;
  }

  /** Current noise-floor estimate (dBFS); very low until ~1 sub-window has passed. */
  float noiseFloorDb() const {
    float f = subMin_;
    for (float m : floorMins_) f = std::min(f, m);
    return f;
  }

  /**
   * Apply AGC to one frame in place.
   * @param inRms    RMS of `frame` before gain (the level the AGC regulates)
   * @param vad      speech probability for this frame [0, 1]
   * @param gateRms  RMS of the same frame *before denoising*; used only for
   *                 the noise-floor test. Defaults to inRms (unit tests).
   */
  void process(float* frame, size_t n, float inRms, float vad, float gateRms = -1.0f) {
    const float prevGainDb = gainDb_;
    const float inDb = linToDb(inRms);
    const float gateDb = gateRms >= 0.0f ? linToDb(gateRms) : inDb;

    /* Update the minimum-statistics floor with every frame (speech or not). */
    subMin_ = std::min(subMin_, gateDb);
    if (++subCount_ >= kFloorSubFrames) {
      floorMins_[floorIdx_] = subMin_;
      floorIdx_ = (floorIdx_ + 1) % kFloorWindows;
      subMin_ = kNoFloor;
      subCount_ = 0;
    }
    if (floorFrames_ < kFloorWarmupFrames) floorFrames_++;

    /* Don't adapt until the whole 1.6 s floor window has been seen: a floor
     * from only the first sub-windows is unreliable and let room noise be
     * boosted at start-up (live test: +9.8 dB in the first 4 s). */
    const bool aboveFloor =
        floorFrames_ >= kFloorWarmupFrames && gateDb >= noiseFloorDb() + cfg_.minSnrDb;

    if (vad >= cfg_.speechVad && inDb > cfg_.minSpeechDbfs && aboveFloor) {
      levelDb_ = haveLevel_ ? levelDb_ + cfg_.levelAlpha * (inDb - levelDb_) : inDb;
      haveLevel_ = true;
      const float want = std::clamp(cfg_.targetDb - levelDb_, cfg_.minGainDb, cfg_.maxGainDb);
      const float step = std::clamp(want - gainDb_, -cfg_.slewDownDbPerFrame, cfg_.slewUpDbPerFrame);
      gainDb_ += step;
    }
    /* Non-speech: hold the gain (no pumping on noise or pauses). */

    const float g0 = dbToLin(prevGainDb);
    const float g1 = dbToLin(gainDb_);
    const float inc = n ? (g1 - g0) / float(n) : 0.0f;
    float g = g0;
    for (size_t i = 0; i < n; i++) {
      g += inc;
      frame[i] *= g;
    }
  }

  float gainDb() const { return gainDb_; }
  float speechLevelDb() const { return levelDb_; }
  const Config& config() const { return cfg_; }

 private:
  static constexpr float kNoFloor = 1e9f; /* "no minimum yet" (min() ignores it) */

  Config cfg_;
  float gainDb_ = 0.0f;
  float levelDb_ = 0.0f;
  bool haveLevel_ = false;

  float floorMins_[kFloorWindows];
  float subMin_ = kNoFloor;
  int subCount_ = 0;
  int floorIdx_ = 0;
  int floorFrames_ = 0;
};

class PeakLimiter {
 public:
  explicit PeakLimiter(float ceilingDb = -1.0f, float releaseMs = 60.0f, float sampleRate = 48000.0f)
      : ceiling_(dbToLin(ceilingDb)),
        release_(1.0f - std::exp(-1.0f / (releaseMs * 0.001f * sampleRate))) {}

  void reset() { gain_ = 1.0f; }

  /** Limit one frame in place; returns the minimum gain applied in it. */
  float process(float* frame, size_t n) {
    float minGain = 1.0f;
    for (size_t i = 0; i < n; i++) {
      const float a = std::fabs(frame[i]);
      const float limit = a > ceiling_ ? ceiling_ / a : 1.0f;
      /* Release smoothly towards unity, but never above what this sample allows. */
      gain_ = std::min(limit, gain_ + release_ * (1.0f - gain_));
      frame[i] *= gain_;
      minGain = std::min(minGain, gain_);
    }
    return minGain;
  }

  float ceiling() const { return ceiling_; }

 private:
  float ceiling_;
  float release_;
  float gain_ = 1.0f;
};

}  // namespace ainoiceguard

#endif  // AINOICEGUARD_DYNAMICS_H
