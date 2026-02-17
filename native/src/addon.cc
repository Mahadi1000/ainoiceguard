/**
 * NoiseGuard N-API Native Addon
 *
 * Exposes the C++ AudioEngine to JavaScript via Node-API (N-API).
 * All heavy audio work stays in C++. JavaScript only calls:
 *   - getDevices()                -> list audio devices
 *   - start(inputIdx, outputIdx)  -> start noise cancellation
 *   - stop()                      -> stop noise cancellation
 *   - setNoiseLevel(level)        -> adjust suppression [0.0, 1.0]
 *   - getNoiseLevel()             -> read current suppression level
 *   - isRunning()                 -> check engine state
 */

#include <napi.h>
#include "audio.h"

namespace {

/* Single global engine instance. One engine per process is sufficient. */
static noiseguard::AudioEngine g_engine;

/**
 * getDevices() -> { inputs: [...], outputs: [...] }
 *
 * Returns an object with two arrays listing available audio input/output devices.
 * Each device entry has: { index, name, maxChannels, defaultSampleRate }.
 */
Napi::Value GetDevices(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  auto devices = noiseguard::AudioEngine::enumerateDevices();

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
 * start(inputDeviceIndex: number, outputDeviceIndex: number) -> string
 *
 * Start the audio engine. Returns empty string on success, error message on failure.
 * Pass -1 for either index to use the system default device.
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

  noiseguard::AudioConfig config;
  config.inputDeviceIndex = inputIdx;
  config.outputDeviceIndex = outputIdx;
  config.sampleRate = 48000.0;
  config.framesPerBuffer = noiseguard::kRNNoiseFrameSize;
  config.tryExclusiveMode = true;

  std::string err = g_engine.start(config);
  return Napi::String::New(env, err);
}

/**
 * stop() -> void
 *
 * Stop the audio engine. Blocks until processing thread exits.
 */
void Stop(const Napi::CallbackInfo& /*info*/) { g_engine.stop(); }

/**
 * setNoiseLevel(level: number) -> void
 *
 * Set suppression level. 0.0 = passthrough, 1.0 = full suppression.
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
 * isRunning() -> boolean
 */
Napi::Value IsRunning(const Napi::CallbackInfo& info) {
  return Napi::Boolean::New(info.Env(), g_engine.isRunning());
}

/**
 * Module initialization -- called when require('noiseguard') loads the .node file.
 */
Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("getDevices", Napi::Function::New(env, GetDevices));
  exports.Set("start", Napi::Function::New(env, Start));
  exports.Set("stop", Napi::Function::New(env, Stop));
  exports.Set("setNoiseLevel", Napi::Function::New(env, SetNoiseLevel));
  exports.Set("getNoiseLevel", Napi::Function::New(env, GetNoiseLevel));
  exports.Set("isRunning", Napi::Function::New(env, IsRunning));
  return exports;
}

NODE_API_MODULE(noiseguard, Init)

}  // anonymous namespace
