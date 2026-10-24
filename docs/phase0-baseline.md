# Phase 0 — RNNoise Baseline Results

**Date:** 2026-10-24 · **Branch:** `phase0-baseline` (from clean local `main` @ `848a128`, not `origin/main`)
**Plan:** [implementation-plan.md](implementation-plan.md) · **Tooling:** [bench/README.md](../bench/README.md)

## Environment

| Item | Value |
|---|---|
| Machine | HP ProDesk 400 G4 MT, 4 physical cores, Windows 11 Pro 26100 |
| Compiler | MSVC (Visual Studio 18 2026), Release, static CRT |
| RNNoise | mumble-voip/rnnoise @ `a9afcccb3c4f01e6e76a1ef67ccb58b7799856bf` (pinned) |
| Fixtures | 22 synthetic inputs from `bench/gen_fixtures.py` (seed 20261024, numpy 2.2.6) |
| Timing | `--repeat 3` (one warm-up pass excluded, 2 measured passes per file), idle machine |

## Results (synthetic fixtures)

| backend | delay (impulse) | delay (xcorr, voice) | noise attenuation | silence out | clipped | NaN | frame mean | worst-file p99 | max frame | RTF | overruns > 10 ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| rnnoise-raw | **10.0 ms** | 9.9 ms | −26.0 dB | −166.8 dBFS | 0 | 0 | 216 µs | 2265 µs | 3349 µs | 0.022 | 0 |
| rnnoise-chain | **10.0 ms** | 11.6 ms ¹ | −21.7 dB | −92.0 dBFS ² | 0 | 0 | 225 µs | 2308 µs | 2564 µs | 0.023 | 0 |

Noise attenuation per noise type:

| | pink | fan (brown) | hum 50 Hz + harmonics |
|---|---:|---:|---:|
| rnnoise-raw | −13.8 dB | −38.1 dB | −26.0 dB |
| rnnoise-chain | −11.6 dB | −33.4 dB | −20.0 dB |

SI-SDR of the output, each backend scored against its matching reference (clean for raw, 80 Hz high-passed clean for chain ³):

| input SNR | −5 | 0 | +5 | +10 | +20 dB |
|---|---:|---:|---:|---:|---:|
| rnnoise-raw (vs clean) | −2.8 | 4.4 | 8.8 | 12.2 | 15.2 |
| rnnoise-chain (vs HPF clean) | 0.2 | 5.8 | 9.8 | 12.9 | 15.7 |

1. The chain's cross-correlation delay includes the 80 Hz high-pass's phase shift. The impulse fixture shows its true latency is 10.0 ms, the same as raw RNNoise.
2. Comfort noise in the chain is intentional (about −70 dBFS before gating); raw RNNoise outputs effectively digital silence.
3. SI-SDR penalises non-linear-phase filters, so scoring the chain against an unfiltered reference shows a misleading ~11 dB loss at +20 dB SNR. Against the matching reference, the chain's post-processing (blend, makeup gain, gate, comfort noise) costs **no** measurable waveform fidelity.

## Findings

- **CPU is not the constraint.** RNNoise uses ~2.2 % of one core in real time (mean ~220 µs per 10 ms frame). Even the worst frame (3.3 ms) is a third of the budget.
- **Tail latency is OS jitter, not the model.** p99 per file is ~10× the mean. A run made while a build was compiling showed 5 overruns and a 18.9 ms max frame, so live measurements (Phase 3) must record per-frame timing and overruns, not just averages.
- **The chain's extra stages are nearly free:** +9 µs per frame over raw RNNoise.
- **The chain attenuates noise 2–6 dB less than raw RNNoise** on noise-only input. That is mostly the 1.5 s calibration window (gate held open) plus the −38 dB minimum gate gain and comfort noise. It's a tuning trade-off to evaluate on real speech, not a defect.
- **Algorithmic delay is exactly 10 ms** (480 samples), the reference point for the DeepFilterNet comparison in Phase 1.
- **Speech level:** the chain lands +0.8 dB vs clean (its +2 dB makeup gain minus RNNoise's reduction); raw RNNoise is −1.6 dB.

## What this baseline does *not* show yet

- **Speech quality.** The synthetic voice is not real speech; STOI and DNSMOS are deliberately left out of the tables above. Quality conclusions need the VoiceBank+DEMAND subset (`bench/fetch_voicebank.py`), STOI (`pystoi`) and DNSMOS, which all require downloads you have to approve.
- **Linux/macOS goldens.** The golden files were produced with MSVC. The `bench-golden` CI job checks them on Linux/GCC; that hasn't run yet because nothing is pushed while the GitHub account is being secured.

## Acceptance checks (from the plan)

| ID | Check | Result |
|---|---|---|
| T0.1 | `scripts/build-native.ps1` with bench off; addon loads | Pass: build succeeds with pinned RNNoise; addon loads in Node and Electron 33.4.11 |
| T0.2 | `ng_bench` deterministic | Pass: repeated runs are byte-identical (`cmp`) |
| T0.3 | Golden test within 1e-4 RMS | Pass: 4 cases; drift is detected (level 0.95 → RMS diff 3.7e-3 → fail) |
| T0.4 | Metric sanity (clean vs clean) | Pass: SI-SDR 226.6 dB, delay 0 |
| — | Fixture determinism | Pass: `gen_fixtures.py --check` matches all 23 SHA-256 hashes |
