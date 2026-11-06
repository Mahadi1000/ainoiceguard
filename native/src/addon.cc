/**
 * Ainoiceguard N-API Native Addon
 *
 * Exposes the C++ AudioEngine to JavaScript via Node-API (N-API).
 * All heavy audio work stays in C++. JavaScript only calls:
 *   - getDevices()                -> list audio devices (with stable ids)
 *   - start(input, output, opts)  -> start noise cancellation (ids or legacy indices)
 *   - onStatus(cb)                -> engine status events (device lost/restarted/...)
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

#include <utility>

#include "audio.h"

namespace {

/* Single global engine instance. One engine per process is sufficient. */
static ainoiceguard::AudioEngine g_engine;

/* Engine status events -> JS (set by onStatus; may be called from any thread). */
static Napi::ThreadSafeFunction g_statusTsfn;
static bool g_statusTsfnActive = false;

Napi::Object DeviceToJs(Napi::Env env, const ainoiceguard::DeviceInfo& d, bool input) {
  Napi::Object obj = Napi::Object::New(env);
  obj.Set("index", Napi::Number::New(env, d.index));
  obj.Set("id", Napi::String::New(env, ainoiceguard::deviceId(d)));
  obj.Set("name", Napi::String::New(env, d.name));
  obj.Set("hostApi", Napi::String::New(env, d.hostApi));
  obj.Set("preferred", Napi::Boolean::New(env, d.preferredHostApi));
  obj.Set("isDefault", Napi::Boolean::New(env, input ? d.isHostDefaultInput : d.isHostDefaultOutput));
  obj.Set("maxChannels", Napi::Number::New(env, input ? d.maxInputChannels : d.maxOutputChannels));
  obj.Set("defaultSampleRate", Napi::Number::New(env, d.defaultSampleRate));
  return obj;
}

/**
 * getDevices() -> { inputs: [...], outputs: [...], preferredHostApi }
 *
 * Each device: { index, id, name, hostApi, preferred, isDefault, maxChannels,
 * defaultSampleRate }. `id` ("<hostApi>|<name>") is stable across device
 * changes; `index` is not and is kept only for compatibility. `preferred`
 * marks the host API the engine uses by default (WASAPI on Windows).
 */
Napi::Value GetDevices(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  auto devices = ainoiceguard::AudioEngine::enumerateDevices();

  Napi::Array inputs = Napi::Array::New(env);
  Napi::Array outputs = Napi::Array::New(env);
  uint32_t inIdx = 0, outIdx = 0;
  std::string preferred;

  for (const auto& d : devices) {
    if (d.preferredHostApi) preferred = d.hostApi;
    if (d.maxInputChannels > 0) inputs.Set(inIdx++, DeviceToJs(env, d, true));
    if (d.maxOutputChannels > 0) outputs.Set(outIdx++, DeviceToJs(env, d, false));
  }

  Napi::Object result = Napi::Object::New(env);
  result.Set("inputs", inputs);
  result.Set("outputs", outputs);
  result.Set("preferredHostApi", Napi::String::New(env, preferred));
  return result;
}

/**
 * JS device argument -> DeviceSpec.
 *   undefined | null | "" | -1   -> default device (preferred host API)
 *   "mute" | "none" | -2         -> no device (output muted)
 *   "<hostApi>|<name>"           -> that device, by identity
 *   number >= 0                  -> PortAudio index (legacy; not stable)
 */
ainoiceguard::DeviceSpec SpecFromJs(const Napi::Value& v) {
  using ainoiceguard::DeviceSpec;
  if (v.IsNumber()) {
    const int i = v.As<Napi::Number>().Int32Value();
    if (i == -2) return DeviceSpec::none();
    if (i < 0) return DeviceSpec::def();
    return DeviceSpec::byIndex(i);
  }
  if (v.IsString()) {
    const std::string s = v.As<Napi::String>().Utf8Value();
    if (s.empty()) return DeviceSpec::def();
    if (s == "mute" || s == "none") return DeviceSpec::none();
    return DeviceSpec::byId(s);
  }
  return DeviceSpec::def();
}

/**
 * start(input, output, options?) -> string ("" on success, else an error)
 *   input/output: see SpecFromJs.  options: { exclusive?: boolean }
 *   (WASAPI exclusive mode; default false = shared).
 */
Napi::Value Start(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  ainoiceguard::AudioConfig config;
  if (info.Length() >= 1) config.input = SpecFromJs(info[0]);
  if (info.Length() >= 2) config.output = SpecFromJs(info[1]);
  if (config.input.kind == ainoiceguard::DeviceSpec::Kind::None) {
    return Napi::String::New(env, "An input device is required");
  }
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object opts = info[2].As<Napi::Object>();
    Napi::Value ex = opts.Get("exclusive").UnwrapOr(env.Undefined());
    if (ex.IsBoolean()) config.tryExclusiveMode = ex.As<Napi::Boolean>().Value();
  }
  config.sampleRate = 48000.0;
  config.framesPerBuffer = ainoiceguard::kRNNoiseFrameSize;

  std::string err = g_engine.start(config);
  return Napi::String::New(env, err);
}

/**
 * onStatus(callback(type, message)) -> void
 *   type: "device-lost" | "restarted" | "restart-failed" | "fallback".
 *   Called on the JS thread; events originate on the audio processing
 *   thread. Register once, before start() (replacing it while the engine is
 *   running is not synchronised). Does not keep the process alive.
 */
void OnStatus(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsFunction()) return;

  g_engine.setStatusCallback(nullptr);
  if (g_statusTsfnActive) {
    g_statusTsfn.Release();
    g_statusTsfnActive = false;
  }

  g_statusTsfn = Napi::ThreadSafeFunction::New(env, info[0].As<Napi::Function>(),
                                               "ainoiceguard-status", 0, 1);
  g_statusTsfn.Unref(env);
  g_statusTsfnActive = true;

  g_engine.setStatusCallback([](const std::string& type, const std::string& message) {
    auto* payload = new std::pair<std::string, std::string>(type, message);
    napi_status st = g_statusTsfn.NonBlockingCall(
        payload, [](Napi::Env env, Napi::Function cb, std::pair<std::string, std::string>* p) {
          cb.Call({Napi::String::New(env, p->first), Napi::String::New(env, p->second)});
          delete p;
        });
    if (st != napi_ok) delete payload; /* queue closing: drop the event */
  });
}

/** _debugForceDeviceLoss() -> void  (test hook: exercises the restart path) */
void DebugForceDeviceLoss(const Napi::CallbackInfo& /*info*/) {
  g_engine.debugForceDeviceLoss();
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

/**
 * Index, id, name and host API of an opened device. Uses the engine's own
 * record (no PortAudio call: a restart may be re-initialising PortAudio on
 * the processing thread). Null fields if unavailable at this instant.
 */
void SetDeviceInfo(Napi::Env env, Napi::Object& o, const char* prefix, int idx,
                   const ainoiceguard::AudioEngine::OpenedDevice* d) {
  std::string p(prefix);
  o.Set(p + "Device", Napi::Number::New(env, idx));
  auto str = [&](const std::string& s) {
    return d && !s.empty() ? Napi::Value(Napi::String::New(env, s)) : env.Null();
  };
  o.Set(p + "Id", str(d ? d->id : ""));
  o.Set(p + "Name", str(d ? d->name : ""));
  o.Set(p + "HostApi", str(d ? d->hostApi : ""));
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

  /* Streams (identity of what is open right now; null during a restart). */
  Napi::Object streams = Napi::Object::New(env);
  if (running) {
    ainoiceguard::AudioEngine::OpenedDevice in, out;
    const bool have = g_engine.openedDevices(in, out);
    SetDeviceInfo(env, streams, "input", snap.inputDevice, have ? &in : nullptr);
    SetDeviceInfo(env, streams, "output", snap.outputDevice,
                  have && snap.outputDevice >= 0 ? &out : nullptr);
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
  exports.Set("onStatus", Napi::Function::New(env, OnStatus));
  exports.Set("_debugForceDeviceLoss", Napi::Function::New(env, DebugForceDeviceLoss));

  /* On environment teardown: stop audio and release the status queue before
   * the TSFN is destroyed, so no event can target a dead environment. */
  env.AddCleanupHook([]() {
    g_engine.stop();  /* first: no processing thread may still emit events */
    g_engine.setStatusCallback(nullptr);
    if (g_statusTsfnActive) {
      g_statusTsfn.Release();
      g_statusTsfnActive = false;
    }
  });
  exports.Set("getMetrics", Napi::Function::New(env, GetMetrics));
  return exports;
}

NODE_API_MODULE(ainoiceguard, Init)

}  // anonymous namespace
