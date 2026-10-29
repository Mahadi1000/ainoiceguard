/**
 * Unit tests for the engine's lock-free building blocks (no audio devices).
 * Built with -DNG_BUILD_BENCH=ON as `ng_engine_tests`; run by
 * test/native-unit.test.js. Exit code 0 = all passed.
 */

#include <cmath>
#include <cstdio>
#include <vector>

#include "dynamics.h"
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

/* A 480-sample frame of a 440 Hz sine with the given RMS (dBFS), continuing phase. */
static void sineFrame(float* f, float rmsDb, double& phase) {
  const float amp = dbToLin(rmsDb) * std::sqrt(2.0f);
  for (int i = 0; i < 480; i++) {
    f[i] = amp * float(std::sin(phase));
    phase += 2.0 * 3.14159265358979 * 440.0 / 48000.0;
  }
}

static float rmsOf(const float* f, int n) {
  double s = 0;
  for (int i = 0; i < n; i++) s += double(f[i]) * f[i];
  return float(std::sqrt(s / n));
}

static void testLimiter() {
  PeakLimiter lim;  /* -1 dBFS ceiling */
  float f[480];
  double ph = 0;
  float peak = 0;
  /* +6 dBFS peaks (amplitude 2.0) must never exceed the ceiling. */
  for (int k = 0; k < 50; k++) {
    for (int i = 0; i < 480; i++) {
      f[i] = 2.0f * float(std::sin(ph));
      ph += 2.0 * 3.14159265358979 * 440.0 / 48000.0;
    }
    lim.process(f, 480);
    for (float v : f) peak = std::max(peak, std::fabs(v));
  }
  CHECK(peak <= lim.ceiling() + 1e-6f);
  CHECK(peak > lim.ceiling() * 0.99f); /* and it actually reaches the ceiling */

  /* After loud input, a quiet signal recovers to unity gain (release). */
  for (int k = 0; k < 100; k++) {
    sineFrame(f, -30.0f, ph);
    lim.process(f, 480);
  }
  sineFrame(f, -30.0f, ph);
  const float before = rmsOf(f, 480);
  const float minGain = lim.process(f, 480);
  CHECK(minGain > 0.999f);
  CHECK(std::fabs(rmsOf(f, 480) - before) < 1e-4f);
}

/*
 * Speech-like test signal: 200 ms "syllables" at inDb (VAD = vad) separated by
 * 100 ms gaps 30 dB lower (VAD 0.1), like pauses between words. Returns the
 * output level of the last syllable frame.
 */
static float runAgc(SpeechAgc& agc, float inDb, float vad, int frames, double& ph) {
  float f[480];
  float outDb = 0;
  for (int k = 0; k < frames; k++) {
    const bool syllable = (k % 30) < 20;
    sineFrame(f, syllable ? inDb : inDb - 30.0f, ph);
    agc.process(f, 480, rmsOf(f, 480), syllable ? vad : 0.1f);
    if (syllable) outDb = linToDb(rmsOf(f, 480));
  }
  return outDb;
}

/* Stationary signal at a constant level, every frame flagged as speech. */
static void runSteady(SpeechAgc& agc, float inDb, float vad, int frames, double& ph) {
  float f[480];
  for (int k = 0; k < frames; k++) {
    sineFrame(f, inDb, ph);
    agc.process(f, 480, rmsOf(f, 480), vad);
  }
}

static void testAgc() {
  double ph = 0;
  /* Quiet speech (-35 dBFS) converges to the -18 dBFS target within 10 s. */
  SpeechAgc quiet;
  const float outQuiet = runAgc(quiet, -35.0f, 1.0f, 1000, ph);
  CHECK(std::fabs(outQuiet - -18.0f) <= 1.0f);
  CHECK(std::fabs(quiet.gainDb() - 17.0f) <= 1.0f);

  /* Loud speech (-10 dBFS) is turned down to the target. */
  SpeechAgc loud;
  const float outLoud = runAgc(loud, -10.0f, 1.0f, 1000, ph);
  CHECK(std::fabs(outLoud - -18.0f) <= 1.0f);

  /* Range limits: very quiet input stops at +18 dB. */
  SpeechAgc tiny;
  runAgc(tiny, -50.0f, 1.0f, 2000, ph);
  CHECK(tiny.gainDb() <= 18.0f + 1e-4f && tiny.gainDb() > 17.9f);

  /* Non-speech frames (VAD below threshold) hold the gain: no pumping. */
  SpeechAgc hold;
  runAgc(hold, -35.0f, 1.0f, 300, ph);
  const float g = hold.gainDb();
  runAgc(hold, -60.0f, 0.1f, 500, ph); /* noise between words */
  CHECK(std::fabs(hold.gainDb() - g) < 1e-6f);

  /* Regression (Phase 4): stationary noise that the VAD calls speech (RNNoise
   * reports ~0.85 on pink noise) must not be boosted -- it never rises above
   * its own minimum-statistics floor. */
  SpeechAgc steady;
  const float g0steady = steady.gainDb();
  runSteady(steady, -46.0f, 0.85f, 1000, ph);
  CHECK(std::fabs(steady.gainDb() - g0steady) < 1e-6f);
  CHECK(std::fabs(steady.noiseFloorDb() - -46.0f) < 0.5f);

  /* Gain is ramped within a frame: no step at frame boundaries. */
  SpeechAgc ramp;
  float f[480];
  for (int i = 0; i < 480; i++) f[i] = 0.01f;
  const float g0 = dbToLin(ramp.gainDb());
  ramp.process(f, 480, 0.01f, 1.0f);
  CHECK(std::fabs(f[0] / 0.01f - g0) < 0.01f);  /* starts at the previous gain */
  CHECK(std::fabs(f[479] / 0.01f - dbToLin(ramp.gainDb())) < 1e-4f);
}

int main() {
  testLimiter();
  testAgc();
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
