# Phase 4c — Virtual microphone via VB-Cable (hardening)

**Date:** 2026-11-06 · **Branch:** `phase4c-vbcable` (from `phase4b-device-handling` @ `1be909a`)
**Decision:** VB-Cable route chosen over a first-party driver (needs the WDK, test signing and EV/attestation signing) and Linux PipeWire (not testable here). **Plan:** [implementation-plan.md](implementation-plan.md)

## 1. Clock-drift compensation (plan L8)

**Problem.** The microphone and VB-Cable run on independent clocks. A 100 ppm mismatch is 4.8 samples/s, so the output ring slowly fills (latency grows, then the ring overflows and drops audio) or drains (underruns). Nothing controlled this before.

**Design** (`native/src/drift.h`, used in `AudioEngine::writeOutput`):
- **DriftResampler:** 4-point Hermite interpolation at a ratio near 1. Exact passthrough at ratio 1 (constant 2-sample delay), and ≥ 60 dB SNR at +500 ppm on a 1 kHz tone.
- **DriftController:** PI control of the output-ring level towards **960 samples (20 ms)**, clamped to ±2,000 ppm, with a 0.5 s warm-up. Final settings after the live measurements (§4): kp = 0.0032, ki = 2e-6 (ω = 0.001 rad/frame, ζ ≈ 0.8), level smoothing ≈ 2 s (α = 0.005). Engine options: `driftCompensation` (default on) and `driftTarget` (samples, 240–3,000, for testing).
- **Continuous level:** the output callback removes one 480-sample block at a time, so the fill the producer sees is a sawtooth whose phase slides with drift. The controller uses `fill − block × (time since the last device read / block period)`, a steady-drain model, from timestamps the output callback stores. On or after a restart (a new device is a new clock), everything resets.
- Engine option `driftCompensation` (default **on**); `getMetrics().engine.drift = { enabled, ppm, level, targetLevel }`.

**How it was tuned.** These are simulations (`ng_engine_tests`, `ng_engine_tests sweep`): event-driven producer and consumer, the engine's 4,096-sample ring, 10 simulated minutes.

| version | result |
|---|---|
| first gains (kp 5e-4, ki 5e-6), raw fill | underdamped (ζ ≈ 0.08): fill swung ±450 samples; underruns at ±300 ppm |
| loop-model gains, raw fill | still chased the sawtooth: "learned" drift jumped between −21 and +2,000 ppm |
| **loop-model gains + continuous level** | **learns the exact drift** (+999.0 for +1,000 ppm, +299.9, −300.1, −1,001.0); 0 underruns, 0 overflows |

**Jitter margin** (±jitter on every producer and consumer event):

| jitter | target 720 (15 ms) | **target 960 (20 ms, chosen)** |
|---|---|---|
| 0 ms | 0 underruns | 0 underruns |
| ±2 ms (≈ worst live frame, Phase 4a) | 0 underruns | 0 underruns |
| ±4 ms | **1,334–7,285 underrun samples** | **0 underruns**, drift still learned within 2 ppm |

**Without control** the same simulation underruns at +300 ppm and overflows at −300 ppm. The tests assert this too, so they can't pass by being insensitive.

**Live:** measured with VB-Cable on 2026-11-20; see §4. The first tuning (kp 0.0128) was too jumpy on real hardware and was retuned there.

## 2. VB-Cable pairing and correct instructions

- **Bug fixed:** the UI told users to select **"CABLE Input"** as the microphone in other apps; the meter hint said the same. In VB-Cable it's the reverse: the app *plays into* **CABLE Input**, and Zoom, Discord or Meet *record from* **CABLE Output**.
- `device-choice.js` `findVirtualCable()` pairs the two ends on the same host API: VB-Cable, CABLE-A…D, VoiceMeeter (Input/Aux/VAIO3), and BlackHole on macOS (same name on both ends). A half-installed cable (one end missing) isn't treated as ready.
- The banner shows the exact microphone name other apps must select. On first run the output defaults to the cable's playback end, unless the user has saved another choice (Phase 4b).
- **Feedback-loop guard:** choosing the cable's recording end as Ainoiceguard's *own* microphone while writing into the cable would loop the output into itself. Start and restart refuse this with an explanation (`isFeedbackLoop`).
- Tests cover the pairings, host-API mismatch, a half-installed cable and the loop guard.

## 3. Loopback self-test tool

`ng_loopback` (built with `-DNG_BUILD_BENCH=ON`):
- **What it does:** plays 1 ms 2 kHz bursts on an output device and records an input device, both in WASAPI shared mode at 48 kHz mono.
- **Output:**
  - **latency per burst** (PortAudio DAC → ADC timestamps): median, min and max;
  - **bursts detected vs played** (a routing check: exit code 3 = nothing arrived);
  - **each device's clock vs the system clock, in ppm** (least-squares fit over all callback timestamps, accurate to < 20 ppm with ±2 ms jitter over 10 s) and their difference;
  - xrun counts.
- The pure maths (`loopback.h`) is unit-tested: a synthetic 37.5 ms delay through noise and a 20 ms stream offset is recovered within 0.5 ms, and synthetic clocks within 0.5 ppm (exact timestamps) and < 20 ppm (jittered).

```bash
deps/bench-build/Release/ng_loopback.exe --list
```

```bash
deps/bench-build/Release/ng_loopback.exe --out "CABLE Input" --in "CABLE Output" --seconds 20 --json bench/results/loopback-cable.json
```

## 4. Live measurements with VB-Cable (2026-11-20)

Same HP ProDesk, VB-Cable installed, WASAPI shared mode, mic = "External Microphone (Synaptics HD Audio)".

**Cable route and latency** (`ng_loopback`, CABLE Input → CABLE Output, silent):

| run | bursts | latency median | p10–p90 | min–max | sd | trend | PortAudio's estimate |
|---|---|---:|---:|---:|---:|---:|---:|
| 20 s | 79/79 | 110.6 ms | — | 99.5–133.4 ms | — | — | 44 ms |
| 30 s | 119/119 | **102.1 ms** | 99.7–105.2 ms | — | 2.1 ms | 0.001 ms/s | 44 ms |

VB-Cable adds **~100–110 ms**, about 60 ms more than PortAudio reports. It's stable within a session (sd 2 ms, no trend) and varies a little between sessions. This is most likely VB-Cable's internal buffer (its "Max Latency" setting in the VB-Cable control panel). The figure is measured but not checked against an independent reference.

**Mic vs cable clock** (`ng_loopback` into the cable while recording the mic, 2 × 60 s):

| run | mic clock | cable clock | difference |
|---|---:|---:|---:|
| 1 | +8.9 ppm | +2.9 ppm | −6.0 ppm |
| 2 | +4.7 ppm | +5.4 ppm | +0.7 ppm |

On this machine the two clocks are essentially the same (within the method's ~±6 ppm noise). USB headsets typically drift 50–300 ppm, which is what the compensation is for. (The tool's exit code now requires at least half the bursts to arrive: recording the mic matched 4–16 of 239 bursts to random room sounds.)

**Engine runs, 5 min each, mic → CABLE Input (silent):**

| run | applied correction (after 60 s) | level vs target | underrun events | drops / xruns / overruns | est. latency |
|---|---|---|---:|---|---:|
| control **off** (two sessions) | — | natural level ~590 | 1 (start) | 0 / 0 / 0 | 64.5 / ~74 ms (varies per session) |
| first tuning (kp 0.0128, α 0.02), target 960 | −128 … +300 ppm | held ~960 | 1 (start) | 0 / 0 / 0 | 80.1 ms |
| gentle gains (α 0.02), target 720 | sd 98 ppm | held ~720 | **2** | 0 / 0 / 0 | 74.9 ms |
| gentle gains (α 0.02), target 960 | sd 136 ppm | held ~960 | 1 (start) | 0 / 0 / 0 | 77.7 ms |
| **final (gentle gains, α 0.005), target 960** | **mean −9.8, sd 19.3 ppm** | 943–967 | 1 (start) | 0 / 0 / 0 | **77.6 ms** |

- **Retuning:** the live level wobbles about ±40 samples (output callbacks arrive in bursts), more than the simulation assumed. The first tuning turned that into ±100–300 ppm of needless correction. That's inaudible (300 ppm ≈ 0.5 cents of pitch) but meaningless. Gentler gains plus heavier smoothing bring it to **sd 19 ppm, centred on the real ~0–6 ppm drift.**
- **Target:** 960 rather than 720, because 720 showed an extra underrun live, and in simulation it underruns from ±3 ms jitter.
- **Cost on this machine:** with almost no drift, compensation mainly holds the ring at a fixed level. That gives ~3–13 ms more latency than control-off, depending on where a session happens to settle, in exchange for **predictable** latency and protection against real drift in long sessions or on USB devices.
- **One underrun event at start** in every run, with or without control: the first output block before the ring has filled. It's a start-up artefact, not drift.

**App** (real window, temporary settings, VB-Cable installed):
- the first run selects CABLE Input as the output, and the banner says to select "CABLE Output (VB-Audio Virtual Cable)" in Zoom, Discord or Meet;
- choosing CABLE Output as Ainoiceguard's own mic is refused, and the engine doesn't start;
- the real mic → CABLE Input starts with the latency label at "~73 ms".

**End-to-end latency for a call app** ≈ the app's engine path (~65–78 ms estimated: mic device + rings + RNNoise + output device) **+ VB-Cable's ~100 ms** ≈ **165–180 ms**. VB-Cable is the largest single contributor. That's worth noting in the UI and the docs, and it's one argument for a first-party driver later.

## Tests

All engine unit tests pass (drift, resampler, loopback maths, device selection, AGC/limiter, stats, ring buffer), plus the JavaScript tests for the cable pairing and loop guard. Run `npm test` for the full suite.
