/**
 * Unit tests for the engine's lock-free building blocks (no audio devices).
 * Built with -DNG_BUILD_BENCH=ON as `ng_engine_tests`; run by
 * test/native-unit.test.js. Exit code 0 = all passed.
 */

#include <cstdio>
#include <vector>

#include "engine_stats.h"
#include "ringbuffer.h"

using namespace ainoiceguard;

static int g_failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
      g_failures++;                                                       \
    }                                                                     \
  } while (0)

static void testBuckets() {
  CHECK(procBucketFor(0) == 0);
  CHECK(procBucketFor(100) == 0);   /* inclusive upper bound */
  CHECK(procBucketFor(101) == 1);
  CHECK(procBucketFor(999) == 3);
  CHECK(procBucketFor(10000) == kProcBuckets - 2);
  CHECK(procBucketFor(10001) == kProcBuckets - 1);  /* over budget */
  CHECK(procBucketFor(4000000000u) == kProcBuckets - 1);
}

static void testFrameTimes() {
  EngineStats s;
  s.recordFrameTime(200);
  s.recordFrameTime(300);
  s.recordFrameTime(12000);
  CHECK(s.procFrames.load() == 3);
  CHECK(s.procTotalUs.load() == 12500);
  CHECK(s.procMaxUs.load() == 12000);
  CHECK(s.procOverruns.load() == 1);
  CHECK(s.procHist[1].load() == 1);  /* 200 -> (100,250] */
  CHECK(s.procHist[2].load() == 1);  /* 300 -> (250,500] */
  CHECK(s.procHist[kProcBuckets - 1].load() == 1);
  uint64_t total = 0;
  for (auto& h : s.procHist) total += h.load();
  CHECK(total == 3);
}

static void testFill() {
  EngineStats s;
  s.recordFill(100, 999, false);  /* output inactive: output stats untouched */
  CHECK(s.outputFillMin.load() == UINT32_MAX);
  CHECK(s.outputFillMax.load() == 0);
  CHECK(s.captureFillMax.load() == 100);
  s.recordFill(50, 480, true);
  s.recordFill(300, 960, true);
  CHECK(s.captureFillMax.load() == 300);
  CHECK(s.outputFillMin.load() == 480);
  CHECK(s.outputFillMax.load() == 960);
  CHECK(s.captureFillSum.load() == 450);
  CHECK(s.outputFillSum.load() == 1440);
  CHECK(s.fillSamples.load() == 3);
}

static void testReset() {
  EngineStats s;
  s.recordFrameTime(20000);
  s.recordFill(10, 10, true);
  bump(s.inputOverflows);
  s.restarts.store(3);
  s.reset();
  CHECK(s.procFrames.load() == 0);
  CHECK(s.procOverruns.load() == 0);
  CHECK(s.procMaxUs.load() == 0);
  CHECK(s.inputOverflows.load() == 0);
  CHECK(s.restarts.load() == 0);
  CHECK(s.outputFillMin.load() == UINT32_MAX);
  for (auto& h : s.procHist) CHECK(h.load() == 0);
}

static void testRingBuffer() {
  RingBuffer rb(1000);  /* rounds up to 1024 */
  CHECK(rb.capacity() == 1024);
  std::vector<float> in(2000), out(2000);
  for (size_t i = 0; i < in.size(); i++) in[i] = float(i);

  /* Capacity - 1 usable slots; the rest is reported as not written (a drop). */
  CHECK(rb.write(in.data(), 2000) == 1023);
  CHECK(rb.available_read() == 1023);
  CHECK(rb.available_write() == 0);
  CHECK(rb.write(in.data(), 10) == 0);

  /* Partial read, then wrap around the end of the buffer. */
  CHECK(rb.read(out.data(), 1000) == 1000);
  CHECK(out[0] == 0.0f && out[999] == 999.0f);
  CHECK(rb.write(in.data() + 1023, 500) == 500);
  CHECK(rb.read(out.data(), 2000) == 523);
  CHECK(out[0] == 1000.0f && out[22] == 1022.0f && out[23] == 1023.0f && out[522] == 1522.0f);
  CHECK(rb.available_read() == 0);
  CHECK(rb.read(out.data(), 1) == 0);  /* underrun: nothing to read */
}

int main() {
  testBuckets();
  testFrameTimes();
  testFill();
  testReset();
  testRingBuffer();
  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all engine tests passed\n");
  return 0;
}
