# Phase 3 — Live Engine Metrics

**Date:** 2026-10-24 · **Branch:** `phase3-live-metrics` (from `phase1b-dfn-capi` @ `cfd4499`)
**Plan:** [implementation-plan.md](implementation-plan.md) §3 Phase 3 · **Decision context:** [phase1b-results.md](phase1b-results.md)

The engine design is unchanged: the same PortAudio streams, rings, processing thread and RNNoise chain, and the UI is untouched. Phase 3 adds measurement, plus the one behaviour change the plan called for (3.1).

## What changed

| Plan item | Change | Files |
|---|---|---|
| 3.1 | **xruns are counted, not restarted on.** PortAudio over/underflow flags used to stop and reopen both streams, a 0.1–3.1 s outage per xrun. Restarts now come only from a **device-loss watchdog**: a stream stopped by PortAudio, or no capture callback for 1 s, checked every 250 ms. After a failed restart the watchdog keeps retrying, so a replugged device recovers. | `native/src/audio.{h,cpp}` |
| 3.2 | Lock-free counters (`EngineStats`, one writer per field): xrun flags, callback counts, **capture/output ring drops** (previously silent), **output underruns after priming**, ring fill (mean/min/max), **per-frame processing time** (histogram 100 µs…10 ms+, mean, max, overruns > 10 ms), restarts, restart failures, device-loss events. | `native/src/engine_stats.h` |
| 3.3 | **Latency estimate:** PortAudio-reported device latency (in + out) + mean wait in each ring + 10 ms RNNoise frame. Also records what was *actually* opened: device, host API, exclusive vs shared. | `native/src/audio.cpp`, `addon.cc` |
| 3.4 | Main-process CPU % (`app.getAppMetrics()`) in `audio:get-metrics`. One JSON line per session is appended to `<logs>/engine-sessions.jsonl` on stop/quit, plus a one-line console summary. | `electron/main.js`, `electron/session-stats.js` |
| 3.5 | Everything is under a new `engine` key in `getMetrics()` (`process.cpuPercent` added by `main.js`); **all existing fields are unchanged**. | `native/src/addon.cc` |

`getMetrics().engine` shape: `running`, `streams` {input/output Device, Name, HostApi, Exclusive; sampleRate; ringCapacity}, `xruns`, `callbacks`, `ring` {fills, drops, underruns}, `processing` {frames, meanUs, maxUs, budgetUs, overruns, histogramUpperUs, histogramCounts}, `latency` {inputDeviceMs, captureRingMs, backendMs, outputRingMs, outputDeviceMs, estimatedTotalMs}, `recovery` {restarts, restartFailures, deviceLossEvents}. Counters are cumulative since the last `start()`.

## Live findings on this machine

Capture-only runs (output muted). No VB-Cable is installed here, and playing the mic to the speakers risks feedback. Same HP ProDesk, Electron 33.4.11.

| run | device opened | estimated latency (capture side) | processing mean / max | frames > 10 ms | mic samples dropped | xruns | restarts |
|---|---|---:|---:|---:|---:|---:|---:|
| `start(-1, -2)`, 5 s, idle | **MME** "External Microphone (Synaptics" (the default) | 41.6 ms (20 dev + 11.6 ring + 10) | 1019 µs / 47.2 ms | 5 | **705** | 0 | 0 |
| `start(5, -2)`, 5 s, idle | WASAPI "External Microphone (Synaptics HD Audio)", **shared** | 32.7 ms (22 + 0.7 + 10) | 1153 µs / 17.4 ms | 2 | 0 | 0 | 0 |
| `start(5, -2)`, 10 s, **all 4 cores busy** | WASAPI, shared | — | 390 µs / 5.4 ms | 0 | **6465 (135 ms)** | 0 | 0 |

What this shows (each item is a measurement, not an inference, unless noted):

1. **The default device path is MME, not WASAPI.** `Pa_GetDefaultInputDevice()` returns the default host API's device, which is MME on Windows. That costs ~9 ms more latency, and the WASAPI exclusive-mode code never applies to it.
2. **WASAPI exclusive mode was never obtained** (`inputExclusive: false`), even on the WASAPI device. The "exclusive first" path silently falls back to shared mode here. This supports making shared the default in Phase 4 (L7).
3. **Audio is silently lost, without any xrun.** The capture ring overflowed in both the MME start-up run (705 samples) and the CPU-stress run (6,465 samples, ~135 ms). PortAudio raised no status flag either time, so the old restart logic would not have reacted and nothing recorded it. *Hypothesis:* the MME case is a start-up burst before the processing thread drains the ring; the stress case is the normal-priority processing thread being starved.
4. **Live processing is ~4× slower than the offline benchmark when idle** (≈1.0–1.15 ms vs 0.27 ms per frame), with 17–47 ms stalls. Under full CPU load the mean drops to 390 µs. That points to CPU frequency scaling and cold caches: the thread sleeps 0.5 ms between frames. Together with Phase 1B (no stalls at high priority), this makes **processing-thread priority / MMCSS and event-driven wake-ups (L9) the top Phase 4 engine fix.**
5. **The watchdog did not fire falsely** under full CPU load (0 device-loss events, 0 restarts).

## Tests

| ID | Test | Result |
|---|---|---|
| T3.1 | xruns/stress do not restart streams | Pass (stress run: 0 restarts, 0 device-loss events). PortAudio-level xruns could not be induced here, so the counting path for them is covered by code review only. |
| — | `ng_engine_tests` (bucket bounds, overruns, fill min/max with output off, reset, ring buffer full/wrap/underrun) | Pass, in `npm test` and the `bench-golden` CI job |
| — | `test/session-stats.test.js` (summary, missing data, one-line format) | Pass |
| — | Golden RNNoise output unchanged | Pass (4/4) |
| T3.2 | Measured latency within ±5 ms of the estimate | **Not done:** needs a second capture path (VB-Cable or loopback) that this machine lacks; see below |

All 15 tests pass (`npm test`).

## Not covered yet (needs your hardware / setup)

1. **Output side (output underruns, output ring fill, drift):** run the app with headphones, mic → speakers/headphones, or install VB-Cable. Then read `engine.ring.outputUnderrunEvents`, `outputFillMin/Max` (drift shows as a slowly rising or falling fill) and `latency.estimatedTotalMs`.
2. **Measured end-to-end latency (T3.2):** with VB-Cable installed and the app's output set to *CABLE Input*:
   - record the raw mic and *CABLE Output* simultaneously as two tracks (e.g. Audacity, WASAPI, two devices);
   - clap or click near the mic;
   - the offset between the two tracks is the app's added latency;
   - compare it with `latency.estimatedTotalMs` (expect within ±5 ms).
3. **Device-loss recovery:** during a session, unplug the mic (or disable it in Sound settings), then reconnect it. Expect `recovery.deviceLossEvents ≥ 1` and `restarts ≥ 1` once it's back, and a session line in `engine-sessions.jsonl` (in Electron's logs directory) recording it.
4. **The Electron wiring** (`stopEngineAndLog`, `process.cpuPercent`) is syntax-checked and its pure logic is unit-tested, but it hasn't been exercised by clicking through the real app window. Start and stop once in `npm start`, then check that the session line appears.
