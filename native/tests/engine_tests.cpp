/**
 * Unit tests for the engine's lock-free building blocks (no audio devices).
 * Built with -DNG_BUILD_BENCH=ON as `ng_engine_tests`; run by
 * test/native-unit.test.js. Exit code 0 = all passed.
 */

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "device_select.h"
#include "drift.h"
#include "dynamics.h"
#include "engine_stats.h"
#include "loopback.h"
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

static DeviceEntry dev(int idx, const char* name, const char* api, int in, int out,
                       bool defIn = false, bool defOut = false) {
  DeviceEntry d;
  d.index = idx;
  d.name = name;
  d.hostApi = api;
  d.maxInputChannels = in;
  d.maxOutputChannels = out;
  d.isHostDefaultInput = defIn;
  d.isHostDefaultOutput = defOut;
  return d;
}

static void testDeviceSelect() {
  const std::string W = "Windows WASAPI";
  /* Mirrors the real list: MME first (global default), WASAPI later. */
  std::vector<DeviceEntry> list = {
      dev(0, "Microsoft Sound Mapper - Input", "MME", 2, 0, true, false),
      dev(1, "External Microphone (Synaptics ", "MME", 2, 0),
      dev(4, "Speakers (Synaptics HD Audio)", W.c_str(), 0, 2, false, true),
      dev(5, "External Microphone (Synaptics HD Audio)", W.c_str(), 2, 0, true, false),
  };

  /* Default -> the preferred host API's default, not the first (MME) one. */
  Resolved r = resolveDevice(list, DeviceSpec::def(), true, W);
  CHECK(r.index == 5 && !r.fellBack);
  r = resolveDevice(list, DeviceSpec::def(), false, W);
  CHECK(r.index == 4);

  /* By id survives an index shift (Krisp's devices appeared: mic 5 -> 9). */
  const DeviceSpec mic = DeviceSpec::byId(W + "|External Microphone (Synaptics HD Audio)");
  CHECK(resolveDevice(list, mic, true, W).index == 5);
  auto shifted = list;
  shifted[3].index = 9;
  shifted.push_back(dev(5, "Krisp Speaker (Krisp Audio)", W.c_str(), 0, 2));
  r = resolveDevice(shifted, mic, true, W);
  CHECK(r.index == 9 && !r.fellBack);

  /* The stale index now points at an output: never used as an input. */
  r = resolveDevice(shifted, DeviceSpec::byIndex(5), true, W);
  CHECK(r.index == 9 && r.fellBack);  /* falls back to the default input */

  /* Same name on another host API is not a match (different path). */
  r = resolveDevice(list, DeviceSpec::byId("MME|External Microphone (Synaptics HD Audio)"), true, W);
  CHECK(r.index == 5 && r.fellBack);

  /* Missing device -> default + fellBack; none -> -1. */
  r = resolveDevice(list, DeviceSpec::byId(W + "|USB Headset"), true, W);
  CHECK(r.index == 5 && r.fellBack);
  CHECK(resolveDevice(list, DeviceSpec::none(), false, W).index == -1);

  /* No preferred host API present -> any host default, then any device. */
  r = resolveDevice(list, DeviceSpec::def(), true, "ALSA");
  CHECK(r.index == 0);
  std::vector<DeviceEntry> bare = {dev(3, "Mic", "ALSA", 1, 0)};
  CHECK(resolveDevice(bare, DeviceSpec::def(), true, "ALSA").index == 3);
  std::vector<DeviceEntry> none;
  r = resolveDevice(none, DeviceSpec::def(), true, W);
  CHECK(r.index == -1 && !r.fellBack);
}

/*
 * Event simulation of the output path: the processing thread writes one
 * 480-sample frame every 10 ms of *mic* time (through the drift resampler),
 * the output callback reads 480 samples every 10 ms of *output-device* time,
 * which runs `ppm` faster (+) or slower (-). Same 4096-sample ring as the
 * engine. Returns fill/underrun/overflow statistics after `settleS`.
 */
struct DriftSim {
  size_t underrunSamples = 0;   /* after priming and settling */
  size_t overflowSamples = 0;   /* dropped because the ring was full */
  double fillMin = 1e9, fillMax = -1e9;
  double finalPpm = 0;
};

static DriftSim simulateDrift(double ppm, double seconds, bool control, double settleS = 60.0,
                              double target = -1, double jitterS = 0.0) {
  /* Deterministic jitter in [-jitterS, +jitterS] on every event time. */
  uint32_t lcg = 12345;
  auto jit = [&]() {
    lcg = lcg * 1664525u + 1013904223u;
    return jitterS * (double(lcg >> 8) / double(1u << 24) * 2.0 - 1.0);
  };
  RingBuffer ring(4096);
  DriftResampler rs;
  rs.reset();
  DriftController::Config cfg;
  if (target > 0) cfg.targetFill = target;
  DriftController dc(cfg);
  dc.reset();
  DriftSim r;

  float in[480], out[600], sink[480];
  for (float& v : in) v = 0.1f;
  const double prodStep = 0.010;                 /* mic clock */
  const double consStep = 0.010 / (1.0 + ppm * 1e-6); /* output clock */
  double np = 0.0, nc = 0.005;                   /* nominal event times */
  double tp = np, tc = nc;                       /* actual (jittered) times */
  double tcLast = 0.0;                           /* time of the last consumer read */
  bool primed = false;
  double ppmSum = 0;
  size_t ppmCount = 0;

  while (tp < seconds || tc < seconds) {
    if (tp <= tc) {
      const size_t n = rs.process(in, 480, out, 600);
      const size_t w = ring.write(out, n);
      if (w < n && tp > settleS) r.overflowSamples += n - w;
      const double fill = double(ring.available_read());
      /* The engine uses the nominal block period; drift makes it ~ppm off. */
      const double level = continuousLevel(fill, tp - tcLast, 0.010, 480.0);
      rs.setRatio(control ? dc.update(level) : 1.0);
      if (tp > settleS) {
        r.fillMin = std::min(r.fillMin, fill);
        r.fillMax = std::max(r.fillMax, fill);
        ppmSum += dc.ppm();
        ppmCount++;
      }
      np += prodStep;
      tp = np + jit();
    } else {
      const size_t got = ring.read(sink, 480);
      if (got > 0) primed = true;
      if (primed && got < 480 && tc > settleS) r.underrunSamples += 480 - got;
      tcLast = tc;
      nc += consStep;
      tc = nc + jit();
    }
  }
  r.finalPpm = ppmCount ? ppmSum / double(ppmCount) : 0.0;
  return r;
}

static void testDriftControl() {
  for (double ppm : {300.0, -300.0, 1000.0, -1000.0, 0.0}) {
    const DriftSim on = simulateDrift(ppm, 600.0, true);
    if (on.underrunSamples || on.overflowSamples) {
      std::printf("  drift %+.0f ppm: underrun %zu overflow %zu fill [%.0f, %.0f]\n", ppm,
                  on.underrunSamples, on.overflowSamples, on.fillMin, on.fillMax);
    }
    CHECK(on.underrunSamples == 0);
    CHECK(on.overflowSamples == 0);
    /* Fill stays near the 960-sample target (+/- one 480 block + margin). */
    CHECK(on.fillMin >= 960 - 700 && on.fillMax <= 960 + 700);
    /* The controller learns the drift: output-side faster => consume less. */
    CHECK(std::fabs(-on.finalPpm - ppm) < std::max(30.0, std::fabs(ppm) * 0.1));
  }
  /* Robust to +/-4 ms timing jitter on both sides (2x the worst frame time
   * measured live in Phase 4a). With the default 960-sample target only:
   * 720 underran at 4 ms in the sweep (`ng_engine_tests sweep`). */
  for (double ppm : {300.0, -300.0}) {
    const DriftSim j = simulateDrift(ppm, 600.0, true, 60.0, -1, 0.004);
    CHECK(j.underrunSamples == 0 && j.overflowSamples == 0);
    CHECK(std::fabs(-j.finalPpm - ppm) < 10.0);
  }
  /* Without control the same drift breaks the stream (test sensitivity). */
  CHECK(simulateDrift(300.0, 600.0, false).underrunSamples > 0);
  CHECK(simulateDrift(-300.0, 600.0, false).overflowSamples > 0);
}

static void testDriftResampler() {
  /* Ratio 1: exact passthrough with a constant 2-sample delay. */
  DriftResampler rs;
  rs.reset();
  std::vector<float> in(4800), out;
  for (size_t i = 0; i < in.size(); i++) in[i] = float(std::sin(0.01 * double(i)));
  float buf[600];
  for (size_t off = 0; off < in.size(); off += 480) {
    const size_t n = rs.process(in.data() + off, 480, buf, 600);
    out.insert(out.end(), buf, buf + n);
  }
  CHECK(out.size() == in.size());
  float maxErr = 0;
  for (size_t k = 2; k < out.size(); k++) maxErr = std::max(maxErr, std::fabs(out[k] - in[k - 2]));
  CHECK(maxErr < 1e-6f);

  /* Ratio 1 + 500 ppm on a 1 kHz tone: still a clean tone at f * ratio. */
  const double ratio = 1.0005;
  rs.reset();
  rs.setRatio(ratio);
  std::vector<float> tone(48000);
  for (size_t i = 0; i < tone.size(); i++) tone[i] = float(0.5 * std::sin(2 * 3.14159265358979 * 1000.0 * i / 48000.0));
  out.clear();
  for (size_t off = 0; off < tone.size(); off += 480) {
    const size_t n = rs.process(tone.data() + off, 480, buf, 600);
    out.insert(out.end(), buf, buf + n);
  }
  CHECK(std::fabs(double(out.size()) - 48000.0 / ratio) <= 2.0);
  /* Least-squares fit of a sine at the expected frequency; residual = error. */
  const double w = 2 * 3.14159265358979 * 1000.0 * ratio / 48000.0;
  double ss = 0, cc = 0, sc = 0, ys = 0, yc = 0;
  for (size_t k = 100; k < out.size() - 100; k++) {
    const double s = std::sin(w * k), c = std::cos(w * k), y = out[k];
    ss += s * s; cc += c * c; sc += s * c; ys += y * s; yc += y * c;
  }
  const double det = ss * cc - sc * sc;
  const double a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
  double sig = 0, err = 0;
  for (size_t k = 100; k < out.size() - 100; k++) {
    const double fit = a * std::sin(w * k) + b * std::cos(w * k);
    sig += fit * fit;
    err += (out[k] - fit) * (out[k] - fit);
  }
  const double snrDb = 10 * std::log10(sig / std::max(err, 1e-30));
  if (snrDb < 60) std::printf("  resampler SNR %.1f dB\n", snrDb);
  CHECK(snrDb >= 60.0);
}

static void testLoopbackMath() {
  using namespace loopback;
  const double sr = 48000.0;
  const auto burst = makeBurst(sr);
  CHECK(burst.size() == 48);

  /*
   * Synthetic run: bursts every 250 ms on the "output"; the "input" receives
   * them 37.5 ms later, attenuated, with noise, and its stream started 20 ms
   * after the output's. Timestamps come in 480-sample blocks, as from PA.
   */
  const size_t n = size_t(sr * 3);
  const double inStart = 0.020, latency = 0.0375;
  std::vector<float> rec(n, 0.0f);
  std::vector<double> played;
  uint32_t lcg = 1;
  for (size_t i = 0; i < n; i++) {
    lcg = lcg * 1664525u + 1013904223u;
    rec[i] = 0.002f * (float(lcg >> 8) / float(1u << 24) - 0.5f); /* noise floor */
  }
  for (int k = 0; k < 10; k++) {
    const double tPlay = 0.1 + 0.25 * k;
    played.push_back(tPlay);
    const size_t at = size_t((tPlay + latency - inStart) * sr);
    for (size_t i = 0; i < burst.size(); i++) rec[at + i] += 0.3f * burst[i];
  }
  std::vector<Stamp> inStamps;
  for (size_t s = 0; s < n; s += 480) inStamps.push_back({s, inStart + double(s) / sr});

  const auto onsets = detectOnsets(rec, 0.02f, size_t(sr * 0.05));
  CHECK(onsets.size() == 10);
  std::vector<double> recTimes;
  for (size_t o : onsets) recTimes.push_back(timeOf(inStamps, o, sr));
  const auto lat = matchLatencies(played, recTimes, 0.5);
  CHECK(lat.size() == 10);
  /* Onset is detected a few samples into the burst's rising window. */
  CHECK(std::fabs(median(lat) - latency) < 0.0005);

  /* Clock estimate: a stream that delivers 48000 * (1 + 120e-6) frames/s. */
  std::vector<Stamp> fast;
  for (int b = 0; b <= 6000; b++) fast.push_back({size_t(b) * 480, double(b) * 480.0 / (sr * (1 + 120e-6))});
  CHECK(std::fabs(clockPpm(fast, sr) - 120.0) < 0.5);
  CHECK(clockPpm({}, sr) == 0.0);

  /* 10 s with +/-2 ms callback-time jitter: still within 20 ppm. */
  std::vector<Stamp> jittery;
  uint32_t j = 99;
  for (int b = 0; b <= 1000; b++) {
    j = j * 1664525u + 1013904223u;
    const double jit = 0.002 * (double(j >> 8) / double(1u << 24) * 2 - 1);
    jittery.push_back({size_t(b) * 480, double(b) * 480.0 / (sr * (1 - 80e-6)) + jit});
  }
  const double est = clockPpm(jittery, sr);
  if (std::fabs(est - -80.0) >= 20.0) std::printf("  jittery clock estimate %.1f ppm\n", est);
  CHECK(std::fabs(est - -80.0) < 20.0);
}

int main(int argc, char** argv) {
  /* `ng_engine_tests sweep`: print drift behaviour for a range of targets. */
  if (argc > 1 && std::string(argv[1]) == "sweep") {
    for (double jitterMs : {0.0, 2.0, 4.0}) {
      for (double target : {720.0, 960.0}) {
        for (double ppm : {1000.0, 300.0, 0.0, -300.0, -1000.0}) {
          const DriftSim s = simulateDrift(ppm, 600.0, true, 60.0, target, jitterMs / 1000.0);
          std::printf("jitter %.0f ms target %4.0f drift %+5.0f ppm: fill [%4.0f, %4.0f] underrun %4zu overflow %4zu learned %+7.1f ppm\n",
                      jitterMs, target, ppm, s.fillMin, s.fillMax, s.underrunSamples, s.overflowSamples, -s.finalPpm);
        }
      }
    }
    return 0;
  }
  testLoopbackMath();
  testDriftResampler();
  testDriftControl();
  testDeviceSelect();
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
