/**
 * Clock-drift compensation between the capture device and the output device.
 *
 * The microphone and the output (e.g. VB-Cable) run on independent clocks.
 * Even a 100 ppm mismatch means 4.8 samples/s too many or too few: the output
 * ring either fills up (latency grows, then the ring overflows and drops
 * audio) or drains (underruns, zero-filled gaps). Phase 3 measured ring fill
 * but nothing controlled it.
 *
 * DriftController watches the output-ring fill after every processed frame
 * and returns a resampling ratio slightly above or below 1 (PI control on a
 * smoothed fill, clamped to +/-maxPpm). DriftResampler applies that ratio with
 * 4-point Hermite (Catmull-Rom) interpolation: at |ratio - 1| < 0.2 % the
 * change is inaudible, and it adds a constant 2-sample delay.
 *
 * Both are allocation-free and real-time safe (used in the processing
 * thread). Pure, so they are unit-tested with a simulated drifting consumer
 * (native/tests/engine_tests.cpp).
 */

#ifndef AINOICEGUARD_DRIFT_H
#define AINOICEGUARD_DRIFT_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace ainoiceguard {

class DriftResampler {
 public:
  static constexpr size_t kMaxIn = 512;  /* max input samples per call */
  static constexpr size_t kHist = 3;     /* samples carried between calls */

  void reset() {
    std::memset(hist_, 0, sizeof(hist_));
    t_ = 1.0;
    ratio_ = 1.0;
  }

  /** Input samples consumed per output sample (1 = passthrough). */
  void setRatio(double r) { ratio_ = r; }
  double ratio() const { return ratio_; }

  /**
   * Resample `n` (<= kMaxIn) input samples into `out` (capacity `maxOut`,
   * >= n + 2 is always enough for ratio >= 0.99). Returns samples written.
   */
  size_t process(const float* in, size_t n, float* out, size_t maxOut) {
    n = std::min(n, kMaxIn);
    float buf[kMaxIn + kHist];
    std::memcpy(buf, hist_, sizeof(hist_));
    std::memcpy(buf + kHist, in, n * sizeof(float));
    const size_t total = n + kHist;

    size_t k = 0;
    /* Interpolation at t needs buf[i-1 .. i+2], i = floor(t). */
    while (k < maxOut) {
      const size_t i = size_t(t_);
      if (i + 2 >= total) break;
      const float f = float(t_ - double(i));
      out[k++] = hermite(buf[i - 1], buf[i], buf[i + 1], buf[i + 2], f);
      t_ += ratio_;
    }

    std::memcpy(hist_, buf + total - kHist, sizeof(hist_));
    t_ -= double(total - kHist);
    return k;
  }

 private:
  static float hermite(float x0, float x1, float x2, float x3, float f) {
    const float c1 = 0.5f * (x2 - x0);
    const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
    const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    return ((c3 * f + c2) * f + c1) * f + x1;
  }

  float hist_[kHist] = {0, 0, 0};
  double t_ = 1.0;
  double ratio_ = 1.0;
};

/**
 * Ring level as a continuous quantity. The output callback removes one block
 * at a time, so the raw fill seen by the producer is a sawtooth whose phase
 * slides with clock drift (measured in simulation: the controller chased it
 * and the fill wandered by a whole block). Treating the consumer as a steady
 * drain -- subtracting the part of the next block it has "already used" since
 * its last read -- removes the sawtooth.
 */
inline double continuousLevel(double fill, double secondsSinceRead, double blockSeconds,
                              double blockSamples) {
  const double used = blockSeconds > 0 ? std::clamp(secondsSinceRead / blockSeconds, 0.0, 1.0) : 0.0;
  return fill - blockSamples * used;
}

class DriftController {
 public:
  struct Config {
    double targetFill = 960.0;  /* samples: 20 ms at 48 kHz */
    double maxPpm = 2000.0;     /* clamp (+/-0.2 %); real clock drift is ~10-300 ppm */
    double emaAlpha = 0.02;     /* fill smoothing per frame (~0.5 s) */
    /*
     * Loop per 10 ms frame: x' = -(T/2)·(kp·x/T + I + d) with I' = ki·x/T,
     * i.e. natural frequency w = sqrt(ki/2) and damping zeta = kp/(4w).
     * w = 0.004 rad/frame (~16 s period) and zeta ~0.8 settle in ~10-20 s.
     * (A first try with kp=5e-4, ki=5e-6 gave zeta ~0.08 and swung +/-450 samples.)
     */
    double kp = 0.0128;         /* ratio change per unit relative fill error */
    double ki = 0.000032;       /* integral gain per frame */
    int warmupFrames = 50;      /* 0.5 s of priming before control starts */
  };

  DriftController() = default;
  explicit DriftController(const Config& c) : cfg_(c) {}

  void reset() {
    ema_ = cfg_.targetFill;
    integ_ = 0.0;
    frames_ = 0;
    ratio_ = 1.0;
  }

  /**
   * Feed the output-ring fill (samples) after this frame was written.
   * Returns the ratio for the next frame (> 1 = output fewer samples, used
   * when the ring is too full; < 1 = more samples, when it drains).
   */
  double update(double fill) {
    ema_ += cfg_.emaAlpha * (fill - ema_);
    if (++frames_ <= cfg_.warmupFrames) {
      ratio_ = 1.0;
      return ratio_;
    }
    const double err = (ema_ - cfg_.targetFill) / cfg_.targetFill;
    const double lim = cfg_.maxPpm * 1e-6;
    integ_ = std::clamp(integ_ + cfg_.ki * err, -lim, lim); /* anti-windup */
    ratio_ = 1.0 + std::clamp(cfg_.kp * err + integ_, -lim, lim);
    return ratio_;
  }

  double ratio() const { return ratio_; }
  double ppm() const { return (ratio_ - 1.0) * 1e6; }
  double fillEma() const { return ema_; }
  const Config& config() const { return cfg_; }

 private:
  Config cfg_;
  double ema_ = 960.0;
  double integ_ = 0.0;
  int frames_ = 0;
  double ratio_ = 1.0;
};

}  // namespace ainoiceguard

#endif  // AINOICEGUARD_DRIFT_H
