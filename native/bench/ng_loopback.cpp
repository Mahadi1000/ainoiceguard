/**
 * ng_loopback -- measure a playback -> capture path (e.g. VB-Cable).
 *
 * Plays 1 ms tone bursts on an output device and records an input device,
 * both in WASAPI shared mode (Windows) at 48 kHz mono, then reports:
 *   - latency of each burst (output DAC time -> input ADC time), median/min/max
 *   - bursts detected vs played (routing check: 0 = the path is not connected)
 *   - each device's clock vs the system clock (ppm) and their difference,
 *     i.e. the drift the engine's drift compensation must correct
 *
 * Usage:
 *   ng_loopback --out "CABLE Input" --in "CABLE Output" [--seconds 10]
 *               [--interval 0.25] [--threshold 0.02] [--json result.json]
 *   ng_loopback --list
 * Device arguments match a full id ("<hostApi>|<name>") or a name substring
 * (case-insensitive) on the preferred host API.
 *
 * Note: it does play audio. Use it on a virtual cable, or keep the volume low
 * when testing speakers -> microphone acoustically.
 */

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "loopback.h"
#include "portaudio.h"
#ifdef _WIN32
#include "pa_win_wasapi.h"
#endif

using namespace ainoiceguard::loopback;

namespace {

constexpr double kSr = 48000.0;
constexpr unsigned long kBlock = 480;

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower((unsigned char)c));
  return s;
}

std::string hostName(int dev) {
  const PaDeviceInfo* d = Pa_GetDeviceInfo(dev);
  const PaHostApiInfo* h = d ? Pa_GetHostApiInfo(d->hostApi) : nullptr;
  return h && h->name ? h->name : "";
}

std::string preferredHost() {
#ifdef _WIN32
  PaHostApiIndex w = Pa_HostApiTypeIdToHostApiIndex(paWASAPI);
  if (w >= 0) return Pa_GetHostApiInfo(w)->name;
#endif
  return Pa_GetHostApiInfo(Pa_GetDefaultHostApi())->name;
}

int findDevice(const std::string& spec, bool input) {
  const std::string pref = preferredHost();
  for (int i = 0; i < Pa_GetDeviceCount(); i++) {
    const PaDeviceInfo* d = Pa_GetDeviceInfo(i);
    if (!d || (input ? d->maxInputChannels : d->maxOutputChannels) <= 0) continue;
    const std::string id = hostName(i) + "|" + d->name;
    if (id == spec) return i;
  }
  for (int i = 0; i < Pa_GetDeviceCount(); i++) {
    const PaDeviceInfo* d = Pa_GetDeviceInfo(i);
    if (!d || (input ? d->maxInputChannels : d->maxOutputChannels) <= 0) continue;
    if (hostName(i) == pref && lower(d->name).find(lower(spec)) != std::string::npos) return i;
  }
  return -1;
}

struct Run {
  std::vector<float> burst;
  size_t interval = 0;      /* samples between bursts */
  size_t firstBurst = 0;    /* sample index of the first burst */
  /* Output side (written by the output callback only). */
  std::vector<Stamp> outStamps;
  std::atomic<size_t> outStampCount{0};
  size_t outPos = 0;
  /* Input side (written by the input callback only). */
  std::vector<float> rec;
  std::atomic<size_t> recCount{0};
  std::vector<Stamp> inStamps;
  std::atomic<size_t> inStampCount{0};
  std::atomic<uint64_t> outUnderflows{0}, inOverflows{0};
};

double stampTime(PaStream* s, double paTime) {
  /* Some host APIs report 0; fall back to the stream clock now. */
  return paTime > 0 ? paTime : Pa_GetStreamTime(s);
}

PaStream* g_out = nullptr;
PaStream* g_in = nullptr;

int outCb(const void*, void* output, unsigned long n, const PaStreamCallbackTimeInfo* ti,
          PaStreamCallbackFlags flags, void* user) {
  auto* r = static_cast<Run*>(user);
  auto* out = static_cast<float*>(output);
  const size_t c = r->outStampCount.load(std::memory_order_relaxed);
  if (c < r->outStamps.size()) {
    r->outStamps[c] = {r->outPos, stampTime(g_out, ti->outputBufferDacTime)};
    r->outStampCount.store(c + 1, std::memory_order_release);
  }
  for (unsigned long i = 0; i < n; i++) {
    const size_t pos = r->outPos + i;
    float v = 0.0f;
    if (pos >= r->firstBurst) {
      const size_t off = (pos - r->firstBurst) % r->interval;
      if (off < r->burst.size()) v = r->burst[off];
    }
    out[i] = v;
  }
  r->outPos += n;
  if (flags & paOutputUnderflow) r->outUnderflows.fetch_add(1, std::memory_order_relaxed);
  return paContinue;
}

int inCb(const void* input, void*, unsigned long n, const PaStreamCallbackTimeInfo* ti,
         PaStreamCallbackFlags flags, void* user) {
  auto* r = static_cast<Run*>(user);
  const size_t pos = r->recCount.load(std::memory_order_relaxed);
  const size_t c = r->inStampCount.load(std::memory_order_relaxed);
  if (c < r->inStamps.size()) {
    r->inStamps[c] = {pos, stampTime(g_in, ti->inputBufferAdcTime)};
    r->inStampCount.store(c + 1, std::memory_order_release);
  }
  const size_t room = r->rec.size() - pos;
  const size_t k = std::min<size_t>(n, room);
  if (input && k) std::memcpy(r->rec.data() + pos, input, k * sizeof(float));
  r->recCount.store(pos + k, std::memory_order_release);
  if (flags & paInputOverflow) r->inOverflows.fetch_add(1, std::memory_order_relaxed);
  return paContinue;
}

PaError openStream(PaStream** s, int dev, bool input, Run* run) {
  PaStreamParameters p{};
  p.device = dev;
  p.channelCount = 1;
  p.sampleFormat = paFloat32;
  const PaDeviceInfo* d = Pa_GetDeviceInfo(dev);
  p.suggestedLatency = input ? d->defaultLowInputLatency : d->defaultLowOutputLatency;
#ifdef _WIN32
  PaWasapiStreamInfo w{};
  if (Pa_GetHostApiInfo(d->hostApi)->type == paWASAPI) {
    w.size = sizeof(w);
    w.hostApiType = paWASAPI;
    w.version = 1;
    w.flags = paWinWasapiAutoConvert; /* shared mode, like the engine */
    p.hostApiSpecificStreamInfo = &w;
  }
#endif
  return input ? Pa_OpenStream(s, &p, nullptr, kSr, kBlock, paClipOff, inCb, run)
               : Pa_OpenStream(s, nullptr, &p, kSr, kBlock, paClipOff, outCb, run);
}

std::map<std::string, std::string> parse(int argc, char** argv) {
  std::map<std::string, std::string> a;
  for (int i = 1; i < argc; i++) {
    std::string k = argv[i];
    if (k.rfind("--", 0) != 0) continue;
    if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) a[k.substr(2)] = argv[++i];
    else a[k.substr(2)] = "1";
  }
  return a;
}

}  // namespace

int main(int argc, char** argv) {
  auto args = parse(argc, argv);
  if (Pa_Initialize() != paNoError) {
    std::fprintf(stderr, "Pa_Initialize failed\n");
    return 1;
  }

  if (args.count("list")) {
    const std::string pref = preferredHost();
    for (int i = 0; i < Pa_GetDeviceCount(); i++) {
      const PaDeviceInfo* d = Pa_GetDeviceInfo(i);
      if (!d || hostName(i) != pref) continue;
      std::printf("%s %s|%s\n", d->maxInputChannels > 0 ? "in " : "out", hostName(i).c_str(), d->name);
    }
    Pa_Terminate();
    return 0;
  }
  if (!args.count("out") || !args.count("in")) {
    std::fprintf(stderr, "usage: ng_loopback --out <device> --in <device> [--seconds 10] "
                         "[--interval 0.25] [--threshold 0.02] [--json file]\n       ng_loopback --list\n");
    Pa_Terminate();
    return 2;
  }

  const double seconds = std::stod(args.count("seconds") ? args["seconds"] : "10");
  const double interval = std::stod(args.count("interval") ? args["interval"] : "0.25");
  const float threshold = std::stof(args.count("threshold") ? args["threshold"] : "0.02");
  const int outDev = findDevice(args["out"], false);
  const int inDev = findDevice(args["in"], true);
  if (outDev < 0 || inDev < 0) {
    std::fprintf(stderr, "device not found: %s\n", outDev < 0 ? args["out"].c_str() : args["in"].c_str());
    Pa_Terminate();
    return 1;
  }

  Run run;
  run.burst = makeBurst(kSr);
  run.interval = size_t(interval * kSr);
  run.firstBurst = size_t(0.5 * kSr); /* let both streams settle first */
  const size_t maxBlocks = size_t((seconds + 2) * kSr / kBlock) + 64;
  run.outStamps.resize(maxBlocks);
  run.inStamps.resize(maxBlocks);
  run.rec.assign(size_t((seconds + 2) * kSr), 0.0f);

  PaError e = openStream(&g_in, inDev, true, &run);
  if (e == paNoError) e = openStream(&g_out, outDev, false, &run);
  if (e != paNoError) {
    std::fprintf(stderr, "open failed: %s\n", Pa_GetErrorText(e));
    Pa_Terminate();
    return 1;
  }
  Pa_StartStream(g_in);
  Pa_StartStream(g_out);
  std::this_thread::sleep_for(std::chrono::milliseconds(int(seconds * 1000)));
  Pa_StopStream(g_out);
  /* Give the tail of the last bursts time to arrive. */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  Pa_StopStream(g_in);

  const double inLat = Pa_GetStreamInfo(g_in)->inputLatency;
  const double outLat = Pa_GetStreamInfo(g_out)->outputLatency;
  Pa_CloseStream(g_out);
  Pa_CloseStream(g_in);

  std::vector<Stamp> outStamps(run.outStamps.begin(), run.outStamps.begin() + run.outStampCount.load());
  std::vector<Stamp> inStamps(run.inStamps.begin(), run.inStamps.begin() + run.inStampCount.load());
  std::vector<float> rec(run.rec.begin(), run.rec.begin() + run.recCount.load());

  std::vector<double> played;
  for (size_t s = run.firstBurst; s + run.burst.size() < run.outPos; s += run.interval) {
    played.push_back(timeOf(outStamps, s, kSr));
  }
  std::vector<double> recTimes;
  for (size_t o : detectOnsets(rec, threshold, run.interval / 2)) recTimes.push_back(timeOf(inStamps, o, kSr));
  const auto lat = matchLatencies(played, recTimes, interval * 0.9);

  double lo = 0, hi = 0;
  if (!lat.empty()) {
    lo = *std::min_element(lat.begin(), lat.end());
    hi = *std::max_element(lat.begin(), lat.end());
  }
  const double outPpm = clockPpm(outStamps, kSr), inPpm = clockPpm(inStamps, kSr);
  const std::string outName = hostName(outDev) + "|" + Pa_GetDeviceInfo(outDev)->name;
  const std::string inName = hostName(inDev) + "|" + Pa_GetDeviceInfo(inDev)->name;

  char buf[2048];
  std::snprintf(buf, sizeof(buf),
                "{\n  \"output\": \"%s\",\n  \"input\": \"%s\",\n  \"seconds\": %.1f,\n"
                "  \"bursts_played\": %zu,\n  \"bursts_detected\": %zu,\n"
                "  \"latency_ms\": { \"median\": %.2f, \"min\": %.2f, \"max\": %.2f },\n"
                "  \"reported_latency_ms\": { \"output\": %.1f, \"input\": %.1f },\n"
                "  \"clock_ppm\": { \"output\": %.1f, \"input\": %.1f, \"difference\": %.1f },\n"
                "  \"xruns\": { \"output_underflow\": %llu, \"input_overflow\": %llu }\n}\n",
                outName.c_str(), inName.c_str(), seconds, played.size(), lat.size(),
                median(lat) * 1000, lo * 1000, hi * 1000, outLat * 1000, inLat * 1000, outPpm, inPpm,
                outPpm - inPpm, (unsigned long long)run.outUnderflows.load(),
                (unsigned long long)run.inOverflows.load());
  std::fputs(buf, stdout);
  if (args.count("json")) {
    if (std::FILE* f = std::fopen(args["json"].c_str(), "wb")) {
      std::fputs(buf, f);
      std::fclose(f);
    }
  }
  Pa_Terminate();
  return lat.empty() ? 3 : 0; /* 3 = nothing detected: path not connected */
}
