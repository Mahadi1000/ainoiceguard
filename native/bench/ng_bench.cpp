/**
 * ng_bench -- offline noise-suppression benchmark.
 *
 * Runs the same processing code as the addon over a 48 kHz WAV file, frame by
 * frame (480 samples = 10 ms), and reports per-frame processing time.
 *
 * Backends:
 *   rnnoise-raw    rnnoise_process_frame() only (the model, no post-processing)
 *   rnnoise-chain  RNNoiseWrapper::processFrame() (HPF, blend, makeup gain,
 *                  VAD gate, comfort noise) -- exactly what the app runs
 *
 * Usage:
 *   ng_bench process --backend <name> --in <in.wav> --out <out.wav>
 *                    [--json <stats.json>] [--format f32|s16]
 *                    [--level 0..1] [--vad 0..1] [--repeat N]
 *   ng_bench compare --a <a.wav> --b <b.wav> [--tol 1e-4]
 *
 * Output length always equals input length; the last partial frame is
 * zero-padded for processing and trimmed afterwards. No delay compensation
 * is applied -- delay is measured by the scoring script.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "rnnoise.h"
#include "rnnoise_wrapper.h"
#include "wav_io.h"

#ifndef NG_RNNOISE_COMMIT
#define NG_RNNOISE_COMMIT "unknown"
#endif

using ainoiceguard::kRNNoiseFrameSize;
using ainoiceguard::RNNoiseWrapper;
namespace wav = ainoiceguard::wav;

namespace {

constexpr int kSampleRate = 48000;

using Args = std::map<std::string, std::string>;

Args parseArgs(int argc, char** argv, int start) {
  Args a;
  for (int i = start; i < argc; i++) {
    std::string k = argv[i];
    if (k.rfind("--", 0) == 0 && i + 1 < argc) {
      a[k.substr(2)] = argv[++i];
    }
  }
  return a;
}

std::string get(const Args& a, const std::string& k, const std::string& def = "") {
  auto it = a.find(k);
  return it == a.end() ? def : it->second;
}

int usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  ng_bench process --backend rnnoise-raw|rnnoise-chain --in IN.wav --out OUT.wav\n"
               "                   [--json STATS.json] [--format f32|s16] [--level 0..1] [--vad 0..1]\n"
               "                   [--repeat N]  (N>1: first pass is warm-up, excluded from timing)\n"
               "  ng_bench compare --a A.wav --b B.wav [--tol 1e-4]\n");
  return 2;
}

/** Load a 48 kHz WAV and return mono samples (only mono input is accepted). */
bool loadMono(const std::string& path, std::vector<float>& mono) {
  wav::Audio a;
  std::string err = wav::read(path, a);
  if (!err.empty()) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return false;
  }
  if (a.sampleRate != kSampleRate) {
    std::fprintf(stderr, "error: %s is %d Hz; fixtures must be %d Hz\n",
                 path.c_str(), a.sampleRate, kSampleRate);
    return false;
  }
  if (a.channels != 1) {
    std::fprintf(stderr, "error: %s has %d channels; fixtures must be mono\n",
                 path.c_str(), a.channels);
    return false;
  }
  mono = std::move(a.samples);
  return true;
}

double percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  double idx = p / 100.0 * double(v.size() - 1);
  size_t lo = size_t(std::floor(idx));
  size_t hi = std::min(lo + 1, v.size() - 1);
  return v[lo] + (v[hi] - v[lo]) * (idx - double(lo));
}

std::string jsonEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '\\' || c == '"') o += '\\';
    o += c;
  }
  return o;
}

/* ───────────────────────── process ───────────────────────── */

int cmdProcess(const Args& args) {
  const std::string backend = get(args, "backend");
  const std::string in = get(args, "in");
  const std::string out = get(args, "out");
  const std::string json = get(args, "json");
  const std::string fmtName = get(args, "format", "f32");
  const float level = std::stof(get(args, "level", "1.0"));
  const float vad = std::stof(get(args, "vad", "0.5"));

  if (in.empty() || out.empty()) return usage();
  if (backend != "rnnoise-raw" && backend != "rnnoise-chain") {
    std::fprintf(stderr, "error: unknown backend '%s'\n", backend.c_str());
    return 2;
  }
  if (fmtName != "f32" && fmtName != "s16") {
    std::fprintf(stderr, "error: --format must be f32 or s16\n");
    return 2;
  }

  std::vector<float> input;
  if (!loadMono(in, input)) return 1;

  const int repeat = std::max(1, std::stoi(get(args, "repeat", "1")));

  const size_t n = input.size();
  const size_t frames = (n + kRNNoiseFrameSize - 1) / kRNNoiseFrameSize;
  std::vector<float> output;
  std::vector<double> frameUs;  /* timings of the measured passes */
  double firstFrameUs = 0.0;
  double vadSum = 0.0;

  /*
   * Each pass processes the whole file with fresh backend state, created once
   * and kept for the whole pass as in the app. With --repeat N > 1 the first
   * pass is a warm-up (CPU clocks, caches) and is excluded from the stats.
   * Output and VAD come from the last pass; passes are deterministic.
   */
  for (int pass = 0; pass < repeat; pass++) {
    const bool measured = (repeat == 1) || (pass > 0);
    output.assign(frames * kRNNoiseFrameSize, 0.0f);
    vadSum = 0.0;

    DenoiseState* raw = nullptr;
    RNNoiseWrapper chain;
    if (backend == "rnnoise-raw") {
      raw = rnnoise_create(nullptr);
    } else {
      if (!chain.init()) {
        std::fprintf(stderr, "error: RNNoise init failed\n");
        return 1;
      }
      chain.setSuppressionLevel(level);
      chain.setVadThreshold(vad);
    }

    float frame[kRNNoiseFrameSize];
    for (size_t f = 0; f < frames; f++) {
      const size_t off = f * kRNNoiseFrameSize;
      const size_t len = std::min(kRNNoiseFrameSize, n - off);
      std::memset(frame, 0, sizeof(frame));
      std::memcpy(frame, input.data() + off, len * sizeof(float));

      const auto t0 = std::chrono::steady_clock::now();
      float v;
      if (raw) {
        for (float& s : frame) s *= 32767.0f;
        v = rnnoise_process_frame(raw, frame, frame);
        for (float& s : frame) s *= (1.0f / 32767.0f);
      } else {
        v = chain.processFrame(frame);
      }
      const auto t1 = std::chrono::steady_clock::now();

      const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
      if (measured) {
        if (f == 0 && frameUs.empty()) firstFrameUs = us;
        frameUs.push_back(us);
      }
      vadSum += v;
      std::memcpy(output.data() + off, frame, sizeof(frame));
    }

    if (raw) rnnoise_destroy(raw);
  }
  output.resize(n);
  const size_t measuredPasses = size_t(repeat == 1 ? 1 : repeat - 1);

  std::string err = wav::writeMono(
      out, kSampleRate, output,
      fmtName == "s16" ? wav::OutFormat::Pcm16 : wav::OutFormat::Float32);
  if (!err.empty()) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }

  double total = 0.0, maxUs = 0.0;
  size_t overruns = 0;
  for (double u : frameUs) {
    total += u;
    maxUs = std::max(maxUs, u);
    if (u > 10000.0) overruns++;
  }
  const double audioSec = double(n) / kSampleRate;
  const double meanUs = frameUs.empty() ? 0.0 : total / double(frameUs.size());
  /* Percentiles exclude the very first measured frame (allocation/warm-up). */
  std::vector<double> steady(frameUs.begin() + (frameUs.size() > 1 ? 1 : 0), frameUs.end());
  const double measuredSec = audioSec * double(measuredPasses);

  char buf[2048];
  std::snprintf(
      buf, sizeof(buf),
      "{\n"
      "  \"tool\": \"ng_bench\",\n"
      "  \"backend\": \"%s\",\n"
      "  \"rnnoise_commit\": \"%s\",\n"
      "  \"input\": \"%s\",\n"
      "  \"output\": \"%s\",\n"
      "  \"sample_rate\": %d,\n"
      "  \"frame_size\": %zu,\n"
      "  \"samples\": %zu,\n"
      "  \"frames\": %zu,\n"
      "  \"audio_seconds\": %.6f,\n"
      "  \"params\": { \"level\": %.4f, \"vad_threshold\": %.4f },\n"
      "  \"passes\": { \"total\": %d, \"measured\": %zu },\n"
      "  \"mean_vad\": %.6f,\n"
      "  \"timing_us\": {\n"
      "    \"first_frame\": %.3f,\n"
      "    \"mean\": %.3f,\n"
      "    \"p50\": %.3f,\n"
      "    \"p95\": %.3f,\n"
      "    \"p99\": %.3f,\n"
      "    \"max\": %.3f,\n"
      "    \"overruns_over_10ms\": %zu\n"
      "  },\n"
      "  \"real_time_factor\": %.6f\n"
      "}\n",
      backend.c_str(), NG_RNNOISE_COMMIT, jsonEscape(in).c_str(),
      jsonEscape(out).c_str(), kSampleRate, kRNNoiseFrameSize, n, frames, audioSec,
      level, vad, repeat, measuredPasses, frames ? vadSum / double(frames) : 0.0,
      firstFrameUs, meanUs, percentile(steady, 50),
      percentile(steady, 95), percentile(steady, 99), maxUs, overruns,
      measuredSec > 0 ? (total / 1e6) / measuredSec : 0.0);

  if (!json.empty()) {
    std::FILE* jf = std::fopen(json.c_str(), "wb");
    if (!jf) {
      std::fprintf(stderr, "error: cannot write %s\n", json.c_str());
      return 1;
    }
    std::fputs(buf, jf);
    std::fclose(jf);
  } else {
    std::fputs(buf, stdout);
  }
  return 0;
}

/* ───────────────────────── compare ───────────────────────── */

int cmdCompare(const Args& args) {
  const std::string a = get(args, "a");
  const std::string b = get(args, "b");
  const double tol = std::stod(get(args, "tol", "1e-4"));
  if (a.empty() || b.empty()) return usage();

  std::vector<float> x, y;
  if (!loadMono(a, x) || !loadMono(b, y)) return 1;
  if (x.size() != y.size()) {
    std::printf("{ \"match\": false, \"reason\": \"length %zu vs %zu\" }\n",
                x.size(), y.size());
    return 1;
  }

  double sq = 0.0, maxAbs = 0.0;
  for (size_t i = 0; i < x.size(); i++) {
    double d = double(x[i]) - double(y[i]);
    sq += d * d;
    maxAbs = std::max(maxAbs, std::fabs(d));
  }
  const double rms = x.empty() ? 0.0 : std::sqrt(sq / double(x.size()));
  const bool match = rms <= tol;
  std::printf("{ \"match\": %s, \"rms_diff\": %.3e, \"max_abs_diff\": %.3e, \"tol\": %.1e }\n",
              match ? "true" : "false", rms, maxAbs, tol);
  return match ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  const std::string cmd = argv[1];
  const Args args = parseArgs(argc, argv, 2);
  if (cmd == "process") return cmdProcess(args);
  if (cmd == "compare") return cmdCompare(args);
  return usage();
}
