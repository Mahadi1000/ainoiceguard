/**
 * EngineStats -- live, lock-free counters for the audio engine.
 *
 * Every field has exactly one writer thread, so plain relaxed atomics are
 * enough (no read-modify-write races):
 *   - capture callback   -> input xrun flags, capture-ring drops, callback count
 *   - output callback    -> output xrun flags, output underruns, callback count
 *   - processing thread  -> per-frame timing, ring-fill samples, output-ring drops,
 *                           restarts
 * Readers (the N-API thread via getMetrics()) only load.
 *
 * Nothing here allocates; all methods are safe to call from the real-time
 * callbacks.
 */

#ifndef AINOICEGUARD_ENGINE_STATS_H
#define AINOICEGUARD_ENGINE_STATS_H

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ainoiceguard {

/** Upper bounds (µs, inclusive) of the processing-time histogram buckets.
 *  The last bucket is open-ended (> 10 ms = over the real-time budget). */
static constexpr uint32_t kProcBucketBoundsUs[] = {100, 250, 500, 1000, 2000, 5000, 10000};
static constexpr size_t kProcBuckets = sizeof(kProcBucketBoundsUs) / sizeof(uint32_t) + 1;

/** Budget for one 480-sample frame at 48 kHz. */
static constexpr uint32_t kFrameBudgetUs = 10000;

inline size_t procBucketFor(uint32_t us) {
  for (size_t i = 0; i < kProcBuckets - 1; i++) {
    if (us <= kProcBucketBoundsUs[i]) return i;
  }
  return kProcBuckets - 1;
}

/** Single-writer helpers: no CAS needed because only one thread writes. */
inline void bump(std::atomic<uint64_t>& a, uint64_t by = 1) {
  a.store(a.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
}
inline void storeMax(std::atomic<uint32_t>& a, uint32_t v) {
  if (v > a.load(std::memory_order_relaxed)) a.store(v, std::memory_order_relaxed);
}
inline void storeMin(std::atomic<uint32_t>& a, uint32_t v) {
  if (v < a.load(std::memory_order_relaxed)) a.store(v, std::memory_order_relaxed);
}

struct EngineStats {
  /* ── PortAudio status flags (counted, no longer trigger restarts) ── */
  std::atomic<uint64_t> inputUnderflows{0};
  std::atomic<uint64_t> inputOverflows{0};
  std::atomic<uint64_t> outputUnderflows{0};
  std::atomic<uint64_t> outputOverflows{0};

  /* ── Callback activity (also used by the device-loss watchdog) ── */
  std::atomic<uint64_t> captureCallbacks{0};
  std::atomic<uint64_t> outputCallbacks{0};

  /* ── Ring buffers ── */
  std::atomic<uint64_t> captureDroppedSamples{0}; /* capture ring full: mic audio lost */
  std::atomic<uint64_t> outputDroppedSamples{0};  /* output ring full: processed audio lost */
  std::atomic<uint64_t> outputUnderrunEvents{0};  /* output cb had to zero-fill (after priming) */
  std::atomic<uint64_t> outputUnderrunSamples{0};

  /* Fill levels in samples, sampled once per processed frame. */
  std::atomic<uint32_t> captureFillMax{0};
  std::atomic<uint32_t> outputFillMin{UINT32_MAX};
  std::atomic<uint32_t> outputFillMax{0};
  std::atomic<uint64_t> captureFillSum{0};
  std::atomic<uint64_t> outputFillSum{0};
  std::atomic<uint64_t> fillSamples{0};

  /* ── Processing time per frame (processFrame only) ── */
  std::atomic<uint64_t> procHist[kProcBuckets] = {};
  std::atomic<uint64_t> procFrames{0};
  std::atomic<uint64_t> procTotalUs{0};
  std::atomic<uint32_t> procMaxUs{0};
  std::atomic<uint64_t> procOverruns{0}; /* frames slower than kFrameBudgetUs */

  /* ── Recovery ── */
  std::atomic<uint32_t> restarts{0};         /* successful automatic restarts */
  std::atomic<uint32_t> restartFailures{0};  /* gave up after all attempts */
  std::atomic<uint32_t> deviceLossEvents{0}; /* watchdog detections */

  /** Reset everything. Call only while no audio thread is running. */
  void reset() {
    for (auto* a : {&inputUnderflows, &inputOverflows, &outputUnderflows, &outputOverflows,
                    &captureCallbacks, &outputCallbacks, &captureDroppedSamples,
                    &outputDroppedSamples, &outputUnderrunEvents, &outputUnderrunSamples,
                    &captureFillSum, &outputFillSum, &fillSamples, &procFrames, &procTotalUs,
                    &procOverruns}) {
      a->store(0, std::memory_order_relaxed);
    }
    for (auto& h : procHist) h.store(0, std::memory_order_relaxed);
    captureFillMax.store(0, std::memory_order_relaxed);
    outputFillMin.store(UINT32_MAX, std::memory_order_relaxed);
    outputFillMax.store(0, std::memory_order_relaxed);
    procMaxUs.store(0, std::memory_order_relaxed);
    restarts.store(0, std::memory_order_relaxed);
    restartFailures.store(0, std::memory_order_relaxed);
    deviceLossEvents.store(0, std::memory_order_relaxed);
  }

  /** Processing thread: record one frame's processing time. */
  void recordFrameTime(uint32_t us) {
    bump(procHist[procBucketFor(us)]);
    bump(procFrames);
    bump(procTotalUs, us);
    storeMax(procMaxUs, us);
    if (us > kFrameBudgetUs) bump(procOverruns);
  }

  /** Processing thread: sample ring fill levels (in samples). */
  void recordFill(uint32_t captureFill, uint32_t outputFill, bool outputActive) {
    storeMax(captureFillMax, captureFill);
    bump(captureFillSum, captureFill);
    if (outputActive) {
      storeMin(outputFillMin, outputFill);
      storeMax(outputFillMax, outputFill);
      bump(outputFillSum, outputFill);
    }
    bump(fillSamples);
  }
};

}  // namespace ainoiceguard

#endif  // AINOICEGUARD_ENGINE_STATS_H
