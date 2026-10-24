# Krisp Desktop (Windows) — Observable Audio Architecture

**Purpose:** reference study to guide engineering of Ainoiceguard (Electron + N-API C++ addon + PortAudio + RNNoise).
**Date of investigation:** 2026-10-17
**Target:** Krisp for Windows 3.9.4 (previous 3.4.7), per-user install, on this machine.
**Method:** passive, read-only inspection of artifacts on disk: install logs, the application's own runtime logs, its settings store, the Windows audio/driver registry, and file metadata. No binaries were disassembled, no model files (`.kef`) were opened or copied, no licence or access control was touched, and the target was not modified.

---

## 0. Scope, method and key limitation

| Item | Status |
|---|---|
| Krisp program files (`%LOCALAPPDATA%\Programs\Krisp\app-3.9.4\`) | **Not present.** The app has been uninstalled; only an empty `app-3.4.7` folder remains. |
| Krisp user data (`%LOCALAPPDATA%\Krisp\`) | Present: logs (~28 MB, 23 files), settings, ASR/accent models, cached updater. |
| Krisp kernel driver | Present: `C:\Windows\System32\drivers\KrispAudio.sys` v4.0.2.2, service `KrispAudioS` (Manual, Stopped). PnP node `KRISPVUSB\KRISPVAD\…` with status *Unknown* (device not present). |
| Krisp audio endpoints in MMDevices | **Absent** (removed with the app). |
| Live process | Not running and not installed, so **no live runtime measurement was possible.** |

> **Update (2026-10-17, later the same day):** Krisp **3.18.3** was reinstalled and inspected live. The new evidence is in **§9 Live observations**, and it confirms or refines several hypotheses in §§1–5. Sections 1–8 still describe the 3.4.7/3.9.4 log-based analysis.

**Consequence:** almost everything below comes from Krisp's own verbose logs (`kr_app`, `kr_audio_dm`, `kr_media_sp`, `kr_native_utils`, `kr_ext_hub`), covering sessions from Aug 2025 to Feb 2026, plus the MSI/WiX install logs. These logs give strong evidence of *behaviour and structure*, but they are self-reported and say nothing about implementation internals.

**Confidence scale used:**
- **High:** directly observed in multiple log lines or artifacts.
- **Medium:** consistent inference from several observations.
- **Low:** plausible hypothesis that needs confirmation.

Personal data seen in the logs (account IDs, OS serial, install GUID, calendar data) is deliberately left out of this report.

---

## 1. Application modules, executables and native libraries

### 1.1 Verified observations

**Packaging (High).** The installer is a WiX Burn bundle (`krispInstaller.exe`, Burn v6.0.1) with three packages:
- `MainAppInstallationPackageId`: per-user MSI that installs to `%LOCALAPPDATA%\Programs\Krisp\app-<ver>\`.
- `MainAppInstallationPackageIdMachine`: a per-machine variant.
- `KDrivers`: a per-machine MSI named "Krisp Audio Drivers". It is registered as **permanent**, so it survives app uninstall (observed: the driver remains today).

Updates are downloaded silently to `%LOCALAPPDATA%\Krisp\update\krispInstaller.exe` and run with `/quiet /norestart`.

**Runtime is Electron/Chromium (High).** The evidence is `app.asar` (51 MB), `ffmpeg.dll`, `libEGL.dll`, `libGLESv2.dll`, `d3dcompiler_47.dll`, `vk_swiftshader.dll`, `vulkan-1.dll`, plus Chromium profile dirs (`Partitions\krispapp`, `Code Cache`, `GPUCache`, `Local Storage\leveldb`) and Crashpad and Sentry folders.

**Native modules** (from MSI FileCopy records, sizes in bytes):

| Module | Size | Role (from its own log class names) |
|---|---:|---|
| `kr-media-sp.node` | 26,037,248 | Media/signal-processing engine: graphs, processors, WASAPI capture, virtual-device sink, NC/AEC/ASR, recorders (`SPAPI`, `Graph`, `ProcessorFactory`, `KrispMediaSDK`, `KMSStream`). |
| `kr-audio-dm.node` | 1,415,168 | Audio *device manager*: MMDevice enumeration, default-device control, endpoint volume, audio-session monitoring, Krisp virtual-device load/unload (`MMApiHelper`, `MMNotificationHandler`, `AudioSessionNotificationHandler`, `KrispDeviceConfigurator`, `DeviceTuneUpController`). |
| `kr-native-utils.node` | 1,795,584 | System utilities: CPU/GPU/memory monitor, WMI, secure storage/credentials, certificate validation, process monitor (`HardwareUtilizationMonitor`, `SecureStorage`, `ProcessMonitorPoll`). |
| `kr-ext-hub.node` | 2,613,248 | Integrations with meeting apps (`GetChromeMeetingMetadataAction`, `GetZoomMeetingMetadataAction`, `SendZoomMeetingMessageAction`). |
| `kr-node-logger.node` | 574,464 | Native logging backend. |
| `aec-sdk.dll` | (not recovered) | Acoustic echo cancellation SDK (`KrispEchoCancellerSDK`). |
| `daily-core-sdk-wrapper.dll` | 28,418,560 | Daily.co (WebRTC) SDK wrapper, probed at startup for the "translations" feature. |
| `libmp3lame.dll` | 469,504 | MP3 encoding for call recordings. |
| several obfuscated-name `.dll` and `.kef` files | — | Bundled models and resources (names only; not inspected). |

**One process, many modules (Medium-High).** The same OS thread id (`17424`) appears in `kr_audio_dm.log`, `kr_media_sp.log` and `kr_native_utils.log` during the same session. Thread ids are unique system-wide while alive, so the device manager and media engine run **in the same process**. Which Electron process that is (main or utility) cannot be determined.

**Model files.** These are bundled in `resources\resources\models\audio\`. Names, sizes and roles below come from logs and the MSI only:

| File | Size | Log-declared role |
|---|---:|---|
| `c8.f.m.528d1f-1.0.1_1.0.kef` | 21.5 MB | `nc-outbound-mfb`: mic NC, "medium full-band", workRate 32 kHz, performance 50 |
| `c8.f.s.17aa82-1.0.1_1.0.kef` | 6.4 MB | `nc-outbound-sfb`: mic NC "small full-band", workRate 32 kHz, performance 15; also used as the *energy model* |
| `c8.w.m.d15e29_1.0.kef` | 13.3 MB | `nc-inbound-mwb`: speaker-side NC, "medium wide-band", workRate 16 kHz |
| `c6.b78b81_1.0.kef` | 0.9 MB | `rd-inbound-wb` (type 2): inbound "RD" model, 16 kHz |
| `hs.c6.f.m.75df8f.berlin.kef` / `hs.c6.f.s.de56df.bucharest.kef` | 29.4 / 15.1 MB | loaded as NC processors at startup (likely a separate feature family; see 1.2) |
| `%LOCALAPPDATA%\Krisp\models\asr_eng_6.2.0_1.0.kef` | 567 MB | English ASR (downloaded separately) |
| `%LOCALAPPDATA%\Krisp\models\ar_ind_wb_c_ma_3.7.0_1.0.kef` | 73 MB | accent-related model ("indian" accent type in logs) |

### 1.2 Hypotheses

- **Filename grammar (Medium).** The names appear to follow `c<family>.<band>.<size>.<hash>`, where `f` = full-band (32 kHz work rate), `w` = wide-band (16 kHz), `m`/`s` = medium/small. This matches the log names `mfb`/`sfb`/`mwb` exactly.
- **`hs.*` models (Low).** The `hs.*.berlin`/`bucharest` models may be "headset"-specific or a newer generation, given the BVC headset lists downloaded from `cdn.krisp.ai/win/res/headset_lists/v1.6.json`. Not confirmed.
- **"RD" (Low).** "RD" plausibly stands for a reverb/room or "remove duplicates" detector, given the `rdThreshold` parameter on the NC processor. Unknown.

### 1.3 Cannot be determined
Model architecture, parameter count, inference runtime (ONNX, custom, etc.), quantization, `.kef` container format or encryption, and how JS calls into the `.node` modules beyond what log names imply.

---

## 2. Microphone input, processing boundaries and output routing

### 2.1 Verified observations: the stream-graph model (High)

Krisp's media engine is organised as **named graphs**, each with one *source media*, an ordered *processor chain* and an optional *sink media*. Graph types seen:

| Graph | Source | Sink | Purpose |
|---|---|---|---|
| `krisp_mic_outbound` | physical mic (`'system'` media, WASAPI) | **Krisp virtual mic** (`'krisp'` media → `KrispSinkDevice` / `KrispRenderDevice`) | The actual NC path that call apps hear |
| `krisp_spk_inbound` | Krisp virtual speaker | physical speaker | Inbound (far-end) NC |
| `system_mic_outbound` | physical mic | none | Side-chain for AEC reference, stats, ASR and recording |
| `capture_inbound` | **loopback** of the active speaker (`isDeviceLoopback: true`) | none | Far-end capture for AEC reference, ASR and recording |

**Processor chain of `krisp_mic_outbound`** (creation order from `ProcessorFactory::CreateProcessor`, 2026-02-05 19:44:22):

```
WASAPI capture (device mix format, e.g. 48 kHz / 2 ch / float32)
  └─ FrameProcessingChain: downmix + resample → 32 kHz / 1 ch / float, 10 ms frames
       kr_aec      AEC_PROC   processingDelayMs=3000   (reverse stream = loopback graph)
       kr_mic_vol  VOL        volumeFactor
       kr_mic_asr  ASR        (suspended)
       kr_mic_rec_b FILE_REC  "before" tap
       kr_mic_ac   AL         accent processor (suspended)
       kr_mic_ar   AR         (suspended), noiseSuppressionLevel
       kr_mic_nc   NC         ncModel=nc-outbound-mfb, energyModel=nc-outbound-sfb, noiseSuppressionLevel=100
       kr_mic_rec_a FILE_REC  "after" tap
       kr_mic_rtc_asr RTC_ASR
       kr_mic_st_rec / kr_mic_tr_rec STREAM_REC
       kr_mic_play FILE_PLAY  (inject audio file into outbound stream)
  └─ Renderer → KrispRenderDevice  format 's' (int16), 48 kHz, 1 ch  → driver → "Krisp Microphone"
```

Further verified details:

- **Frame size: 10 ms at every stage** (`frameSpec: {(32000:1'f'):10}`; 1280-byte frames = 320 float samples).
- **Format adaptation at the boundary.** The capturer reads the device's native spec and converts it (`(48000:2'f') -> (32000:1'f')`). The `highQualityResampler: false` flag shows there is a choice of resampler quality. The ASR path uses 16 kHz.
- **The processing rate is chosen per model.** Each model declares `minRate`, `maxRate` and `workRate`, and the graph resamples to the model's work rate.
- **Bounded buffers.** `capturerFramesCacheSize: 10` (10 frames = 100 ms) and `rendererFramesCacheSize: 20` (200 ms). The capturer reports `Write/Overwrite/Read/Empty` counters on stop, and the renderer reports `BufferSizeErrorCount`, `PendingOperationErrorCount` and `UnableToWriteBytes`.
- **Processors can be suspended or resumed at runtime** (`Processor::Suspend/Resume`, `Graph::ChangeProcessorSettings`) without rebuilding the graph.
- **Health monitoring per graph:** `streamAvailabilityCheckInterval` (3000 ms on the NC graph), `capturedAllZerosCheckInterval` (10 s on the loopback graph; it fired a warning), and clipping detection (threshold 0.98, 2 clips in 5 × 500 ms windows).
- **Energy statistics** are logged every ~5 s per graph: `B:` before / `A:` after RMS (with peak).
- The NC processor reports **noise-class time buckets** on stop (`noNoiseMs`, `lowNoiseMs`, `mediumNoiseMs`, `highNoiseMs`, secondary-speech buckets).

### 2.2 Hypotheses
- **AEC delay (Medium).** `processingDelayMs: 3000` is most likely the AEC's *maximum* delay search window, not added latency. The AEC processor itself averages ~0 µs per frame (max 73 µs), which is consistent with it being idle or bypassed while no far-end reference was active.
- **Taps and file injection (Medium).** The FILE_REC "before/after" taps exist so recordings and A/B quality telemetry can capture both raw and processed audio. `FILE_PLAY` supports playing a test or soundboard clip into the virtual mic.

### 2.3 Cannot be determined
End-to-end latency through the driver, the internal algorithm-specific look-ahead of the NC model, and the exact resampler algorithm.

---

## 3. Virtual audio device integration and device selection

### 3.1 Verified observations

**Two-layer driver (High).**
- `KrispVUSB` is a virtual bus enumerator (`KrispVUSB.cat`, `KrispCoinstaller.dll` v3.0.1.1), shipped as the DIFx component `KrispDriverBus`.
- `KrispAudio.sys` (v4.0.2.2, 78 KB, dated 2024-04-16) is the audio function driver, shipped as component `KrispDriverAudio`.
- PnP id: `KRISPVUSB\KRISPVAD\…`. The driver exposes **"Krisp Microphone (Krisp Audio)"** and **"Krisp Speaker (Krisp Audio)"**.

**Endpoints are loaded and unloaded dynamically by the app (High).**
- On start: `KrispDeviceConfigurator::ConfigureKrispDevice: Type: 0, Load: true` and `Type: 1, Load: true`, after which `MMNotificationHandler::OnDeviceStateChanged … newState: ACTIVE` fires for the Krisp endpoints.
- On exit: `Successfully unloaded krisp mic` / `krisp spk`. The virtual devices therefore exist only while Krisp is running.

**App ↔ driver data path (Medium).** The media engine writes processed frames to a `KrispSinkDevice` / `KrispRenderDevice` object (int16 / 48 kHz / mono) rather than opening the virtual endpoint through WASAPI. That points to a private IOCTL or shared-memory channel into the driver. The mechanism itself is not observable.

**Device selection model (High):**
- **Persisted per-type priority queue** of physical devices, e.g. `PriorityList: Microphone (NVIDIA RTX Voice), External Microphone (Synaptics HD Audio)`, plus a flag for whether "same as system default" was chosen.
- **Default rule:** `ActiveDevice:updateDeviceList. Selected same as system. Device priority ignored.` When the user picks "same as system", Krisp follows the OS default *physical* device and ignores the priority queue.
- **Krisp makes its virtual device the Windows default** and remembers that it did so:
  - `needToSetKrispAsDefault: … isLastDefault: true → Setting Krisp as default multimedia device`.
  - The per-role state is persisted (`IS_LAST_MIC_DEFAULT_DEVICE_KRISP`, `IS_LAST_MIC_DEFAULT_COMMUNICATION_DEVICE_KRISP`).
  - **On exit it restores the physical device as the default** for each role (`DeviceManager::SetDefaultDevice: Succeeded. type: 0, role: 'communication', id: <physical mic>`).
- **Krisp tracks which apps use which endpoint.** It uses audio-session enumeration (`AudioSessionNotificationHandler`, `DeviceManager.dumpStreams` table with pid, app name and version per endpoint) to detect calls (`CallService … Calling Apps: NVIDIA RTX Voice, chrome, obs64`) and to count `krispStreams`.
- **Self-capture filtering:** `Discarded short spk event pid: …, app: krisp`.
- **Physical mic gain management:** `volumeControlMode: LOCK_ON_OPTIMAL_LEVEL` and `DeviceTuneUpController::PreventVolumeExternalChange … volume: 1`. Krisp pins the *physical* mic's endpoint volume and reverts external changes, and can optionally map the Krisp-mic volume slider onto the physical device (`VOLUME_MAPPING_USED_PREVIOUSLY`).
- **HID headset detection** (`TryGetHIDHeadsetPath`, `SetHidControlMode`) is used for headset mute-button integration.

### 3.2 Hypotheses
- **(Medium)** The NC graph is built eagerly when the device is active. The heavier model (`mfb`) is swapped in once a client actually opens the Krisp mic: `graphShouldBeStarted … krispStreams: 1` is followed by `getModelName … useModelSelectionByPerformance: true → nc-outbound-mfb`.

### 3.3 Cannot be determined
Driver internals (WaveRT vs. custom miniport, buffer sizes, clock source), and how the driver handles clock drift between the app's writes and the consumer's reads.

---

## 4. Startup, device disconnection and settings persistence

### 4.1 Startup sequence (High, from `kr_app.log` 2026-02-05 19:44:18–19:44:24)

1. Download BVC/headset lists; read system properties (CPU cores, GPU, OS build) via `kr-native-utils`.
2. Account/profile sync (Free plan; `UserModeManager:setup - disabled` → NC switch *enforced off*).
3. `AudioService:init` loads the persisted priority queues, then `StreamManager:init` registers the model catalog. This takes ~0.95 s.
4. `DeviceManager:init`. The first device-list event arrives, then the active physical device is resolved and the graph is (re)created with it.
5. The Krisp virtual devices are loaded, and Krisp sets itself as the default device if it was the default at the last exit.
6. The graphs start. Mic NC graph creation to `Renderer::Start` took ~2.1 s, mostly loading 3–5 NC models.

**Autostart:** a `Krisp` entry exists under `HKCU\…\Explorer\StartupApproved\Run`. This is the Windows "startup apps" toggle and is a leftover, since there is no matching `Run` value now.

### 4.2 Device-change and disconnection handling (High)
- **Sources of change events:** `IMMNotificationClient` events (`OnDeviceStateChanged` ACTIVE/UNPLUGGED/NOTPRESENT, `OnDefaultDeviceChanged` per flow and role, `OnDeviceAdded/Removed`), plus audio-session state events.
- **Debouncing:** stream-event control timers (`CreateStreamEventControlTimer`); short events are discarded.
- **On change, the graph's source device is swapped in place.** `StreamManager:changeGraphSettings … Settings: {"deviceId": …, "processors":[…]}` keeps the graph and processor state and replaces only the device. A full `removeStreamGraph` + `createStreamGraph` is used when the device *type* changes, e.g. switching the speaker to a loopback source.
- **Fallback:** if the selected device disappears, the next device in the priority queue is used, or the system default when "same as system" is set.
- **Errors are tolerated and logged** rather than crashing (`recreateAndStartGraphWithActiveDevice … activeDevice: undefined`, `graph doesn't exist`), and recovery happens on the next device event.

### 4.3 Settings persistence (High)
- `%LOCALAPPDATA%\Krisp\userConfigs.json` is an **NeDB-style append-only JSON-lines store**. Each record is `{date:{$$date}, userID, key, value, _id}`, where `userID: 0` means a machine-global setting and anything else is per-account.
- Audio-relevant keys include `AUDIO_ACTIVE_DEVICE_MIC/SPK`, `IS_DEFAULT_MIC_SELECTED`, `MIC_DEVICE_LIST`, `SPK_DEVICE_LIST` (the priority queue, stored **by device friendly name**), `IS_LAST_MIC_DEFAULT_DEVICE_KRISP`, `NC_PROCESSOR_STATE_MIC/SPK`, `NC_PROCESSING_MODE_MIC` (`BEST_NC` | `CPU_OPTIMIZED`), `VOLUME_MAPPING_USED_PREVIOUSLY`, `KRISP_DRIVER_IS_NOT_AVAILABLE`, `IS_FIRST_ADD_FOR_KRISP_MIC`, `LOAD_MODELS_ON_CHECKSUM_FAIL` and `ASR_MODELS_CHECKSUM`.
- Other state: `Preferences` / `Local State` (Chromium), `bvcLists.json`, `analytics.json`, and pending-upload queues (`pendingUsages.json`, etc.).
- Secrets are kept separately via `SecureStorage` / `CredentialManager` (Windows Credential Manager); they were not inspected.

### 4.4 Cannot be determined
How quickly audio resumes after an unplug, measured end to end. The logs show events but no glitch duration.

---

## 5. Runtime behaviour and performance

No live measurement was possible because Krisp is uninstalled. All figures below come from Krisp's own per-graph `TimeStats` and per-processor `Time Statistics` (units are almost certainly µs per 10 ms frame, given `Overrun - Dur: 10000`).

**Mic NC graph (`krisp_mic_outbound`), 12 sessions (High):**

| Frames | Avg µs | Min | Max | Overruns > 10 ms |
|---:|---:|---:|---:|---:|
| 38,743 | 942 | 38 | 5,157 | 0 |
| 15,498 | 838 | 34 | 284,535 | 1 (start-up) |
| 13,780 | 981 | 45 | 4,740 | 0 |
| 10,387 | 939 | 42 | 4,550 | 0 |
| 5,215 | 1,089 | 41 | 4,735 | 0 |
| others | 867–1,465 | — | ≤ 5,676 | 0 |

- Whole-graph cost is **~0.85–1.5 ms per 10 ms frame (8–15 % of one core's real-time budget)** on a 4-core HP ProDesk 400 G4 with a GT 710 GPU. Worst-case frames stayed around 4–6 ms, apart from one 285 ms stall at graph start, most likely model warm-up.
- The **NC processor alone** averages 766–929 µs (max ~4.5 ms) against a 6 ms per-processor overrun budget (`Overrun - Dur: 6000`). It is therefore budgeted at 60 % of the frame.
- **Overload detector (`PDTracker`).** Two filters ("Soft" alert at 7,500 µs and "Hard" at 15,000 µs) apply linear and FIR smoothing (lengths 7/22 and 2), need 200/20 samples to raise an incident and 1,000 samples to recover. Results surface as `[SYSTEM_OVERLOAD] … State: Stabilized` events in the UI layer, and `modelHistory` records `isOverloaded` per model. Only "Stabilized" transitions were seen, so no overload ever occurred here.
- **Processing modes** are user-selectable (`BEST_NC` ↔ `CPU_OPTIMIZED`) and map to `processingMode: BestQuality | BestPerformance`. A separate flag, `useModelSelectionByPerformance`, chooses between `mfb` (performance 50) and `sfb` (performance 15).
- **Power awareness:** at graph start Krisp logs `{isBatterySaverEnabled, currentPowerMode, isRunningOnACPower, batteryLevelPercentage}`.
- **System telemetry:** `HardwareUtilizationMonitor` dumps 32-sample CPU/GPU/memory tables roughly once a minute while a call is active, with per-process GPU engine usage. `krisp.exe` used about 0.7 % of the 3D engine, from the UI rather than inference: `forceCpuInference: true` is logged for ASR, and NC appears CPU-bound.
- **Log hygiene:** each module has its own rotating log (2 MB × up to 7 files), a `* Start new session *` banner, and statistics dumped on every stop. This is very effective for field diagnosis.

**Cannot be determined:** per-process CPU %, memory footprint of `krisp.exe`, and true input-to-virtual-mic latency.

---

## 6. Application layer vs. noise-suppression model

Evidence that Krisp's product value is **mostly in the application/framework layer around the model**, and what is model-only:

| Capability | Layer | Evidence | Confidence |
|---|---|---|---|
| Device capture, format conversion, resampling to the model's work rate | App/framework | `FrameProcessingChain`, `Resampler` independent of models | High |
| Model choice by sample rate, quality mode and CPU performance | App | `ProcessorManager:selectDynamicModel` over a JSON catalog (`minRate/maxRate/workRate/performance`) | High |
| Running the NC model on/off without tearing down audio | App | `Processor::Suspend/Resume`; NC stats show `FramesProcessed: 0, FramesSkipped: N` while NC was enforced off | High |
| Noise-level classification for telemetry/UX | Model-assisted, app-orchestrated | a small *energy model* (`sfb`) runs alongside; `noNoiseMs/lowNoiseMs/…` buckets reported even when NC output is bypassed | Medium |
| Suppression strength | App parameter into model | `noiseSuppressionLevel: 100` passed to NC/AR processors | High (that it exists) / Unknown (how it is applied) |
| Echo cancellation | Separate SDK | `aec-sdk.dll`, `KrispEchoCancellerSDK`, own instance per graph with ref-counting | High |
| Overload detection and degradation | App | `PDTracker`, `onProcessingOverloadStateChanged`, `modelHistory` | High |
| Virtual devices, default-device handling, volume lock, call detection | App + driver | `kr-audio-dm`, `KrispDeviceConfigurator`, session enumeration | High |
| Actual speech/noise separation quality | **Model** | `.kef` weights loaded by `KMSStream::CreateNCProcessor` | High that it is the model; internals unknown |

Important caveat on the CPU figures: in the sessions measured, **NC output was suspended** (Free-plan enforcement; `isNCSuspended: true`, all frames *skipped*). Yet the NC processor still spent ~0.8 ms per frame. The measured cost therefore most likely reflects the energy/statistics model plus framework overhead, **not** the full `mfb` model's cost when actively suppressing. Treat the 5 % figure as a lower bound (confidence: Medium).

**Cannot be determined:** how much of Krisp's perceived quality is due to the model rather than the pipeline (AEC, resampling quality, gain lock). A controlled A/B listening test with the app installed would be needed, and is out of scope here.

---

## 7. Comparison with current Ainoiceguard (from repository source)

| Concern | Krisp (observed) | Ainoiceguard today (`native/src/*`, `electron/*`) |
|---|---|---|
| Output to call apps | own virtual mic driver, loaded on demand | VB-Cable via PortAudio output stream |
| Capture mode | WASAPI shared, device native format, resampled | WASAPI **exclusive attempted first** (`tryExclusiveMode = true` in `addon.cc`), fixed 48 kHz mono |
| Frame size | 10 ms | 10 ms (480 samples at 48 kHz) ✓ |
| Pipeline | graph of suspendable processors | fixed chain in `RNNoiseWrapper::processFrame` (HPF → RNNoise → blend → gain → gate → comfort noise) |
| Thread hand-off | frame caches with overwrite/empty counters | SPSC ring, 4096 samples (~85 ms), processing thread polls with 500 µs sleep |
| Clock drift (mic clock vs. render clock) | handled inside the Krisp driver/sink (not observable) | **not handled**: the output ring silently fills or empties |
| Restart trigger | device-state notifications | `paInputOverflow/Underflow` / `paOutputUnderflow/Overflow` flags, i.e. ordinary xruns, trigger a **full stream restart** |
| Restart status reporting | logged + UI events | `setStatusCallback` exists but **is never wired** in `addon.cc`, so the UI never hears about restarts |
| Device identity | endpoint ID + persisted friendly-name priority list | **PortAudio index** (unstable across hot-plug), not persisted |
| Hot-plug discovery | `IMMNotificationClient` | none; PortAudio caches the device list until `Pa_Terminate`, and `enumerateDevices()` while running cannot see new devices |
| Settings persistence | NeDB key/value per user | none (no store in `main.js`/`renderer.js`) |
| Performance telemetry | per-frame avg/min/max/overrun, overload detector | RMS/VAD/gain metrics only; no timing |
| Health checks | all-zeros, clipping, stream availability | none |

---

## 8. Prioritized engineering improvements for Ainoiceguard

Each item is grounded in a pattern observed above and needs no proprietary code or models. Ordered by user impact relative to effort.

### P0: correctness and robustness
1. **Stop treating xruns as disconnects.**
   - Today any over/underflow flag tears down and reopens both streams (`audio.cpp` `captureCallback`/`outputCallback` → `attemptRestart`). Count xruns as metrics instead.
   - Restart only on real device loss: `paUnanticipatedHostError`, `Pa_IsStreamActive()==0` while running, or an endpoint-removed notification.
2. **Wire the status callback to JS** with a `Napi::ThreadSafeFunction`, so the UI shows "device lost / recovered" events. At present `setStatusCallback` is never called.
3. **Default to WASAPI shared mode.** Exclusive capture blocks every other app from the physical mic and fails on devices whose native rate is not 48 kHz. Krisp opens the device at its mix format and converts.
   - Make exclusive mode an opt-in "low-latency" setting.
   - Add downmix and a resampler to 48 kHz using a permissively licensed library such as speexdsp or libsamplerate, both BSD.
4. **Stable device identity plus persisted settings.**
   - Store the selected input/output by *name + host API* (better: the WASAPI endpoint ID via PortAudio's `PaWasapi_GetDeviceId` or direct IMMDevice), never by index.
   - Persist suppression level, VAD threshold, devices and "follow system default" in a JSON file under `app.getPath('userData')`, written atomically.
   - Keep a small priority list as Krisp does (`MIC_DEVICE_LIST`).
5. **Hot-plug and default-device awareness.** Add an `IMMNotificationClient` (Windows) in the addon, or periodic re-enumeration done safely when streams are idle.
   - On removal of the active device, fall back to the next device in the priority list or the system default.
   - Swap only the capture stream, keeping the RNNoise state and output stream (the equivalent of Krisp's `changeGraphSettings`).

### P1: audio quality and latency
6. **Clock-drift control between the mic and VB-Cable.** These are two independent clocks.
   - Monitor the output-ring fill level and steer it to a target (e.g. 20 ms) with a slight adaptive resample ratio (±0.1 %), or with frame drop/duplicate as a minimum.
   - Without this, latency slowly grows to ~85 ms and then glitches.
7. **Event-driven processing thread.** Replace the 500 µs sleep loop with a semaphore signalled by the capture callback, and register the thread with MMCSS "Pro Audio" (`AvSetMmThreadCharacteristicsW`). This lowers jitter and wasted wakeups.
8. **Per-frame timing instrumentation plus an overload detector.**
   - Record avg/min/max processing time per 10 ms frame and an overrun count, and expose them through `getMetrics()`.
   - Add soft/hard thresholds with hysteresis, modelled on Krisp's PDTracker (alert above X µs sustained for N frames, recover after M frames).
   - Surface the result in the UI and auto-degrade, e.g. disable the gate or comfort-noise stages.
9. **Suspend/bypass instead of restart.** Toggling NC or changing level must never reopen streams. Krisp's processor-level `Suspend/Resume` keeps audio continuous. Ainoiceguard's level=0 fast path already does this for NC; extend it to every stage and to any future processors.
10. **Health monitors.** Add all-zeros input detection (privacy-muted or dead mic, with a UI warning after ~3–10 s) and clipping detection (peak ≥ 0.98 more than N times per 500 ms window, with a "lower your mic gain" hint). Both are cheap and directly improve support burden.

### P2: architecture and features
11. **Processor-chain abstraction.** Refactor `processFrame` into a list of processors (HPF, NC, gate, comfort noise, later AEC/AGC), each with `process`, `suspend`, `setParam` and per-processor timing. This mirrors Krisp's graph and makes new stages and A/B testing easy.
12. **Optional higher-quality model tier.** Krisp's `BEST_NC` / `CPU_OPTIMIZED` split, with models chosen by sample rate and performance score, is the key app-level idea. Ainoiceguard could keep RNNoise as "CPU optimized" and add an optional open model (e.g. DeepFilterNet, MIT/Apache) as "best quality", selected by a small JSON catalog with `workRate` and `cost` fields.
13. **Echo cancellation using a loopback reference.** Krisp captures the active speaker in loopback as the AEC reverse stream. An open AEC (WebRTC AEC3, BSD) fed from a WASAPI loopback of the user's output device would remove speaker bleed for non-headset users.
14. **Session-aware auto-start.** Use `IAudioSessionManager2` on the VB-Cable *recording* endpoint to detect when a call app opens it, then start or warm up processing and show "Zoom is using Ainoiceguard". Krisp's `krispStreams` count and call detection work the same way.
15. **Structured native logging.** Add per-module rotating logs (size cap, ~5 files), a session banner, and a statistics dump on stop (frames, xruns, timing histogram, ring fill). Keep PII out, unlike the OS serial and account IDs Krisp writes.
16. **Long-term: own virtual microphone.** Only once the above is stable. Krisp needed a bus driver, an audio driver and a co-installer, plus driver signing and attestation. Note that Krisp's driver package is marked *permanent* and outlives uninstall; Ainoiceguard should remove its components cleanly. Until then, keep the VB-Cable path and respect its redistribution licence, as `build/installer/installer.nsh` already does.

---

## 9. Live observations: Krisp 3.18.3 (installed 2026-10-17 21:24)

All checks were read-only: process, module and thread listings, MMDevices registry, plain-text INF/JSON files that ship with the app, and a 30-second CPU/memory sample. Nothing was configured, toggled or patched.

### 9.1 What changed since 3.9.4 (High)

| Item | 3.9.4 (logs) | 3.18.3 (live) |
|---|---|---|
| Driver | `KrispAudio.sys` 4.0.2.2 | `KrispAudio.sys` + `KrispVUSB.sys` **5.3.2.1** (DriverVer 2026-04-23), x64 and ARM64 packages under `%LOCALAPPDATA%\Programs\Krisp\drivers` and `C:\ProgramData\Krisp\Drivers` |
| Background service | none observed | **`KrispHelperService`** (`C:\ProgramData\Krisp\Services\KrispHelperService.exe`, 11.8 MB, **LocalSystem, Auto start**) |
| Native module location | `resources\…` | `resources\app.asar.unpacked\.vite\build\node_modules\kr-*\build\Release\*.node` (Vite-bundled Electron app) |
| New native deps | — | `sioclient_tls.dll` (socket.io client), `krisp-curl-wss-wrapper.dll` (WebSocket over curl), `dxcompiler.dll`/`dxil.dll` (Chromium) |
| New models | — | `diar_2.0.0_1.0.kef` (32.6 MB, speaker diarization), `nc-outbound-snb` narrow-band model, accent-reduction in/outbound |
| `krisp.exe` | — | 214 MB (Electron + Chromium) |

### 9.2 Model catalog is a plain JSON file (High)
`resources\resources\models\audio\models.json` (2.4 KB) is the catalog that `ProcessorManager:selectDynamicModel` uses. It confirms the §1 hypotheses:

| name | type | workRate | rate range | perf cost | file |
|---|---|---:|---|---:|---|
| `nc-outbound-mfb` | 0 (NC) | 32 kHz | 8001–192000 | 50 | `c8.f.m…` |
| `nc-outbound-sfb` | 0 (NC) | 32 kHz | 8001–192000 | 15 | `c8.f.s…` |
| `nc-outbound-snb` | 0 (NC) | 8 kHz | 0–8000 | 10 | `c5.n.s…` |
| `hvc-outbound-lfb` | 1 (HVC) | 32 kHz | 8001–192000 | 75 | `hs.c6.f.m…berlin` |
| `hvc-outbound-sfb` | 1 (HVC) | 32 kHz | 8001–192000 | 15 | `hs.c6.f.s…bucharest` |
| `nc-inbound-mwb` | 0 (NC) | 16 kHz | 0–192000 | 50 | `c8.w.m…` |
| `rd-inbound-wb` | 2 (RD) | 16 kHz | 0–192000 | 0 | `c6.b78b81…` |
| plus `asr` (type 4), `ar-*` accent reduction (type 6), `diar` (type 8) | | | | | |

- **Filename grammar confirmed:** `f` = full-band (32 kHz), `w` = wide-band (16 kHz), `n` = narrow-band (8 kHz); `m`/`s`/`l` = medium/small/large.
- **`hs.*` refined:** these are type 1 **"HVC"** models, most likely *human/background voice cancellation*, which fits the BVC lists in §1.2. That replaces the earlier "headset" guess (confidence: Medium).
- **Design takeaway:** model choice is *data-driven*. A small manifest declaring each model's rate range, internal work rate and relative cost lets the app pick a model per device sample rate and per performance mode without code changes. Ainoiceguard can adopt this pattern without using any Krisp asset (§8 item 12).

### 9.3 Virtual-device driver design (High)
- **Bus driver:** `KrispVUSB.inf` describes a root-enumerated **KMDF 1.15** bus driver (`Root\KrispVUSB`, class USB, `SERVICE_DEMAND_START`, `PnPLockdown=1`). Its security descriptor gives full access to SYSTEM and read/write/execute to Administrators and Authenticated Users. The bus enumerates the child `KrispVUSB\KrispVad`, which is how the app "loads" and "unloads" the endpoints on demand (§3: `ConfigureKrispDevice Load: true`).
- **Audio driver:** in `KrispAudio.inf`, the INF section names (`SYSVAD.I.WaveSpeaker`, `SYSVAD.I.WaveMicIn`, `SYSVAD.I.Topology*`) show it is **derived from Microsoft's public SYSVAD WDK sample**, a PortCls WaveRT virtual audio driver. It registers `KSCATEGORY_AUDIO/RENDER/CAPTURE/REALTIME/TOPOLOGY` interfaces. Its endpoints carry the association `KSNODETYPE_HEADSET_MICROPHONE` / `KSNODETYPE_HEADSET_SPEAKERS`, so Windows and call apps treat them as a headset.
- **Live endpoint mix formats** (from `PKEY_AudioEngine_DeviceFormat`):

  | Endpoint | Mix format |
  |---|---|
  | Krisp Microphone (Krisp Audio) | **48 kHz, 1 ch, 16-bit PCM** (extensible) |
  | Krisp Speaker (Krisp Audio) | 48 kHz, 2 ch, 16-bit |
  | External Microphone (Synaptics) | 48 kHz, 2 ch, 32-bit container / 24 valid |
  | Speakers (Synaptics) | 48 kHz, 2 ch, 32/24 |

  The virtual mic's mono 16-bit format matches `KrispRenderDevice::AudioSampleSpec format 's', 48000, 1ch` in the logs. **Verified end to end:** the app converts its 32 kHz float NC output to 48 kHz int16 mono and writes it into the driver.
- **Implication for Ainoiceguard (§8 item 16):** SYSVAD (MIT-licensed, in `microsoft/Windows-driver-samples`) is a realistic, legitimate starting point for a first-party virtual microphone. The remaining hurdles are a loopback/injection path from user mode, plus attestation signing and an EV certificate.

### 9.4 Process model and threads (High unless noted)
- **15 processes:** the main process (105 threads), a GPU process, a network utility process, crashpad, **10 renderer processes** (so many hidden windows or webviews: widget, notifications, dashboard and others), and `KrispHelperService`.
- **All audio-native modules are loaded in the Electron *main* process** (PID 3448): `kr-media-sp.node`, `kr-audio-dm.node`, `kr-native-utils.node`, `kr-ext-hub.node`, `kr-node-logger.node`, `aec-sdk.dll`, `libmp3lame.dll`, `daily-core-sdk-wrapper.dll`, `sioclient_tls.dll`, `krisp-curl-wss-wrapper.dll`. No other Krisp process loads `kr-media-sp.node`. This resolves the §1 "main vs utility" question in favour of the main process.
- **System audio DLLs in the main process:** `MMDevAPI`, `AudioSes` (WASAPI), `AVRT` (MMCSS), `HID` (headset buttons), `SetupAPI`, `MFPlat`, `WinMM`.
- **No ONNX Runtime or DirectML DLL is loaded.** Inference is therefore most likely statically linked inside `kr-media-sp.node`, which is consistent with its 28.8 MB size (confidence: Medium).
- **Real-time threads:** 5 threads run at **priority 21 (base 21)**. A normal-priority process can reach at most 15 with `THREAD_PRIORITY_TIME_CRITICAL`, so priority 21 together with `AVRT.dll` being loaded indicates MMCSS-registered threads, most likely the capture and render threads of the active graphs (confidence: Medium-High). This supports §8 item 7: register Ainoiceguard's processing thread with MMCSS.

### 9.5 Measured footprint (NC enforced off)
A 30-second sample on a 4-core i-series HP ProDesk with an idle call state:

| Metric | Value |
|---|---|
| Total CPU, all Krisp processes | **0.13 % of machine** (main process 0.12 %) |
| Private memory, total | ~2.4 GB (main process **1.72 GB private, 59 MB working set**) |
| Working set, total | ~1.5 GB (renderers 90–180 MB each) |

- The large private commit in the main process with a small working set suggests models and ASR buffers are allocated or memory-mapped at start-up and then paged out while idle (confidence: Medium).
- For Ainoiceguard, a single renderer plus the native engine should stay at a fraction of this. Memory footprint is a realistic differentiator.

### 9.6 Why active-NC cost and latency were *not* measured
- The signed-in account resolved to `UserModeManager:setup - disabled`. Krisp then logs `enforceNcState … NCState: off` for the mic and speaker, so **NC is suspended by account state.** Any timing taken now would repeat the §5/§6 lower-bound problem.
- The native module logs (`kr_media_sp`, `kr_audio_dm`) were not yet written for this session. Only `kr_app.log` updated, which suggests the native loggers start lazily, e.g. on the first stream (confidence: Low).
- Changing the Windows default device or pulling a device to test disconnect handling would alter system settings, so it was left to the user (see Appendix B).

### 9.7 Privacy observation (High)
`kr_app.log` records the full `aikrispapp://login/?token=…` protocol URL in plain text when you sign in. The token was not copied into this report. **Lesson for Ainoiceguard:** redact URLs, tokens and query strings in all logs by default.

---

### Appendix A: artifacts examined
- `%LOCALAPPDATA%\Krisp\Logs\install_3.9.4*.log`, `Krisp_3.4.7-*_000_KDrivers.log` (WiX/MSI/DIFx)
- `%LOCALAPPDATA%\Krisp\Logs\kr_app*.log`, `kr_audio_dm*.log`, `kr_media_sp*.log`, `kr_native_utils*.log`, `kr_ext_hub.log`
- `%LOCALAPPDATA%\Krisp\userConfigs.json` (keys and non-sensitive values only)
- `C:\Windows\System32\drivers\KrispAudio.sys` (version resource only); service `KrispAudioS`; PnP `KRISPVUSB\KRISPVAD`
- `HKLM\…\MMDevices\Audio\{Capture,Render}`; `HKCU\…\StartupApproved\Run`
- Ainoiceguard: `native/src/audio.{h,cpp}`, `rnnoise_wrapper.{h,cpp}`, `ringbuffer.h`, `addon.cc`, `electron/main.js`, `electron/renderer.js`, `build/installer/installer.nsh`

### Appendix B: remaining tests (require user action)
Krisp 3.18.3 is now installed (§9). The remaining gaps need NC to be **actually on**, which the current account state prevents:
1. **Enable NC:** use an account or trial where the mic NC toggle stays on, then confirm `kr_app.log` shows `changeNCProcessorState: Type: microphone, NCState: true`.
2. **Active-NC cost:** start a test call (any app recording from *Krisp Microphone*), speak for about 2 minutes, then end the call. Krisp's `Graph::Stop` and `Processor::Stop(kr_mic_nc)` will log `TimeStats` with `FramesProcessed > 0`, which can be compared with an external 30 s CPU sample taken during the call.
3. **Latency:** play a click through the physical mic path and record the *Krisp Microphone* and the raw mic simultaneously, e.g. in Audacity with two inputs. The offset between the two is the Krisp path's latency.
4. **Disconnect handling:** during a call, unplug a USB/headset mic (or disable the mic in Sound settings), then plug it back in. The resulting `OnDeviceStateChanged` → `changeGraphSettings` sequence and the gap in `EnergyStats` show the recovery time.
