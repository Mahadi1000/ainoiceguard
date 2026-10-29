/**
 * Ainoiceguard N-API Native Addon
 *
 * Exposes the C++ AudioEngine to JavaScript via Node-API (N-API).
 * All heavy audio work stays in C++. JavaScript only calls:
 *   - getDevices()                -> list audio devices
 *   - start(inputIdx, outputIdx)  -> start noise cancellation
 *   - stop()                      -> stop noise cancellation
 *   - setNoiseLevel(level)        -> adjust suppression [0.0, 1.0]
 *   - getNoiseLevel()             -> read current suppression level
 *   - setVadThreshold(threshold)  -> adjust VAD gate threshold [0.0, 1.0]
 *   - getVadThreshold()           -> read current VAD threshold
 *   - setAgcEnabled(bool)         -> speech AGC on (default) / fixed +2 dB
 *   - getAgcEnabled()             -> read AGC state
 *   - isRunning()                 -> check engine state
 *   - getMetrics()                -> real-time audio metrics
 */

#include <napi.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "audio.h"
#include "portaudio.h"

namespace {

/* Single global engine instance. One engine per process is sufficient. */
static ainoiceguard::AudioEngine g_engine;

/**
 * getDevices() -> { inputs: [...], outputs: [...] }
 */
Napi::Value GetDevices(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  auto devices = ainoiceguard::AudioEngine::enumerateDevices();

  Napi::Array inputs = Napi::Array::New(env);
  Napi::Array outputs = Napi::Array::New(env);
  uint32_t inIdx = 0, outIdx = 0;

  for (const auto& d : devices) {
    if (d.maxInputChannels > 0) {
      Napi::Object obj = Napi::Object::New(env);
      obj.Set("index", Napi::Number::New(env, d.index));
      obj.Set("name", Napi::String::New(env, d.name));
      obj.Set("maxChannels", Napi::Number::New(env, d.maxInputChannels));
      obj.Set("defaultSampleRate", Napi::Number::New(env, d.defaultSampleRate));
      inputs.Set(inIdx++, obj);
    }
    if (d.maxOutputChannels > 0) {
      Napi::Object obj = Napi::Object::New(env);
      obj.Set("index", Napi::Number::New(env, d.index));
      obj.Set("name", Napi::String::New(env, d.name));
      obj.Set("maxChannels", Napi::Number::New(env, d.maxOutputChannels));
      obj.Set("defaultSampleRate", Napi::Number::New(env, d.defaultSampleRate));
      outputs.Set(outIdx++, obj);
    }
  }

  Napi::Object result = Napi::Object::New(env);
  result.Set("inputs", inputs);
  result.Set("outputs", outputs);
  return result;
}

/**
 * start(inputDeviceIndex, outputDeviceIndex) -> string
 */
Napi::Value Start(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  int inputIdx = -1;
  int outputIdx = -1;

  if (info.Length() >= 1 && info[0].IsNumber()) {
    inputIdx = info[0].As<Napi::Number>().Int32Value();
  }
  if (info.Length() >= 2 && info[1].IsNumber()) {
    outputIdx = info[1].As<Napi::Number>().Int32Value();
  }

  ainoiceguard::AudioConfig config;
  config.inputDeviceIndex = inputIdx;
  config.outputDeviceIndex = outputIdx;
  config.sampleRate = 48000.0;
  config.framesPerBuffer = ainoiceguard::kRNNoiseFrameSize;
  config.tryExclusiveMode = true;

  std::string err = g_engine.start(config);
  return Napi::String::New(env, err);
}

/**
 * stop() -> void
 */
void Stop(const Napi::CallbackInfo& /*info*/) { g_engine.stop(); }

/**
 * setNoiseLevel(level) -> void
 */
void SetNoiseLevel(const Napi::CallbackInfo& info) {
  if (info.Length() < 1 || !info[0].IsNumber()) return;
  float level = info[0].As<Napi::Number>().FloatValue();
  g_engine.setSuppressionLevel(level);
}

/**
 * getNoiseLevel() -> number
 */
Napi::Value GetNoiseLevel(const Napi::CallbackInfo& info) {
  return Napi::Number::New(info.Env(), g_engine.getSuppressionLevel());
}

/**
 * setVadThreshold(threshold) -> void
 */
void SetVadThreshold(const Napi::CallbackInfo& info) {
  if (info.Length() < 1 || !info[0].IsNumber()) return;
  float threshold = info[0].As<Napi::Number>().FloatValue();
  g_engine.setVadThreshold(threshold);
}

/**
 * getVadThreshold() -> number
 */
Napi::Value GetVadThreshold(const Napi::CallbackInfo& info) {
  return Napi::Number::New(info.Env(), g_engine.getVadThreshold());
}

/**
 * setAgcEnabled(enabled) -> void
 */
void SetAgcEnabled(const Napi::CallbackInfo& info) {
  if (info.Length() < 1 || !info[0].IsBoolean()) return;
  g_engine.setAgcEnabled(info[0].As<Napi::Boolean>().Value());
}

/**
 * getAgcEnabled() -> boolean
 */
Napi::Value GetAgcEnabled(const Napi::CallbackInfo& info) {
  return Napi::Boolean::New(info.Env(), g_engine.isAgcEnabled());
}

/**
 * isRunning() -> boolean
 */
Napi::Value IsRunning(const Napi::CallbackInfo& info) {
  return Napi::Boolean::New(info.Env(), g_engine.isRunning());
}

double num(const std::atomic<uint64_t>& a) {
  return static_cast<double>(a.load(std::memory_order_relaxed));
}
double num(const std::atomic<uint32_t>& a) {
  return static_cast<double>(a.load(std::memory_order_relaxed));
}

/** Name and host API ("MME", "Windows WASAPI", ...) of a PortAudio device. */
void SetDeviceInfo(Napi::Env env, Napi::Object& o, const char* prefix, int idx) {
  std::string p(prefix);
  o.Set(p + "Device", Napi::Number::New(env, idx));
  const PaDeviceInfo* d = idx >= 0 ? Pa_GetDeviceInfo(idx) : nullptr;
  const PaHostApiInfo* h = d ? Pa_GetHostApiInfo(d->hostApi) : nullptr;
  o.Set(p + "Name", d && d->name ? Napi::Value(Napi::String::New(env, d->name)) : env.Null());
  o.Set(p + "HostApi", h && h->name ? Napi::Value(Napi::String::New(env, h->name)) : env.Null());
}

/**
 * Engine health: what was opened, xruns, ring-buffer drops/fill, per-frame
 * processing time, estimated latency and recovery counters. All counters are
 * cumulative since the last start(). Lock-free reads.
 */
Napi::Object EngineMetrics(Napi::Env env) {
  using ainoiceguard::kFrameBudgetUs;
  using ainoiceguard::kProcBucketBoundsUs;
  using ainoiceguard::kProcBuckets;

  const auto& s = g_engine.stats();
  const bool running = g_engine.isRunning();
  const auto snap = g_engine.streamSnapshot();
  const double sr = snap.sampleRate > 0 ? snap.sampleRate : 48000.0;

  Napi::Object e = Napi::Object::New(env);
  e.Set("running", Napi::Boolean::New(env, running));

  /* Streams (device names need PortAudio initialised, i.e. while running). */
  Napi::Object streams = Napi::Object::New(env);
  if (running) {
    SetDeviceInfo(env, streams, "input", snap.inputDevice);
    SetDeviceInfo(env, streams, "output", snap.outputDevice);
  }
  streams.Set("inputExclusive", Napi::Boolean::New(env, snap.inputExclusive));
  streams.Set("outputExclusive", Napi::Boolean::New(env, snap.outputExclusive));
  streams.Set("sampleRate", Napi::Number::New(env, snap.sampleRate));
  streams.Set("ringCapacity", Napi::Number::New(env, double(snap.ringCapacity)));
  e.Set("streams", streams);

  Napi::Object sched = Napi::Object::New(env);
  sched.Set("mmcss", Napi::Boolean::New(env, snap.processingMmcss));
  sched.Set("wake", Napi::String::New(env, "event"));
  e.Set("scheduling", sched);

  Napi::Object xruns = Napi::Object::New(env);
  xruns.Set("inputUnderflow", Napi::Number::New(env, num(s.inputUnderflows)));
  xruns.Set("inputOverflow", Napi::Number::New(env, num(s.inputOverflows)));
  xruns.Set("outputUnderflow", Napi::Number::New(env, num(s.outputUnderflows)));
  xruns.Set("outputOverflow", Napi::Number::New(env, num(s.outputOverflows)));
  e.Set("xruns", xruns);

  Napi::Object callbacks = Napi::Object::New(env);
  callbacks.Set("capture", Napi::Number::New(env, num(s.captureCallbacks)));
  callbacks.Set("output", Napi::Number::New(env, num(s.outputCallbacks)));
  e.Set("callbacks", callbacks);

  const double fills = num(s.fillSamples);
  const double capMean = fills > 0 ? num(s.captureFillSum) / fills : 0.0;
  const double outMean = fills > 0 ? num(s.outputFillSum) / fills : 0.0;
  const uint32_t outMin = s.outputFillMin.load(std::memory_order_relaxed);

  Napi::Object ring = Napi::Object::New(env);
  ring.Set("captureFill", Napi::Number::New(env, double(running ? g_engine.captureFill() : 0)));
  ring.Set("outputFill", Napi::Number::New(env, double(running ? g_engine.outputFill() : 0)));
  ring.Set("captureFillMean", Napi::Number::New(env, capMean));
  ring.Set("captureFillMax", Napi::Number::New(env, num(s.captureFillMax)));
  ring.Set("outputFillMean", Napi::Number::New(env, outMean));
  ring.Set("outputFillMin", outMin == UINT32_MAX ? env.Null()
                                                 : Napi::Value(Napi::Number::New(env, outMin)));
  ring.Set("outputFillMax", Napi::Number::New(env, num(s.outputFillMax)));
  ring.Set("captureDroppedSamples", Napi::Number::New(env, num(s.captureDroppedSamples)));
  ring.Set("outputDroppedSamples", Napi::Number::New(env, num(s.outputDroppedSamples)));
  ring.Set("outputUnderrunEvents", Napi::Number::New(env, num(s.outputUnderrunEvents)));
  ring.Set("outputUnderrunSamples", Napi::Number::New(env, num(s.outputUnderrunSamples)));
  e.Set("ring", ring);

  const double frames = num(s.procFrames);
  Napi::Object proc = Napi::Object::New(env);
  proc.Set("frames", Napi::Number::New(env, frames));
  proc.Set("meanUs", Napi::Number::New(env, frames > 0 ? num(s.procTotalUs) / frames : 0.0));
  proc.Set("maxUs", Napi::Number::New(env, num(s.procMaxUs)));
  proc.Set("budgetUs", Napi::Number::New(env, kFrameBudgetUs));
  proc.Set("overruns", Napi::Number::New(env, num(s.procOverruns)));
  Napi::Array bounds = Napi::Array::New(env);
  Napi::Array counts = Napi::Array::New(env);
  for (size_t i = 0; i < kProcBuckets; i++) {
    bounds.Set(uint32_t(i), i < kProcBuckets - 1 ? Napi::Value(Napi::Number::New(env, kProcBucketBoundsUs[i]))
                                                  : env.Null()); /* null = open-ended */
    counts.Set(uint32_t(i), Napi::Number::New(env, num(s.procHist[i])));
  }
  proc.Set("histogramUpperUs", bounds);
  proc.Set("histogramCounts", counts);
  e.Set("processing", proc);

  /*
   * Estimated end-to-end latency: PortAudio-reported device latency on both
   * sides + mean time spent waiting in each ring + the backend's algorithmic
   * delay. A measured value comes from tools/ (see docs/phase3-live-metrics.md).
   */
  const bool outputOn = snap.outputDevice >= 0;
  const double capRingMs = capMean * 1000.0 / sr;
  const double outRingMs = outputOn ? outMean * 1000.0 / sr : 0.0;
  Napi::Object lat = Napi::Object::New(env);
  lat.Set("inputDeviceMs", Napi::Number::New(env, snap.inputLatencyMs));
  lat.Set("outputDeviceMs", Napi::Number::New(env, snap.outputLatencyMs));
  lat.Set("captureRingMs", Napi::Number::New(env, capRingMs));
  lat.Set("outputRingMs", Napi::Number::New(env, outRingMs));
  lat.Set("backendMs", Napi::Number::New(env, ainoiceguard::AudioEngine::kBackendDelayMs));
  lat.Set("estimatedTotalMs",
          Napi::Number::New(env, snap.inputLatencyMs + capRingMs +
                                     ainoiceguard::AudioEngine::kBackendDelayMs + outRingMs +
                                     (outputOn ? snap.outputLatencyMs : 0.0)));
  e.Set("latency", lat);

  Napi::Object rec = Napi::Object::New(env);
  rec.Set("restarts", Napi::Number::New(env, num(s.restarts)));
  rec.Set("restartFailures", Napi::Number::New(env, num(s.restartFailures)));
  rec.Set("deviceLossEvents", Napi::Number::New(env, num(s.deviceLossEvents)));
  e.Set("recovery", rec);

  return e;
}

/**
 * getMetrics() -> { inputRms, outputRms, vadProbability, gateGain,
 *                   framesProcessed, noiseFloor, engine: {...} }
 *
 * Returns a snapshot of real-time audio metrics. Lock-free atomic reads.
 * Call this from a polling interval (e.g. every 100ms) to animate the UI meter.
 * `engine` holds the health counters described in EngineMetrics().
 */
Napi::Value GetMetrics(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  const auto& m = g_engine.metrics();

  Napi::Object result = Napi::Object::New(env);
  result.Set("inputRms", Napi::Number::New(env,
      static_cast<double>(m.inputRms.load(std::memory_order_relaxed))));
  result.Set("outputRms", Napi::Number::New(env,
      static_cast<double>(m.outputRms.load(std::memory_order_relaxed))));
  result.Set("vadProbability", Napi::Number::New(env,
      static_cast<double>(m.vadProbability.load(std::memory_order_relaxed))));
  result.Set("gateGain", Napi::Number::New(env,
      static_cast<double>(m.currentGain.load(std::memory_order_relaxed))));
  result.Set("framesProcessed", Napi::Number::New(env,
      static_cast<double>(m.framesProcessed.load(std::memory_order_relaxed))));
  result.Set("noiseFloor", Napi::Number::New(env,
      static_cast<double>(m.noiseFloor.load(std::memory_order_relaxed))));
  result.Set("agcEnabled", Napi::Boolean::New(env, g_engine.isAgcEnabled()));
  result.Set("agcGainDb", Napi::Number::New(env,
      static_cast<double>(m.agcGainDb.load(std::memory_order_relaxed))));
  result.Set("limiterReductionDb", Napi::Number::New(env,
      static_cast<double>(m.limiterReductionDb.load(std::memory_order_relaxed))));
  result.Set("limitedFrames", Napi::Number::New(env,
      static_cast<double>(m.limitedFrames.load(std::memory_order_relaxed))));

  result.Set("engine", EngineMetrics(env));
  return result;
}

/**
 * Module initialization.
 */
Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("getDevices", Napi::Function::New(env, GetDevices));
  exports.Set("start", Napi::Function::New(env, Start));
  exports.Set("stop", Napi::Function::New(env, Stop));
  exports.Set("setNoiseLevel", Napi::Function::New(env, SetNoiseLevel));
  exports.Set("getNoiseLevel", Napi::Function::New(env, GetNoiseLevel));
  exports.Set("setVadThreshold", Napi::Function::New(env, SetVadThreshold));
  exports.Set("getVadThreshold", Napi::Function::New(env, GetVadThreshold));
  exports.Set("setAgcEnabled", Napi::Function::New(env, SetAgcEnabled));
  exports.Set("getAgcEnabled", Napi::Function::New(env, GetAgcEnabled));
  exports.Set("isRunning", Napi::Function::New(env, IsRunning));
  exports.Set("getMetrics", Napi::Function::New(env, GetMetrics));
  return exports;
}

NODE_API_MODULE(ainoiceguard, Init)

}  // anonymous namespace
