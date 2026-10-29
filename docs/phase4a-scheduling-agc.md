# Phase 4a — Processing-thread scheduling, AGC and limiter

**Date:** 2026-10-27 · **Branch:** `phase4-scheduling-agc` (from `phase3-live-metrics` @ `b2f9b9b`)
**Plan:** [implementation-plan.md](implementation-plan.md) §3 Phase 4 · **Evidence that set the order:** [phase3-live-metrics.md](phase3-live-metrics.md)

Phase 4 in the plan has three parts: AGC/limiter, reliable device handling, and the virtual mic. Phase 3 found **silently dropped mic audio** caused by processing-thread scheduling, so this part starts with that fix and then adds the AGC and limiter. Device handling and the virtual mic follow in Phase 4b.

## 1. Processing-thread scheduling

**Change** (`native/src/audio.cpp`, `wake_event.h`, `binding.gyp` adds `avrt.lib`):
- The processing thread registers with **MMCSS "Pro Audio"** (`AvSetMmThreadCharacteristicsW`, priority HIGH) on Windows. This is best effort; if registration fails, the thread keeps normal priority.
- **Event wake-up instead of a 0.5 ms sleep-poll.** The capture callback signals a non-blocking auto-reset event (`SetEvent`; `sem_post` / `dispatch_semaphore` elsewhere). The processing thread waits with a 20 ms timeout, so the watchdog still runs.
- `getMetrics().engine.scheduling` reports `{ mmcss, wake }`.
- **Scope:** Linux and macOS get the event wake-up but no priority change yet.

**Measured** (4 × 20 s, WASAPI mic, output muted, Krisp running in the background in both cases; "stress" = 4 busy processes on 4 cores):

| run | frames | mean | worst frame | frames > 10 ms | mic samples dropped |
|---|---:|---:|---:|---:|---:|
| before, idle | 1988 / 1986 | 1808 / 1556 µs | 118.1 / 115.8 ms | 8 / 9 | **5505 / 5985** |
| before, stress | 1987 / 1986 | 440 / 572 µs | 15.2 / 47.5 ms | 2 / 4 | **5505 / 5505** |
| **after, idle** | 1999 / 1999 | 1057 / 988 µs | **2.0 / 1.9 ms** | **0 / 0** | **0 / 0** |
| **after, stress** | 2000 / 1997 | 287 / 295 µs | **0.4 / 1.6 ms** | **0 / 0** | **0 / 0** |

- **Before:** roughly once per 20 s the normal-priority thread didn't run for over 100 ms, and ~115 ms of mic audio was lost each time. Short 3 s runs didn't show it.
- **After:** no audio is dropped, no frame exceeds 10 ms, and the worst frame is ≤ 2 ms, both idle and under full load.
- The remaining ~1 ms idle mean is CPU frequency scaling (idle cores clock down); it's well within budget.

## 2. Speech AGC and peak limiter

**Change** (`native/src/dynamics.h`, `rnnoise_wrapper.{h,cpp}`):

The chain is now HPF → RNNoise → blend → **AGC** → gate → comfort noise → **limiter**.

- **SpeechAgc:**
  - **Target and range:** −18 dBFS speech RMS. Range +18 / −12 dB; the plan's ±12 dB couldn't meet its own T4.2 case of −35 dBFS input.
  - **Speed:** gain slews at 6 dB/s up and 12 dB/s down, ramped across each frame, and starts at the old +2 dB makeup gain.
  - **Adaptation:** only on frames where VAD ≥ 0.6 **and** the input level (before RNNoise) is ≥ 6 dB above a minimum-statistics noise floor (1.6 s window, fully warmed up before any adaptation).
- **PeakLimiter:** zero latency, −1 dBFS ceiling, instant attack, 60 ms release. Replaces the hard `std::clamp(±1.0)`. The output provably never exceeds the ceiling, since the gain is always ≤ ceiling/|x|.
- **Fallback:** `setAgcEnabled(false)` gives the old fixed +2 dB plus the limiter. **That output is bit-identical to the pre-Phase-4 chain** on the voice golden clip (RMS diff 0), so it is an exact fallback.
- **API:**
  - Addon: `setAgcEnabled` / `getAgcEnabled`.
  - `getMetrics()`: `agcEnabled`, `agcGainDb`, `limiterReductionDb`, `limitedFrames`.
  - `ng_bench`: `--agc 1|0`; `run_bench.py` backend `rnnoise-chain-noagc`.
- **Not done:** no UI control yet.

**Why the AGC needs the noise-floor gate.** This was found during this phase. On stationary pink noise, **RNNoise's VAD reports 0.845 on average**. With VAD as the only condition, the AGC boosted pure noise by **+14.6 dB**. Tracking the floor on RNNoise's *output* wasn't enough (still +12 dB), because RNNoise's own suppression makes its output swell and dip. Tracking it on the *input* fixed it:

| noise-only input | AGC boost, VAD-only | floor on output | **final (floor on input + warm-up)** |
|---|---:|---:|---:|
| pink | +14.6 dB | +12.0 dB | **0.0 dB** |
| fan (brown) | 0.0 dB | 0.0 dB | 0.0 dB |
| hum | 0.0 dB | 0.0 dB | 0.0 dB |
| −70 dBFS floor | 0.0 dB | 0.0 dB | 0.0 dB |

## 3. Acceptance tests (`python bench/agc_check.py`, 30 s of real VoiceBank speech)

| ID | case | AGC on | AGC off | result |
|---|---|---:|---:|---|
| T4.2 | speech at −35 dBFS | **−18.4 dBFS** | −33.6 dBFS | Pass (−18 ± 3) |
| T4.2 | speech at −10 dBFS | **−17.2 dBFS** | −13.1 dBFS (limiter working hard) | Pass |
| T4.2 | −35 dBFS speech in pink noise, 5 dB SNR | **−20.5 dBFS** | −33.9 dBFS | Pass |
| T4.1 | speech with +6 dBFS peaks | **−1.0 dBFS peak, 0 clipped samples** | same | Pass |

**Quality on the 50 VoiceBank files** (chain with AGC vs without, paired):
- DNSMOS OVRL −0.004 [−0.014, +0.004], SIG −0.004, BAK −0.006: none significant.
- STOI −0.001 [−0.001, −0.000].
- Peak max −1.00 dBFS; 0 clipped samples.
- Caveat: VoiceBank clips are ~2.5 s, and the AGC only adapts after its 1.6 s warm-up. So this set shows the AGC is harmless when it barely moves, but **it doesn't measure the AGC's effect on long speech.** An earlier version that adapted from the start cost OVRL −0.016 and STOI −0.002 (significant but tiny), probably from gain movement within an utterance.

**Unit tests** (`ng_engine_tests`):
- limiter: ceiling is never exceeded on +6 dBFS input, and it is transparent after release;
- AGC: −35 dBFS and −10 dBFS converge to −18 ± 1 dB;
- AGC: the +18 dB range limit holds;
- AGC: gain is held on non-speech;
- AGC: **stationary noise with VAD 0.85 is not boosted** (regression test);
- AGC: the gain ramp starts at the previous frame's gain.

All 15 JS/native tests pass. The RNNoise-chain golden files were regenerated deliberately; `rnnoise-raw` goldens are unchanged.

## 4. Open points

1. **The live room check is inconclusive.** With nobody known to be speaking, RNNoise's VAD read 0.99 at −42 dBFS input here, and the AGC rose to +9.8 dB in the first 2–4 s, then held. Real sound in the room (speech, a video) would make that correct. Please check: start the app in a silent room, wait 10 s, and read `agcGainDb` (expected to stay near +2 dB). If it rises with no sound, raise `minSnrDb` (6 dB) or the VAD threshold.
2. **The AGC's quality on long speech** needs ≥ 30 s real-speech fixtures in noise, scored with DNSMOS at matched loudness, because DNSMOS is level-sensitive.
3. **AGC is on by default.** It's your call whether to keep that, or ship it off until point 1 is confirmed. There's no UI toggle yet; adding one belongs with settings persistence in Phase 4b.
4. **Not yet done:** MMCSS-equivalent priority on Linux and macOS.
