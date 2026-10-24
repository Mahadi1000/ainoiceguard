# Ainoiceguard — Suppression Engine Implementation Plan

**Status:** Proposed. Awaiting approval; no code has been changed.
**Date:** 2026-10-17
**Based on:** `origin/main` @ `974ce56`, and [krisp-audio-architecture.md](krisp-audio-architecture.md)

Local `main` has diverged from `origin/main` by one commit each way. The two versions of that commit differ only in `scripts/dist-all.js` and `scripts/verify-dist.js`. The local git remote is still named `Mahadi1000/noiseguard`, which redirects to `Mahadi1000/ainoiceguard`.

**Guiding constraints:**
- Preserve the Electron UI, PortAudio pipeline, C++ addon and ring-buffer design unless a demonstrated problem requires a change.
- RNNoise remains the baseline backend.
- Establish a reproducible baseline before changing the engine.
- No unnecessary dependencies.

---

## 1. Current architecture

```
Mic ─PortAudio capture cb─► captureRing_ (SPSC, 4096 f32) ─► processingLoop thread
    ─► RNNoiseWrapper::processFrame (HPF → RNNoise → dry/wet → +2 dB → VAD gate → comfort noise)
    ─► outputRing_ ─PortAudio output cb─► VB-Cable / speakers
```

| Area | Files |
|---|---|
| Engine | `native/src/audio.{h,cpp}`: PortAudio streams, WASAPI exclusive-first, restart logic |
| Processing | `native/src/rnnoise_wrapper.{h,cpp}`: 10 ms / 480-sample chain, atomic metrics |
| Hand-off | `native/src/ringbuffer.h`: lock-free SPSC ring |
| JS bridge | `native/src/addon.cc`: one global engine; start/stop/level/VAD/metrics |
| Build | `native/CMakeLists.txt` (FetchContent PortAudio v19.7.0 + mumble RNNoise), `native/binding.gyp`, `scripts/build-native.ps1`, `scripts/build-native-linux.sh` |
| App | `electron/main.js` (IPC), `electron/renderer.js` (device UI, VB-Cable detection), `electron/tray.js`, `build/installer/installer.nsh` |
| CI | `.github/workflows/ci.yml`: unit tests on Linux; native build and packaging on Linux, macOS and Windows |
| Tests | `test/*.test.js`: smoke test, metrics formatting, dist checks. No native or audio tests. |

---

## 2. Limitations, with evidence

### Reproducibility (blocks a trustworthy baseline)
- **L1.** RNNoise is fetched at `GIT_TAG master` (`native/CMakeLists.txt:63`), so baseline results can drift between builds.
- **L2.** There are no tests of audio output. Nothing detects a regression in `processFrame`.
- **L3.** A directory literally named `$DEPS_INSTALL/` is committed, containing prebuilt `.lib` files and headers (added in `55256f9`). It is stale and could mask build problems.

### Measurement (blocks a fair RNNoise vs DeepFilterNet comparison)
- **L4.** No processing-time, xrun, ring-fill or latency metrics exist. `getMetrics` only returns RMS, VAD, gain, noise floor and frame count (`native/src/addon.cc`).
- **L5.** Underflow and overflow flags trigger a **full stream restart** rather than being counted (`native/src/audio.cpp:332`, `:368`). An underrun therefore can't be measured; it causes a further 0.1–3.1 s outage through the restart backoff.
- **L6.** Dropped samples go unrecorded: the return value of `RingBuffer::write` is ignored, so a full ring drops audio silently.

### Engine quality and robustness (later phases, but they affect measurements)
- **L7.** WASAPI **exclusive** mode is tried first (`native/src/addon.cc:83`, `native/src/audio.cpp:216`). This locks the mic away from other apps, and the fixed 48 kHz rate fails on 44.1 kHz devices.
- **L8.** Nothing compensates for clock drift between the mic and VB-Cable clocks. The output ring (~85 ms, `native/src/audio.cpp:34`) slowly fills or empties.
- **L9.** The processing thread polls with a 500 µs sleep (`native/src/audio.cpp:407`) and is not registered with MMCSS (Krisp's audio threads run at priority 21; Krisp report §9.4).
- **L10.** **Hard clipping** at the output: `std::clamp(x * 1.26f, -1, 1)` (`native/src/rnnoise_wrapper.cpp:229`). There is no limiter or AGC.
- **L11.** `setStatusCallback` is never called from `addon.cc`, so restart and failure messages never reach the UI.
- **L12.** Devices are addressed by PortAudio index, with no persistence and no hot-plug detection. PortAudio caches its device list until `Pa_Terminate`, and `addon.stop()` blocks the Electron main thread during any restart backoff.

---

## 3. Phased plan

### Phase 0: reproducible baseline (no behaviour change)
- **0.1** ~~Reconcile local `main` with `origin/main`~~. **Changed 2026-10-24:** `origin/main` and 9 other remote branches were force-pushed with obfuscated malicious JavaScript in `scripts/dist-all.js` (and in `scripts/verify-dist.js` on `main`). Phase 0 is built from the clean local `main` (`848a128`) instead; GitHub is to be restored from local once the account is secured. Pin the mumble RNNoise commit in `CMakeLists.txt` and remove the committed `$DEPS_INSTALL/`. **Done:** see [phase0-baseline.md](phase0-baseline.md).
- **0.2** Add an **optional** CMake target, `ng_bench`, behind `-DNG_BUILD_BENCH=ON` (off by default, so app builds are unchanged). It is a small C++ command-line tool that:
  - reads and writes 48 kHz mono PCM16/float32 WAV with a ~100-line in-house reader/writer (no new dependency);
  - runs **two RNNoise variants**: `rnnoise-raw` (model only) and `rnnoise-chain` (the full current `RNNoiseWrapper`). This separates model quality from post-processing, the same app-vs-model split the Krisp report draws;
  - times every frame with `steady_clock` and writes JSON: p50/p95/p99/max µs per frame, real-time factor and output length.
- **0.3** Fixtures under `bench/fixtures/`, with only small files committed and everything else fetched by script and checked against SHA256 hashes:
  - generated synthetic set: clean speech + noise at −5/0/5/10/20 dB SNR, pure silence, a noise-only clip, a near-clipping clip and an impulse train (for latency);
  - optional real set: a fixed 50-file subset of VoiceBank+DEMAND (CC BY 4.0), downloaded by script, not committed.
- **0.4** Metrics script `bench/score.py` (dev-only, pinned `requirements.txt`, never shipped). It reports SI-SDR (numpy), STOI (`pystoi`, MIT) and DNSMOS P.835 (Microsoft's ONNX model) where a clean reference exists, plus noise attenuation and speech-level change in dB. Algorithmic delay is measured by cross-correlating output against input before scoring, so delayed outputs are not penalised. PESQ is optional only (see §5).
- **0.5** Golden regression test: the `rnnoise-chain` output on two short fixtures must match a stored output within a tolerance of 1e-4 RMS difference. It runs in CI on Linux.

### Phase 1: offline DeepFilterNet 3 comparison
- **1.1** Add `dfn` as a third bench backend, built **only** with `-DNG_WITH_DFN=ON`:
  - libDF is built with `cargo build --release --features capi` (crate type `staticlib`), pinned to tag `v0.5.6`;
  - it links the official C API: `df_create(model_path, atten_lim, log_level)`, `df_get_frame_length`, `df_process_frame`, `df_set_atten_lim`, `df_set_post_filter_beta` and `df_free`;
  - the model is the official `models/DeepFilterNet3_onnx.tar.gz`.
- **1.2** Cross-check the integration against the official `deep-filter` CLI on the same files. The two outputs should be near-identical after delay alignment; this proves the wrapper is correct before any quality conclusion is drawn.
- **1.3** Record the facts the published sources don't state, read from the model's `config.ini` and by measurement: hop size, FFT size, lookahead and real algorithmic delay. The streaming C API does **not** compensate delay.
- **1.4** Run each backend at a few settings: DFN at attenuation limits {6, 12, 18, 100 dB} with post-filter on and off; RNNoise at suppression level 1.0 and its default settings.
- **1.5 Decision gate.** DFN proceeds to Phase 2 only if **all** of these hold on the reference machine (the 4-core HP ProDesk 400 G4):
  - DNSMOS OVRL is ≥ 0.3 higher than `rnnoise-chain`, and STOI is no worse than −0.01, averaged over the SNR ≤ 10 dB fixtures;
  - DFN p99 frame time is ≤ 5 ms (half the 10 ms budget) and real-time factor is ≤ 0.3 on one core;
  - the added algorithmic latency is ≤ 40 ms;
  - silence and noise-only fixtures produce no audible artifacts (spot-checked, with the WAVs kept).

  If the gate fails, stop: RNNoise stays and the results are documented.

### Phase 2 (only if the gate passes): native streaming integration
- **2.1** Add a minimal backend interface in the addon: `init`, `frameLength`, `process(in, out)`, `latencySamples` and `reset`. `RNNoiseWrapper` keeps the HPF, gate and comfort-noise stages but moves behind it. The rings, PortAudio code and UI stay as they are.
- **2.2** DFN backend design:
  - **State.** Exactly one `DFState` per engine start, held for the whole session; its STFT and recurrent state persist across `df_process_frame` calls. There is no reset in the C API, so `reset()` means `df_free` followed by `df_create`, done off the audio path.
  - **Buffering.** A FIFO in the processing thread adapts to `df_get_frame_length()` if it isn't 480.
  - **Thread placement.** DFN runs only in the processing thread, never in PortAudio callbacks; its allocation behaviour is unknown.
  - **Crash safety.** A Rust panic aborts the process. The model is validated with a probe frame at `start()`, NaN/Inf input is replaced before every call, and model load happens before the streams start.
  - **Controls.** The suppression slider maps to `df_set_atten_lim` instead of the dry/wet blend. Whether the gate and comfort noise run with DFN is decided by the Phase 1 data.
- **2.3** Packaging: the model ships as an `extraResources` file outside the asar archive. The backend choice is a runtime setting (default RNNoise until it is validated live), with automatic fallback to RNNoise if DFN fails to load.

### Phase 3: live measurements (needed for both backends)
- **3.1** Count xruns per stream instead of restarting on them (fixes L5); keep restarts for real errors. This is the one engine behaviour change needed early, because underruns can't be measured otherwise.
- **3.2** Add atomic counters: a processing-time histogram (µs buckets), dropped samples on the capture and output rings (L6), ring fill min/max, and an overrun count against the 10 ms budget.
- **3.3** End-to-end latency is reported in two ways:
  - **Estimated:** PortAudio-reported input and output latency, plus mean ring fill, plus backend latency.
  - **Measured:** an `ng_latency` tool plays an impulse through a known path and records *CABLE Output* via a separate stream, then cross-correlates. A documented manual click test covers the physical mic.
- **3.4** Process CPU from Electron `app.getAppMetrics()`. A statistics summary is written on stop, in the style of Krisp's logs (Krisp report §5).
- **3.5** All of this is exposed through `getMetrics()` without changing the existing fields.

### Phase 4: after the suppression engine is validated, in priority order
1. **AGC + limiter (L10):**
   - a VAD-gated slow AGC targeting about −18 dBFS for speech, with ±12 dB range and roughly 1 s attack/release;
   - a soft-knee peak limiter (≤ −1 dBFS ceiling, 1–2 ms attack) replacing `std::clamp`;
   - written in-house (~150 lines, no dependency).
2. **Reliable device handling (L7, L11, L12):**
   - WASAPI shared mode by default, with exclusive as opt-in; resampling only for devices not at 48 kHz;
   - stable device IDs (name + host API), persisted to JSON in `app.getPath('userData')`, plus a priority list;
   - device loss detected through PortAudio errors, with recovery by re-enumerating PortAudio while it is idle (Windows `IMMNotificationClient` as an enhancement);
   - status reported to the UI via `Napi::ThreadSafeFunction`;
   - a non-blocking `stop()`;
   - drift control (L8): steer the output ring to a target fill level.
3. **Virtual microphone:**
   - keep VB-Cable short term;
   - Windows long term: a SYSVAD-derived driver (the approach Krisp uses, Krisp report §9.3);
   - Linux: a PipeWire/Pulse virtual source (no driver needed);
   - macOS: an AudioServerPlugIn.

   Each of these is a separate design doc and approval.

---

## 4. Acceptance criteria and test cases

| ID | Phase | Test | Pass condition |
|---|---|---|---|
| T0.1 | 0 | Clean checkout, both build scripts, `NG_BUILD_BENCH=OFF` | Same addon as today; existing tests pass |
| T0.2 | 0 | `ng_bench` run twice on all fixtures | Byte-identical output; JSON written |
| T0.3 | 0 | Golden test on two fixtures | ≤ 1e-4 RMS difference from stored output |
| T0.4 | 0 | `score.py` on clean == processed | SI-SDR > 60 dB and STOI ≈ 1.0 (sanity check of the metric code) |
| T1.1 | 1 | `dfn` vs official `deep-filter` CLI | After delay alignment, SI-SDR between the two outputs > 40 dB |
| T1.2 | 1 | Impulse fixture | Measured delay matches the delay derived from `config.ini` within ±1 frame |
| T1.3 | 1 | Silence and noise-only fixtures | Output RMS ≤ −60 dBFS; no NaNs |
| T1.4 | 1 | Gate report | Table generated for the §3 1.5 criteria; decision recorded |
| T2.1 | 2 | 10-minute live run per backend | 0 crashes; overruns ≤ 0.1 % of frames; ring drops = 0 |
| T2.2 | 2 | Start/stop 100× and backend switch 20× | No leaks (private memory delta < 5 MB) and no hangs |
| T2.3 | 2 | DFN model missing or corrupt | Falls back to RNNoise with a UI message |
| T3.1 | 3 | Induced xrun (CPU stress) | Counter increments; **no** stream restart |
| T3.2 | 3 | `ng_latency` through VB-Cable | Measured result within ±5 ms of the estimate; RNNoise and DFN results recorded |
| T4.1 | 4 | Fixture with a +6 dB peak | Output peak ≤ −1 dBFS; no hard-clipped samples |
| T4.2 | 4 | Quiet (−35 dBFS) and loud (−10 dBFS) speech | Output speech level −18 ± 3 dBFS after settling |
| T4.3 | 4 | Unplug and replug the mic during a run | Recovers within 2 s, the UI shows status, and the saved device is reselected |
| T4.4 | 4 | Second app opens the mic while running | Succeeds (shared mode) |

---

## 5. Cross-platform and licensing concerns
- **DeepFilterNet:**
  - Code is MIT/Apache-2.0 dual-licensed. The pretrained weights sit in the same repository under the "all code in this repository" statement, with **no separate licence for the weights**. Record that in a NOTICE file and treat it as a minor ambiguity worth confirming with the author before shipping.
  - The inference engine, tract, is MIT/Apache.
  - The `capi` feature implies `default-model`, which also embeds a model the C API never uses, so binary size has to be measured.
  - Shipping adds a **Rust toolchain** (≥ 1.70) to Windows, Linux and macOS builds; CI builds macOS for both x64 and arm64. Keep DFN behind a build flag until the gate passes.
- **RNNoise** is BSD-3-Clause, and **PortAudio** has a permissive MIT-style licence. Both stay.
- **Metrics:** PESQ (ITU-T P.862) carries patent and licensing restrictions even through MIT-licensed Python wrappers, so use it only for local reference, never in CI or anything distributed. STOI and SI-SDR are unencumbered. Confirm the DNSMOS model licence in Microsoft's DNS-Challenge repo before relying on it.
- **Datasets:** VoiceBank+DEMAND and the DNS datasets are CC BY 4.0; they need attribution and should be downloaded, not committed.
- **VB-Cable:** redistribution requires VB-Audio's permission (`installer.nsh` already gates this). It is Windows/macOS only; Linux would use PipeWire.
- **Virtual-mic drivers:**
  - Windows: an EV certificate plus Microsoft attestation signing are required.
  - macOS: BlackHole is GPL-3, so don't bundle it; write a first-party AudioServerPlugIn.
  - Linux: no driver needed.
- **Platform differences:** WASAPI shared/exclusive and MMCSS are Windows-only. Wrap them behind PortAudio's host-API checks; ALSA/PulseAudio/PipeWire and CoreAudio paths keep current behaviour.

---

## 6. DeepFilterNet API reference (from the official repo, `Rikorose/DeepFilterNet`)

Checked against the README, `libDF/src/capi.rs`, `libDF/Cargo.toml`, `libDF/src/tract.rs` and the releases page.

- **C API** (`#[no_mangle] extern "C"`):

  | Function | Notes |
  |---|---|
  | `DFState* df_create(const char* path, float atten_lim, const char* log_level)` | `path` must point to a model `.tar.gz`; always mono |
  | `size_t df_get_frame_length(DFState*)` | Returns the hop size |
  | `float df_process_frame(DFState*, float* in, float* out)` | Returns the local SNR; panics on error |
  | `float df_process_frame_raw(...)` | Spectral-domain variant |
  | `void df_set_atten_lim(DFState*, float lim_db)` | |
  | `void df_set_post_filter_beta(DFState*, float beta)` | 0 = off; range 0–0.05 |
  | `char* df_next_log_msg(DFState*)` / `void df_free_log_msg(char*)` | Log retrieval |
  | `void df_free(DFState*)` | |

- **Build:**
  - feature `capi = ["tract", "default-model", "dep:ndarray", "logging"]`;
  - crate types `cdylib`, `rlib` and `staticlib`;
  - header `deep_filter.h` generated by cbindgen/cargo-c;
  - `rust-version = "1.70"`.
- **State and reset:**
  - STFT and model state persist across calls;
  - there is **no reset** in the C API, so use free + create;
  - there is **no delay compensation** in the streaming API (the CLI offers `-D`).
- **Behaviour notes from `tract.rs`:**
  - near-silent input for more than 5 frames is zero-filled (returns −15);
  - an attenuation limit of ≥ 100 dB means no limit;
  - a limit below 0.01 dB means passthrough;
  - otherwise the output is `enh·(1−lim) + noisy·lim`.
- **Models:** `models/DeepFilterNet3_onnx.tar.gz` and a low-latency `DeepFilterNet3_ll_onnx.tar.gz`. Hop size, FFT size and lookahead are read from the model's `config.ini`; they are **not stated** in the sources and are measured in Phase 1.3.
- **CLI:** `deep-filter [-D] [-m model] [-o out_dir] [--pf] files…`. It accepts 48 kHz WAV only.
- **Release:** latest is `v0.5.6`; `main` is at `0.5.7-pre`. The README states support for Linux, macOS and Windows.
