# Phase 4c — Virtual microphone via VB-Cable (hardening)

**Date:** 2026-11-06 · **Branch:** `phase4c-vbcable` (from `phase4b-device-handling` @ `1be909a`)
**Decision:** VB-Cable route chosen over a first-party driver (needs the WDK, test signing and EV/attestation signing) and Linux PipeWire (not testable here). **Plan:** [implementation-plan.md](implementation-plan.md)

## 1. Clock-drift compensation (plan L8)

**Problem.** The microphone and VB-Cable run on independent clocks. A 100 ppm mismatch is 4.8 samples/s, so the output ring slowly fills (latency grows, then the ring overflows and drops audio) or drains (underruns). Nothing controlled this before.

**Design** (`native/src/drift.h`, used in `AudioEngine::writeOutput`):
- **DriftResampler:** 4-point Hermite interpolation at a ratio near 1. Exact passthrough at ratio 1 (constant 2-sample delay), and ≥ 60 dB SNR at +500 ppm on a 1 kHz tone.
- **DriftController:** PI control of the output-ring level towards **960 samples (20 ms)**, clamped to ±2,000 ppm, with a 0.5 s warm-up. Gains come from the loop model (ω = 0.004 rad/frame, ζ ≈ 0.8).
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

**Live:** the muted (mic-only) path is unaffected (drift `enabled: false`, 0 drops, 32 ms). **Real drift hasn't been measured yet**: it needs VB-Cable (see §4).

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

## 4. Waiting on VB-Cable (not measured yet)

After VB-Cable is installed (vb-audio.com/Cable, free; may need a reboot):
1. `ng_loopback` CABLE Input → CABLE Output: the cable's latency and whether it's working.
2. `ng_loopback --out "CABLE Input" --in "External Microphone" …`: no burst will be detected (exit 3), but the two `clock_ppm` values give **the real mic-vs-cable drift**.
3. Run the engine with the mic → CABLE Input for ≥ 5 min and read `engine.drift.ppm` (should match item 2), `engine.ring.outputUnderrunEvents` (expect 0 after warm-up) and `outputDroppedSamples` (expect 0).
4. In the app: the banner shows "CABLE Output (VB-Audio Virtual Cable)"; picking it as the app's own mic is refused.

## Tests

All engine unit tests pass (drift, resampler, loopback maths, device selection, AGC/limiter, stats, ring buffer), plus the JavaScript tests for the cable pairing and loop guard. Run `npm test` for the full suite.
