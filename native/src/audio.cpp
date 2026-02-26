/**
 * AudioEngine implementation.
 *
 * Data flow:
 *   Mic -> captureCallback() -> captureRing_ -> processingLoop() -> RNNoise
 *       -> outputRing_ -> outputCallback() -> Speaker/VB-Cable
 *
 * Threading model:
 *   - Capture callback:    PortAudio's audio thread (real-time priority).
 *   - Output callback:     PortAudio's audio thread (real-time priority).
 *   - Processing loop:     Our own std::thread (elevated priority recommended).
 *   - start()/stop():      Called from Node.js main thread via N-API.
 */

#include "audio.h"

#include <chrono>
#include <cmath>
#include <cstring>

#include "portaudio.h"

#ifdef _WIN32
#include "pa_win_wasapi.h"
#endif

namespace ainoiceguard {

/*
 * Ring buffer capacity in samples.
 * 4096 samples @ 48kHz ~= 85ms -- enough to absorb scheduling jitter
 * without adding perceptible latency. Must be >> framesPerBuffer.
 */
static constexpr size_t kRingCapacity = 4096;

/* Max restart attempts before giving up. */
static constexpr int kMaxRestartAttempts = 5;

/* ───────────────────── Constructor / Destructor ───────────────────── */

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() { stop(); }

/* ───────────────────── Device Enumeration ───────────────────── */

std::vector<DeviceInfo> AudioEngine::enumerateDevices() {
  std::vector<DeviceInfo> devices;

  PaError err = Pa_Initialize();
  if (err != paNoError) return devices;

  int numDevices = Pa_GetDeviceCount();
  for (int i = 0; i < numDevices; i++) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
    if (!info) continue;

    DeviceInfo d;
    d.index = i;
    d.name = info->name ? info->name : "(unknown)";
    d.maxInputChannels = info->maxInputChannels;
    d.maxOutputChannels = info->maxOutputChannels;
    d.defaultSampleRate = info->defaultSampleRate;
    devices.push_back(d);
  }

  Pa_Terminate();
  return devices;
}

/* ───────────────────── Start / Stop ───────────────────── */

std::string AudioEngine::start(const AudioConfig& config) {
  if (running_.load(std::memory_order_acquire)) {
    return "Engine already running";
  }

  config_ = config;

  /* Initialize PortAudio. */
  PaError err = Pa_Initialize();
  if (err != paNoError) {
    return std::string("Pa_Initialize failed: ") + Pa_GetErrorText(err);
  }

  /* Allocate ring buffers. Done once here, never in callbacks. */
  captureRing_ = std::make_unique<RingBuffer>(kRingCapacity);
  outputRing_ = std::make_unique<RingBuffer>(kRingCapacity);

  /* Initialize RNNoise. */
  if (!rnnoise_.init()) {
    Pa_Terminate();
    return "RNNoise initialization failed";
  }

  /* Open PortAudio streams. */
  std::string openErr = openStreams();
  if (!openErr.empty()) {
    rnnoise_.destroy();
    Pa_Terminate();
    return openErr;
  }

  /* Start streams. */
  err = Pa_StartStream(captureStream_);
  if (err != paNoError) {
    closeStreams();
    rnnoise_.destroy();
    Pa_Terminate();
    return std::string("Failed to start capture stream: ") + Pa_GetErrorText(err);
  }

  /* Output stream is optional (outputDeviceIndex == -2 => mute). */
  if (outputStream_) {
    err = Pa_StartStream(outputStream_);
    if (err != paNoError) {
      Pa_StopStream(captureStream_);
      closeStreams();
      rnnoise_.destroy();
      Pa_Terminate();
      return std::string("Failed to start output stream: ") + Pa_GetErrorText(err);
    }
  }

  /* Launch processing thread. */
  running_.store(true, std::memory_order_release);
  processingThread_ = std::thread(&AudioEngine::processingLoop, this);

  return "";  /* Success */
}

void AudioEngine::stop() {
  if (!running_.load(std::memory_order_acquire)) return;

  /* Signal processing thread to exit. */
  running_.store(false, std::memory_order_release);

  /* Wait for processing thread to finish. */
  if (processingThread_.joinable()) {
    processingThread_.join();
  }

  /* Stop and close streams. */
  if (captureStream_) Pa_StopStream(captureStream_);
  if (outputStream_) Pa_StopStream(outputStream_);
  closeStreams();

  /* Cleanup. */
  rnnoise_.destroy();
  captureRing_.reset();
  outputRing_.reset();

  Pa_Terminate();
}

/* ───────────────────── Stream Setup ───────────────────── */

std::string AudioEngine::openStreams() {
  PaError err;

  /* Resolve device indices. -1 means use default. */
  int inputIdx = config_.inputDeviceIndex;
  int outputIdx = config_.outputDeviceIndex;

  const bool outputEnabled = (outputIdx != -2);
  if (inputIdx < 0) inputIdx = Pa_GetDefaultInputDevice();
  if (outputEnabled && outputIdx < 0) outputIdx = Pa_GetDefaultOutputDevice();

  if (inputIdx == paNoDevice) return "No input device available";
  if (outputEnabled && outputIdx == paNoDevice) return "No output device available";

  /* ── Capture stream parameters ── */
  PaStreamParameters inputParams;
  inputParams.device = inputIdx;
  inputParams.channelCount = 1;  /* Mono -- RNNoise is mono only. */
  inputParams.sampleFormat = paFloat32;
  inputParams.suggestedLatency =
      Pa_GetDeviceInfo(inputIdx)->defaultLowInputLatency;
  inputParams.hostApiSpecificStreamInfo = nullptr;

  /* ── Output stream parameters (optional) ── */
  PaStreamParameters outputParams;
  if (outputEnabled) {
    outputParams.device = outputIdx;
    outputParams.channelCount = 1;  /* Mono output. */
    outputParams.sampleFormat = paFloat32;
    outputParams.suggestedLatency =
        Pa_GetDeviceInfo(outputIdx)->defaultLowOutputLatency;
    outputParams.hostApiSpecificStreamInfo = nullptr;
  }

#ifdef _WIN32
  /*
   * WASAPI-specific: attempt exclusive mode for lowest latency.
   * In exclusive mode, we get direct access to the hardware buffer.
   * If the device is busy, PortAudio falls back to shared mode.
   */
  PaWasapiStreamInfo wasapiInputInfo;
  PaWasapiStreamInfo wasapiOutputInfo;

  if (config_.tryExclusiveMode) {
    const PaHostApiInfo* wasapiInfo = nullptr;
    for (PaHostApiIndex i = 0; i < Pa_GetHostApiCount(); i++) {
      const PaHostApiInfo* api = Pa_GetHostApiInfo(i);
      if (api && api->type == paWASAPI) {
        wasapiInfo = api;
        break;
      }
    }

    if (wasapiInfo) {
      memset(&wasapiInputInfo, 0, sizeof(wasapiInputInfo));
      wasapiInputInfo.size = sizeof(PaWasapiStreamInfo);
      wasapiInputInfo.hostApiType = paWASAPI;
      wasapiInputInfo.version = 1;
      wasapiInputInfo.flags = paWinWasapiExclusive | paWinWasapiThreadPriority;
      wasapiInputInfo.threadPriority = eThreadPriorityProAudio;
      inputParams.hostApiSpecificStreamInfo = &wasapiInputInfo;

      if (outputEnabled) {
        memset(&wasapiOutputInfo, 0, sizeof(wasapiOutputInfo));
        wasapiOutputInfo.size = sizeof(PaWasapiStreamInfo);
        wasapiOutputInfo.hostApiType = paWASAPI;
        wasapiOutputInfo.version = 1;
        wasapiOutputInfo.flags = paWinWasapiExclusive | paWinWasapiThreadPriority;
        wasapiOutputInfo.threadPriority = eThreadPriorityProAudio;
        outputParams.hostApiSpecificStreamInfo = &wasapiOutputInfo;
      }
    }
  }
#endif

  /*
   * Open separate input and output streams.
   * Using separate streams is more robust: if one device disconnects,
   * we can detect and restart independently.
   */
  err = Pa_OpenStream(&captureStream_, &inputParams, nullptr,
                      config_.sampleRate, config_.framesPerBuffer,
                      paClipOff,
                      captureCallback, this);

  if (err != paNoError) {
#ifdef _WIN32
    /*
     * Exclusive mode often fails if another app has the device.
     * Retry without exclusive mode (shared/loopback).
     */
    if (config_.tryExclusiveMode) {
      inputParams.hostApiSpecificStreamInfo = nullptr;
      err = Pa_OpenStream(&captureStream_, &inputParams, nullptr,
                          config_.sampleRate, config_.framesPerBuffer,
                          paClipOff, captureCallback, this);
    }
#endif
    if (err != paNoError) {
      return std::string("Failed to open capture stream: ") +
             Pa_GetErrorText(err);
    }
  }

  if (!outputEnabled) {
    outputStream_ = nullptr;
    return ""; /* Success: capture-only (mute output) */
  }

  err = Pa_OpenStream(&outputStream_, nullptr, &outputParams,
                      config_.sampleRate, config_.framesPerBuffer,
                      paClipOff,
                      outputCallback, this);

  if (err != paNoError) {
#ifdef _WIN32
    if (config_.tryExclusiveMode) {
      outputParams.hostApiSpecificStreamInfo = nullptr;
      err = Pa_OpenStream(&outputStream_, nullptr, &outputParams,
                          config_.sampleRate, config_.framesPerBuffer,
                          paClipOff, outputCallback, this);
    }
#endif
    if (err != paNoError) {
      Pa_CloseStream(captureStream_);
      captureStream_ = nullptr;
      return std::string("Failed to open output stream: ") +
             Pa_GetErrorText(err);
    }
  }

  return "";  /* Success */
}

void AudioEngine::closeStreams() {
  if (captureStream_) {
    Pa_CloseStream(captureStream_);
    captureStream_ = nullptr;
  }
  if (outputStream_) {
    Pa_CloseStream(outputStream_);
    outputStream_ = nullptr;
  }
}

/* ───────────────────── Capture Callback (REAL-TIME) ───────────────────── */

int AudioEngine::captureCallback(const void* input, void* /*output*/,
                                 unsigned long frameCount,
                                 const PaStreamCallbackTimeInfo* /*timeInfo*/,
                                 PaStreamCallbackFlags statusFlags,
                                 void* userData) {
  /*
   * REAL-TIME SAFE: This runs on PortAudio's high-priority audio thread.
   * Absolutely NO allocations, NO locks, NO system calls here.
   * We only write to the lock-free ring buffer.
   */
  auto* engine = static_cast<AudioEngine*>(userData);

  if (!input || !engine->running_.load(std::memory_order_relaxed)) {
    return paContinue;
  }

  const auto* samples = static_cast<const float*>(input);

  /*
   * Write captured samples to ring buffer.
   * If the ring buffer is full, samples are silently dropped.
   * This is intentional: in real-time audio, dropping frames is
   * better than blocking or introducing unbounded latency.
   */
  engine->captureRing_->write(samples, frameCount);

  /* Detect device issues via statusFlags. */
  if (statusFlags & 0x00000001 /* paInputUnderflow */ ||
      statusFlags & 0x00000002 /* paInputOverflow */) {
    engine->shouldRestart_.store(true, std::memory_order_relaxed);
  }

  return paContinue;
}

/* ───────────────────── Output Callback (REAL-TIME) ───────────────────── */

int AudioEngine::outputCallback(const void* /*input*/, void* output,
                                unsigned long frameCount,
                                const PaStreamCallbackTimeInfo* /*timeInfo*/,
                                PaStreamCallbackFlags statusFlags,
                                void* userData) {
  /*
   * REAL-TIME SAFE: Same rules as captureCallback.
   * Read processed samples from the output ring buffer.
   * If not enough data is available, output silence (zero-fill).
   */
  auto* engine = static_cast<AudioEngine*>(userData);
  auto* out = static_cast<float*>(output);

  if (!engine->running_.load(std::memory_order_relaxed)) {
    memset(out, 0, frameCount * sizeof(float));
    return paContinue;
  }

  size_t read = engine->outputRing_->read(out, frameCount);

  /* Zero-fill remainder if underrun (not enough processed data yet). */
  if (read < frameCount) {
    memset(out + read, 0, (frameCount - read) * sizeof(float));
  }

  /* Detect output issues. */
  if (statusFlags & 0x00000004 /* paOutputUnderflow */ ||
      statusFlags & 0x00000008 /* paOutputOverflow */) {
    engine->shouldRestart_.store(true, std::memory_order_relaxed);
  }

  return paContinue;
}

/* ───────────────────── Processing Thread ───────────────────── */

void AudioEngine::processingLoop() {
  /*
   * This thread reads from captureRing_, processes through RNNoise,
   * and writes to outputRing_. It runs at slightly below real-time
   * priority (PortAudio callbacks are higher priority).
   *
   * We process in chunks of kRNNoiseFrameSize (480 samples = 10ms).
   */
  float frame[kRNNoiseFrameSize];

  while (running_.load(std::memory_order_acquire)) {
    /* Check if we have a full RNNoise frame available. */
    if (captureRing_->available_read() >= kRNNoiseFrameSize) {
      captureRing_->read(frame, kRNNoiseFrameSize);

      /* Run noise suppression. */
      rnnoise_.processFrame(frame);

      /* If output is disabled, discard processed audio (no monitoring). */
      if (outputStream_) {
        outputRing_->write(frame, kRNNoiseFrameSize);
      }
    } else {
      /*
       * Not enough data yet. Sleep briefly to avoid spinning at 100% CPU.
       * 0.5ms sleep is fine: at 48kHz, a 480-sample frame arrives every 10ms,
       * so we'll check ~10 times per frame period. The ring buffer smooths
       * any scheduling jitter.
       */
      std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    /* Handle device disconnect / restart. */
    if (shouldRestart_.load(std::memory_order_relaxed)) {
      shouldRestart_.store(false, std::memory_order_relaxed);
      attemptRestart();
    }
  }
}

/* ───────────────────── Auto-Restart ───────────────────── */

void AudioEngine::attemptRestart() {
  if (statusCallback_) {
    statusCallback_("Device issue detected, attempting restart...");
  }

  for (int attempt = 0; attempt < kMaxRestartAttempts; attempt++) {
    /* Exponential backoff: 100ms, 200ms, 400ms, 800ms, 1600ms */
    std::this_thread::sleep_for(
        std::chrono::milliseconds(100 * (1 << attempt)));

    if (!running_.load(std::memory_order_acquire)) return;

    /* Stop current streams. */
    if (captureStream_) Pa_StopStream(captureStream_);
    if (outputStream_) Pa_StopStream(outputStream_);
    closeStreams();

    /* Try to reopen. */
    std::string err = openStreams();
    if (!err.empty()) continue;

    PaError e1 = Pa_StartStream(captureStream_);
    if (e1 != paNoError) {
      closeStreams();
      continue;
    }

    if (outputStream_) {
      PaError e2 = Pa_StartStream(outputStream_);
      if (e2 != paNoError) {
        Pa_StopStream(captureStream_);
        closeStreams();
        continue;
      }
    }

    if (statusCallback_) {
      statusCallback_("Audio engine restarted successfully");
    }
    return;
  }

  if (statusCallback_) {
    statusCallback_("Failed to restart audio engine after multiple attempts");
  }
}

/* ───────────────────── Level Control ───────────────────── */

void AudioEngine::setSuppressionLevel(float level) {
  rnnoise_.setSuppressionLevel(level);
}

float AudioEngine::getSuppressionLevel() const {
  return rnnoise_.getSuppressionLevel();
}

void AudioEngine::setStatusCallback(StatusCallback cb) {
  statusCallback_ = std::move(cb);
}

void AudioEngine::setVadThreshold(float threshold) {
  rnnoise_.setVadThreshold(threshold);
}

float AudioEngine::getVadThreshold() const {
  return rnnoise_.getVadThreshold();
}

}  // namespace ainoiceguard
