/**
 * Loopback measurement helpers (pure; used by native/bench/ng_loopback.cpp).
 *
 * The test plays short tone bursts on an output device (e.g. "CABLE Input")
 * and records an input device (e.g. "CABLE Output"). Each burst's playback
 * time comes from PortAudio's outputBufferDacTime, each detected onset's time
 * from inputBufferAdcTime: both are on PortAudio's stream clock, so their
 * difference is the playback-to-capture latency of that path.
 *
 * Separately, each stream's real sample rate is estimated from its callback
 * timestamps vs. the frames it processed: the difference between the two
 * devices is the clock drift the engine's drift control has to correct.
 */

#ifndef AINOICEGUARD_LOOPBACK_H
#define AINOICEGUARD_LOOPBACK_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace ainoiceguard {
namespace loopback {

/** 1 ms Hann-windowed 2 kHz burst: survives format conversion, easy to detect. */
inline std::vector<float> makeBurst(double sampleRate, float amplitude = 0.5f) {
  const size_t n = size_t(sampleRate * 0.001);
  std::vector<float> b(n);
  for (size_t i = 0; i < n; i++) {
    const double w = 0.5 - 0.5 * std::cos(2 * 3.14159265358979 * double(i) / double(n - 1));
    b[i] = float(amplitude * w * std::sin(2 * 3.14159265358979 * 2000.0 * double(i) / sampleRate));
  }
  return b;
}

/**
 * Indices where |x| first exceeds `threshold`, at least `refractory` samples
 * apart (one onset per burst, ignoring its own ringing).
 */
inline std::vector<size_t> detectOnsets(const std::vector<float>& x, float threshold,
                                        size_t refractory) {
  std::vector<size_t> on;
  size_t next = 0;
  for (size_t i = 0; i < x.size(); i++) {
    if (i >= next && std::fabs(x[i]) > threshold) {
      on.push_back(i);
      next = i + refractory;
    }
  }
  return on;
}

/** Timestamped block: the first sample's index and its PortAudio time (s). */
struct Stamp {
  size_t sample = 0;
  double time = 0.0;
};

/** Time of sample `idx`, from the nearest stamp at or before it. */
inline double timeOf(const std::vector<Stamp>& stamps, size_t idx, double sampleRate) {
  if (stamps.empty()) return 0.0;
  auto it = std::upper_bound(stamps.begin(), stamps.end(), idx,
                             [](size_t v, const Stamp& s) { return v < s.sample; });
  const Stamp& s = (it == stamps.begin()) ? stamps.front() : *(it - 1);
  return s.time + (double(idx) - double(s.sample)) / sampleRate;
}

/**
 * Pair each played burst time with the first recorded onset after it, within
 * `maxLatency` seconds. Returns the latencies (s) of matched bursts.
 */
inline std::vector<double> matchLatencies(const std::vector<double>& played,
                                          const std::vector<double>& recorded, double maxLatency) {
  std::vector<double> lat;
  size_t j = 0;
  for (double p : played) {
    while (j < recorded.size() && recorded[j] < p) j++;
    if (j < recorded.size() && recorded[j] - p <= maxLatency) lat.push_back(recorded[j] - p);
  }
  return lat;
}

/** Median of a copy (0 for empty). */
inline double median(std::vector<double> v) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const size_t m = v.size() / 2;
  return v.size() % 2 ? v[m] : 0.5 * (v[m - 1] + v[m]);
}

/**
 * Real sample rate of a stream from its stamps, as ppm relative to `nominal`
 * (positive = device clock runs fast). 0 if not enough data.
 *
 * Least-squares slope of sample index vs time over ALL stamps: callback times
 * jitter by milliseconds, so first/last alone would be off by ~jitter/duration
 * (2 ms over 10 s = 200 ppm, larger than real drift). The fit averages it out.
 */
inline double clockPpm(const std::vector<Stamp>& stamps, double nominal) {
  if (stamps.size() < 3) return 0.0;
  const double n = double(stamps.size());
  double mt = 0, ms = 0;
  for (const auto& s : stamps) { mt += s.time; ms += double(s.sample); }
  mt /= n;
  ms /= n;
  double cov = 0, var = 0;
  for (const auto& s : stamps) {
    const double dt = s.time - mt;
    cov += dt * (double(s.sample) - ms);
    var += dt * dt;
  }
  if (var <= 0) return 0.0;
  return (cov / var / nominal - 1.0) * 1e6;
}

}  // namespace loopback
}  // namespace ainoiceguard

#endif  // AINOICEGUARD_LOOPBACK_H
