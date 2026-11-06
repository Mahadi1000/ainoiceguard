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
#include <mutex>

#include "portaudio.h"

#ifdef _WIN32
#include "pa_win_wasapi.h"
#include <avrt.h> /* MMCSS: AvSetMmThreadCharacteristicsW */
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

/*
 * Device-loss watchdog: checked every 250 ms from the processing thread.
 * A capture stream whose callback has not run for 1 s is treated as lost
 * (WASAPI/MME may stop calling back without reporting an error).
 * After a failed restart the watchdog fires again, so recovery keeps being
 * retried (e.g. until a USB headset is plugged back in).
 */
static constexpr int64_t kWatchdogIntervalMs = 250;
static constexpr int64_t kCaptureStallMs = 1000;

/* Max time the processing thread blocks waiting for audio before re-checking
 * the running flag and watchdog. Frames normally arrive every 10 ms. */
static constexpr unsigned kWakeTimeoutMs = 20;

static int64_t steadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/* ───────────────────── Constructor / Destructor ───────────────────── */

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() { stop(); }

/* ───────────────────── Device Enumeration ───────────────────── */

/*
 * Pa_Initialize/Pa_Terminate and the device list are not thread-safe. They
 * are touched from the N-API thread (getDevices, getMetrics) and from the
 * processing thread (automatic restart), so all of it is serialised here.
 * Never taken in the real-time callbacks.
 */
static std::mutex& paMutex() {
  static std::mutex m;
  return m;
}

/* Preferred host API name; PortAudio must be initialised. */
static std::string preferredHostApiLocked() {
#ifdef _WIN32
  /* WASAPI: lower latency than MME (Phase 3: ~9 ms) and full device names. */
  PaHostApiIndex wasapi = Pa_HostApiTypeIdToHostApiIndex(paWASAPI);
  if (wasapi >= 0) {
    const PaHostApiInfo* h = Pa_GetHostApiInfo(wasapi);
    if (h && h->name && h->deviceCount > 0) return h->name;
  }
#endif
  const PaHostApiInfo* h = Pa_GetHostApiInfo(Pa_GetDefaultHostApi());
  return h && h->name ? h->name : "";
}

/* Snapshot of all devices; PortAudio must be initialised. */
static std::vector<DeviceInfo> collectDevicesLocked() {
  std::vector<DeviceInfo> devices;
  const std::string preferred = preferredHostApiLocked();
  const int numDevices = Pa_GetDeviceCount();
  for (int i = 0; i < numDevices; i++) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
    if (!info) continue;
    const PaHostApiInfo* h = Pa_GetHostApiInfo(info->hostApi);

    DeviceInfo d;
    d.index = i;
    d.name = info->name ? info->name : "(unknown)";
    d.hostApi = h && h->name ? h->name : "";
    d.maxInputChannels = info->maxInputChannels;
    d.maxOutputChannels = info->maxOutputChannels;
    d.isHostDefaultInput = h && h->defaultInputDevice == i;
    d.isHostDefaultOutput = h && h->defaultOutputDevice == i;
    d.defaultSampleRate = info->defaultSampleRate;
    d.preferredHostApi = d.hostApi == preferred;
    devices.push_back(d);
  }
  return devices;
}

std::vector<DeviceInfo> AudioEngine::enumerateDevices() {
  std::lock_guard<std::mutex> lock(paMutex());
  if (Pa_Initialize() != paNoError) return {};
  auto devices = collectDevicesLocked();
  Pa_Terminate();
  return devices;
}

std::string AudioEngine::preferredHostApi() {
  std::lock_guard<std::mutex> lock(paMutex());
  if (Pa_Initialize() != paNoError) return "";
  auto name = preferredHostApiLocked();
  Pa_Terminate();
  return name;
}

bool AudioEngine::openedDevices(OpenedDevice& in, OpenedDevice& out) const {
  /* try_lock: never stall the UI behind a restart (which holds the mutex). */
  std::unique_lock<std::mutex> lock(paMutex(), std::try_to_lock);
  if (!lock.owns_lock()) return false;
  in = openedIn_;
  out = openedOut_;
  return true;
}

/* ───────────────────── Start / Stop ───────────────────── */

std::string AudioEngine::start(const AudioConfig& config) {
  if (running_.load(std::memory_order_acquire)) {
    return "Engine already running";
  }

  config_ = config;

  /* Fresh counters for this session (no audio thread is running yet). */
  stats_.reset();
  outputPrimed_.store(false, std::memory_order_relaxed);
  forceDeviceLoss_.store(false, std::memory_order_relaxed);
  inOutage_ = false;

  /* Serialise with getDevices()/getMetrics() device lookups (see paMutex). */
  std::lock_guard<std::mutex> lock(paMutex());

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

  /* Output stream is optional (config.output == none => mute). */
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

  /* The processing thread (and any restart) has exited. */
  std::lock_guard<std::mutex> lock(paMutex());

  /* Stop and close streams. */
  if (captureStream_) Pa_StopStream(captureStream_);
  if (outputStream_) Pa_StopStream(outputStream_);
  closeStreams();
  openedIn_ = {};
  openedOut_ = {};

  /* Cleanup. */
  rnnoise_.destroy();
  captureRing_.reset();
  outputRing_.reset();

  Pa_Terminate();
}

/* ───────────────────── Stream Setup ───────────────────── */

std::string AudioEngine::openStreams() {
  PaError err;

  /*
   * Resolve devices by identity against the current device list (never a
   * cached index): after a restart PortAudio has been re-initialised, so the
   * list reflects devices that were added or removed meanwhile.
   */
  const auto devices = collectDevicesLocked();
  const std::string preferred = preferredHostApiLocked();
  const bool outputEnabled = config_.output.kind != DeviceSpec::Kind::None;
  const Resolved rin = resolveDevice(devices, config_.input, true, preferred);
  const Resolved rout = outputEnabled ? resolveDevice(devices, config_.output, false, preferred)
                                      : Resolved{};
  const int inputIdx = rin.index;
  const int outputIdx = rout.index;

  if (inputIdx < 0) return "No input device available";
  if (outputEnabled && outputIdx < 0) return "No output device available";

  auto describe = [&](int idx, OpenedDevice& o) {
    o = {};
    for (const auto& d : devices) {
      if (d.index == idx) {
        o.id = deviceId(d);
        o.name = d.name;
        o.hostApi = d.hostApi;
      }
    }
  };
  describe(inputIdx, openedIn_);
  if (outputEnabled) describe(outputIdx, openedOut_); else openedOut_ = {};

  if (statusCallback_ && (rin.fellBack || rout.fellBack)) {
    std::string msg = "Selected ";
    msg += rin.fellBack ? "microphone" : "output";
    if (rin.fellBack && rout.fellBack) msg = "Selected microphone and output";
    msg += " not found; using the default device instead";
    statusCallback_("fallback", msg);
  }

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
   * WASAPI stream options, only for devices that ARE on WASAPI (attaching
   * WASAPI info to an MME device made every open fail once and retry).
   *   shared (default): AutoConvert, so devices whose mix format is not
   *     48 kHz mono float (e.g. 44.1 kHz) are converted by Windows instead of
   *     failing to open.
   *   exclusive (opt-in): lowest latency, but locks the device; falls back
   *     to shared below if it cannot be opened.
   */
  PaWasapiStreamInfo wasapiInputInfo;
  PaWasapiStreamInfo wasapiOutputInfo;
  auto isWasapi = [](int idx) {
    const PaDeviceInfo* d = Pa_GetDeviceInfo(idx);
    const PaHostApiInfo* h = d ? Pa_GetHostApiInfo(d->hostApi) : nullptr;
    return h && h->type == paWASAPI;
  };
  auto fill = [&](PaWasapiStreamInfo& w, bool exclusive) {
    memset(&w, 0, sizeof(w));
    w.size = sizeof(PaWasapiStreamInfo);
    w.hostApiType = paWASAPI;
    w.version = 1;
    w.flags = exclusive ? (paWinWasapiExclusive | paWinWasapiThreadPriority)
                        : paWinWasapiAutoConvert;
    w.threadPriority = eThreadPriorityProAudio;
  };
  if (isWasapi(inputIdx)) {
    fill(wasapiInputInfo, config_.tryExclusiveMode);
    inputParams.hostApiSpecificStreamInfo = &wasapiInputInfo;
  }
  if (outputEnabled && isWasapi(outputIdx)) {
    fill(wasapiOutputInfo, config_.tryExclusiveMode);
    outputParams.hostApiSpecificStreamInfo = &wasapiOutputInfo;
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
  bool inputExclusive = err == paNoError && config_.tryExclusiveMode &&
                        inputParams.hostApiSpecificStreamInfo != nullptr;

  if (err != paNoError) {
#ifdef _WIN32
    /*
     * Exclusive mode often fails if another app has the device (or its
     * format is not supported). Retry in WASAPI shared mode.
     */
    if (config_.tryExclusiveMode && inputParams.hostApiSpecificStreamInfo) {
      fill(wasapiInputInfo, false);
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
    captureStreamSnapshot(inputIdx, -1, inputExclusive, false);
    return ""; /* Success: capture-only (mute output) */
  }

  err = Pa_OpenStream(&outputStream_, nullptr, &outputParams,
                      config_.sampleRate, config_.framesPerBuffer,
                      paClipOff,
                      outputCallback, this);
  bool outputExclusive = err == paNoError && config_.tryExclusiveMode &&
                         outputParams.hostApiSpecificStreamInfo != nullptr;

  if (err != paNoError) {
#ifdef _WIN32
    if (config_.tryExclusiveMode && outputParams.hostApiSpecificStreamInfo) {
      fill(wasapiOutputInfo, false);
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

  captureStreamSnapshot(inputIdx, outputIdx, inputExclusive, outputExclusive);
  return "";  /* Success */
}

void AudioEngine::captureStreamSnapshot(int inputIdx, int outputIdx, bool inExcl,
                                        bool outExcl) {
  snapInputDevice_.store(inputIdx, std::memory_order_relaxed);
  snapOutputDevice_.store(outputIdx, std::memory_order_relaxed);
  snapInputExclusive_.store(inExcl, std::memory_order_relaxed);
  snapOutputExclusive_.store(outExcl, std::memory_order_relaxed);

  const PaStreamInfo* in = captureStream_ ? Pa_GetStreamInfo(captureStream_) : nullptr;
  const PaStreamInfo* out = outputStream_ ? Pa_GetStreamInfo(outputStream_) : nullptr;
  snapInputLatencyMs_.store(in ? in->inputLatency * 1000.0 : 0.0, std::memory_order_relaxed);
  snapOutputLatencyMs_.store(out ? out->outputLatency * 1000.0 : 0.0,
                             std::memory_order_relaxed);
}

StreamSnapshot AudioEngine::streamSnapshot() const {
  StreamSnapshot s;
  s.inputDevice = snapInputDevice_.load(std::memory_order_relaxed);
  s.outputDevice = snapOutputDevice_.load(std::memory_order_relaxed);
  s.inputExclusive = snapInputExclusive_.load(std::memory_order_relaxed);
  s.outputExclusive = snapOutputExclusive_.load(std::memory_order_relaxed);
  s.sampleRate = config_.sampleRate;
  s.inputLatencyMs = snapInputLatencyMs_.load(std::memory_order_relaxed);
  s.outputLatencyMs = snapOutputLatencyMs_.load(std::memory_order_relaxed);
  s.ringCapacity = captureRing_ ? captureRing_->capacity() : 0;
  s.processingMmcss = mmcssActive_.load(std::memory_order_relaxed);
  return s;
}

size_t AudioEngine::captureFill() const {
  return captureRing_ ? captureRing_->available_read() : 0;
}

size_t AudioEngine::outputFill() const {
  return outputRing_ ? outputRing_->available_read() : 0;
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
  EngineStats& st = engine->stats_;
  bump(st.captureCallbacks);

  /*
   * Write captured samples to ring buffer.
   * If the ring buffer is full, samples are dropped -- in real-time audio,
   * dropping is better than blocking. The drop is counted, not silent.
   */
  const size_t written = engine->captureRing_->write(samples, frameCount);
  if (written < frameCount) bump(st.captureDroppedSamples, frameCount - written);

  /* Wake the processing thread (non-blocking; see wake_event.h). */
  engine->wake_.signal();

  /*
   * xruns are counted only. They are normal transient events (scheduling
   * jitter); restarting the streams on them caused 0.1-3.1 s outages.
   * Real device loss is detected by the watchdog in the processing thread.
   */
  if (statusFlags & paInputUnderflow) bump(st.inputUnderflows);
  if (statusFlags & paInputOverflow) bump(st.inputOverflows);

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

  EngineStats& st = engine->stats_;
  bump(st.outputCallbacks);

  size_t read = engine->outputRing_->read(out, frameCount);

  /* Zero-fill remainder if underrun (not enough processed data yet). */
  if (read < frameCount) {
    memset(out + read, 0, (frameCount - read) * sizeof(float));
    /* Before the first processed audio arrives this is just start-up priming. */
    if (engine->outputPrimed_.load(std::memory_order_relaxed)) {
      bump(st.outputUnderrunEvents);
      bump(st.outputUnderrunSamples, frameCount - read);
    }
  }
  if (read > 0) engine->outputPrimed_.store(true, std::memory_order_relaxed);

  /* Counted only -- see captureCallback. */
  if (statusFlags & paOutputUnderflow) bump(st.outputUnderflows);
  if (statusFlags & paOutputOverflow) bump(st.outputOverflows);

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

#ifdef _WIN32
  /*
   * Register with MMCSS ("Pro Audio") so the scheduler treats this thread as
   * real-time audio. Phase 3 measured >100 ms gaps for the former
   * normal-priority thread (dropped mic audio). Best effort: on failure the
   * thread simply keeps normal priority.
   */
  DWORD mmcssTask = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTask);
  if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
  mmcssActive_.store(mmcss != nullptr, std::memory_order_relaxed);
#endif

  wdLastCaptureCallbacks_ = stats_.captureCallbacks.load(std::memory_order_relaxed);
  wdLastProgressMs_ = wdLastCheckMs_ = steadyMs();

  while (running_.load(std::memory_order_acquire)) {
    /* Check if we have a full RNNoise frame available. */
    if (captureRing_->available_read() >= kRNNoiseFrameSize) {
      captureRing_->read(frame, kRNNoiseFrameSize);

      /* Run noise suppression, timing the processing itself only. */
      const auto t0 = std::chrono::steady_clock::now();
      rnnoise_.processFrame(frame);
      const auto t1 = std::chrono::steady_clock::now();
      stats_.recordFrameTime(uint32_t(
          std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()));

      /* If output is disabled, discard processed audio (no monitoring). */
      if (outputStream_) {
        const size_t w = outputRing_->write(frame, kRNNoiseFrameSize);
        if (w < kRNNoiseFrameSize) bump(stats_.outputDroppedSamples, kRNNoiseFrameSize - w);
      }

      /* Ring fill after this frame (latency + drift indicator). */
      stats_.recordFill(uint32_t(captureRing_->available_read()),
                        uint32_t(outputRing_->available_read()), outputStream_ != nullptr);
    } else {
      /*
       * Not enough data yet: block until the capture callback signals new
       * audio (instead of the former 0.5 ms sleep-poll). The timeout keeps
       * the watchdog and the running flag serviced if callbacks stop.
       */
      wake_.wait(kWakeTimeoutMs);
    }

    /* Handle real device loss (not xruns -- those are only counted). */
    if (deviceLost()) {
      attemptRestart();
    }
  }

#ifdef _WIN32
  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
  mmcssActive_.store(false, std::memory_order_relaxed);
#endif
}

/* ───────────────────── Device-Loss Watchdog ───────────────────── */

bool AudioEngine::deviceLost() {
  const int64_t now = steadyMs();
  if (now - wdLastCheckMs_ < kWatchdogIntervalMs) return false;
  wdLastCheckMs_ = now;

  bool lost = forceDeviceLoss_.exchange(false, std::memory_order_relaxed); /* test hook */
  /* A stream PortAudio stopped on its own (host error, device invalidated). */
  if (!captureStream_ || Pa_IsStreamActive(captureStream_) != 1) lost = true;
  if (!lost && config_.output.kind != DeviceSpec::Kind::None &&
      (!outputStream_ || Pa_IsStreamActive(outputStream_) != 1)) {
    lost = true;
  }

  /* The capture callback stopped being called while the stream looks alive. */
  const uint64_t cb = stats_.captureCallbacks.load(std::memory_order_relaxed);
  if (cb != wdLastCaptureCallbacks_) {
    wdLastCaptureCallbacks_ = cb;
    wdLastProgressMs_ = now;
  } else if (now - wdLastProgressMs_ > kCaptureStallMs) {
    lost = true;
  }

  if (lost) {
    stats_.deviceLossEvents.store(stats_.deviceLossEvents.load(std::memory_order_relaxed) + 1,
                                  std::memory_order_relaxed);
  }
  return lost;
}

/* ───────────────────── Auto-Restart ───────────────────── */

void AudioEngine::attemptRestart() {
  /* One "device-lost" event per outage, not one per retry cycle. */
  if (!inOutage_) {
    inOutage_ = true;
    if (statusCallback_) {
      statusCallback_("device-lost", "Audio device lost; trying to reconnect...");
    }
  }

  /* Stop the failed streams right away, not after the first backoff: a
   * stream that is still delivering audio would otherwise overflow the
   * capture ring while this thread sleeps (counted as dropped samples). */
  {
    std::lock_guard<std::mutex> lock(paMutex());
    if (captureStream_) Pa_StopStream(captureStream_);
    if (outputStream_) Pa_StopStream(outputStream_);
    closeStreams();
  }

  for (int attempt = 0; attempt < kMaxRestartAttempts; attempt++) {
    /* Exponential backoff: 100ms, 200ms, 400ms, 800ms, 1600ms */
    std::this_thread::sleep_for(
        std::chrono::milliseconds(100 * (1 << attempt)));

    if (!running_.load(std::memory_order_acquire)) return;

    std::lock_guard<std::mutex> lock(paMutex());

    /* Stop current streams. */
    if (captureStream_) Pa_StopStream(captureStream_);
    if (outputStream_) Pa_StopStream(outputStream_);
    closeStreams();

    /*
     * Re-initialise PortAudio so its device list is current: it is cached
     * until Pa_Terminate, so without this a replugged (or newly added)
     * device would never be found and indices could be stale. All streams
     * are closed here, and the mutex keeps getDevices() out meanwhile.
     */
    Pa_Terminate();
    if (Pa_Initialize() != paNoError) continue;

    /* Try to reopen (devices are resolved by identity again). */
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

    stats_.restarts.store(stats_.restarts.load(std::memory_order_relaxed) + 1,
                          std::memory_order_relaxed);
    resetWatchdog();
    inOutage_ = false;
    if (statusCallback_) {
      statusCallback_("restarted", "Audio reconnected: " + openedIn_.name);
    }
    return;
  }

  stats_.restartFailures.store(stats_.restartFailures.load(std::memory_order_relaxed) + 1,
                               std::memory_order_relaxed);
  resetWatchdog();
  /* Report the first failed round only; retries continue silently. */
  if (statusCallback_ && stats_.restartFailures.load(std::memory_order_relaxed) == 1) {
    statusCallback_("restart-failed",
                    "Could not reopen the audio device; will keep retrying");
  }
}

void AudioEngine::resetWatchdog() {
  outputPrimed_.store(false, std::memory_order_relaxed);
  wdLastCaptureCallbacks_ = stats_.captureCallbacks.load(std::memory_order_relaxed);
  wdLastProgressMs_ = wdLastCheckMs_ = steadyMs();
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
