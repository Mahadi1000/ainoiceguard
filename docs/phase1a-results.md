# Phase 1A — RNNoise vs DeepFilterNet 3 (official CLI)

**Date:** 2026-10-24 · **Branch:** `phase0-baseline` · **Machine:** HP ProDesk 400 G4 (4 cores), Windows 11
**Plan:** [implementation-plan.md](implementation-plan.md) §3 Phase 1 · **Baseline:** [phase0-baseline.md](phase0-baseline.md)

## What was compared

**Why the CLI.** Phase 1 was split because this machine has no Rust toolchain. Phase **1A** compares output quality using DeepFilterNet's **official prebuilt CLI**. Phase **1B** (the libDF C API backend, per-frame timing and the CLI cross-check) only happens if 1A justifies adding Rust.

| Item | Detail |
|---|---|
| DeepFilterNet | `deep-filter-0.5.6-x86_64-pc-windows-msvc.exe` (GitHub release v0.5.6). Embedded **DeepFilterNet3** model, "lookahead 2" per its log. **Unsigned**; SHA-256 `75e11fa16445f560cb6b021521ddb89e89270d13b83089705d98776f58fd7915` |
| RNNoise | mumble rnnoise `a9afccc` via `ng_bench` (`rnnoise-raw` = model only, `rnnoise-chain` = the app's full chain) |
| Real speech | 50 fixed pairs from the VoiceBank+DEMAND test set (48 kHz, CC BY 4.0), 124 s total. Zips: clean `b60bfc2b…`, noisy `ba4a893f…` |
| Metrics | DNSMOS P.835 (`sig_bak_ovr.onnx`, SHA-256 `269fbebd…`, CC BY 4.0), STOI (pystoi 0.4.1), SI-SDR, delay by cross-correlation |
| DFN variants | `dfn-cli` (no limit), `-pf` (post-filter), `-a18/-a12/-a6` (attenuation limits in dB), `-a12-pf` |

## Results on real speech (50 VoiceBank files)

| backend | DNSMOS SIG | DNSMOS BAK | DNSMOS OVRL | STOI | delay | RTF (1 core) |
|---|---:|---:|---:|---:|---:|---:|
| *noisy input* | 3.40 | 3.32 | 2.81 | 0.934 | — | — |
| rnnoise-raw | 3.36 | 3.84 | 3.00 | 0.929 | 10 ms | 0.030 |
| **rnnoise-chain** (app today) | 3.39 | 3.76 | **3.00** | **0.932** | 10 ms | **0.027** |
| **dfn-cli** | 3.46 | 4.09 | **3.21** | **0.951** | **30 ms** | **0.157** |
| dfn-cli-pf | 3.45 | 4.11 | 3.21 | 0.947 | 30 ms | 0.167 |
| dfn-cli-a18 | 3.47 | 3.92 | 3.13 | 0.949 | 30 ms | 0.156 |
| dfn-cli-a12 | 3.47 | 3.81 | 3.07 | 0.948 | 30 ms | 0.131 |
| dfn-cli-a6 | 3.41 | 3.60 | 2.94 | 0.944 | 30 ms | 0.131 |
| dfn-cli-a12-pf | 3.47 | 3.81 | 3.07 | 0.947 | 30 ms | 0.134 |

Paired per-file difference, DFN vs `rnnoise-chain` (mean [95% bootstrap CI], share of files where DFN is higher):

| metric | dfn-cli | dfn-cli-a18 | dfn-cli-a12 |
|---|---|---|---|
| DNSMOS OVRL | **+0.210 [+0.150, +0.274]**, 90 % | +0.132 [+0.084, +0.183], 78 % | +0.078 [+0.033, +0.124], 62 % |
| DNSMOS SIG | +0.064 [+0.025, +0.105], 62 % | +0.076 [+0.032, +0.126], 60 % | +0.075 [+0.032, +0.124], 60 % |
| DNSMOS BAK | +0.339 [+0.257, +0.425], 90 % | +0.167 [+0.109, +0.230], 76 % | +0.052 [−0.020, +0.121], 57 % |
| STOI | +0.019 [+0.012, +0.026], 82 % | +0.017 [+0.012, +0.023], 92 % | +0.016 [+0.011, +0.021], 92 % |

Reproduce with `python bench/compare.py bench/results/phase1a-voicebank`.

## Decision gate (as written in the plan)

| criterion | threshold | measured (dfn-cli) | result |
|---|---|---|---|
| DNSMOS OVRL vs rnnoise-chain | ≥ +0.3 | +0.21 (CI upper bound +0.27) | **Fail** |
| STOI vs rnnoise-chain | no worse than −0.01 | +0.019 | Pass |
| p99 frame time | ≤ 5 ms | not measurable with the CLI (needs 1B) | Unknown |
| Real-time factor (1 core) | ≤ 0.3 | 0.153 median, 0.209 worst file | Pass |
| Added algorithmic latency | ≤ 40 ms total | 30 ms (+20 ms vs RNNoise) | Pass |
| Silence / noise-only artifacts | none | silence → digital zero; no clipping, no NaN | Pass |

**Strictly by the gate as written, DFN does not proceed:** the overall-quality gain is real but below the +0.3 bar I set.

## Interpretation

- **DFN is better on almost every file.** Most of the gain is background removal (BAK +0.34). Speech quality (SIG +0.06) and intelligibility (STOI +0.02) improve by smaller but statistically significant amounts.
- **RNNoise doesn't improve intelligibility on this set.** Its STOI is at or slightly below the noisy input (0.929–0.932 vs 0.934). It mainly lowers the background (BAK 3.32 → 3.76–3.84).
- **The app's chain adds nothing perceptual on top of raw RNNoise.** DNSMOS OVRL is 3.00 for both, and BAK is slightly *lower* with the chain (3.76 vs 3.84), probably because of the comfort noise and −38 dB minimum gate gain. This is a tuning question for Phase 4, independent of DFN.
- **Costs of DFN:**
  - **~6× the CPU** (RTF 0.157 vs 0.027, i.e. ~16 % of one core vs ~3 %);
  - **+20 ms latency** (30 vs 10 ms);
  - a Rust toolchain in the build;
  - a binary of roughly 25 MB or more;
  - **unknown per-frame tail latency** until 1B.
- **Attenuation limits trade quality for naturalness as expected.** At 12 dB, DFN keeps most of its STOI and SIG gains but its BAK advantage disappears.
- **Dataset caveat.** VoiceBank+DEMAND is a comparatively easy test set (SNR 2.5–17.5 dB, stationary-ish noise). Gaps between models are usually larger on harder, non-stationary noise (keyboard, babble, traffic), which isn't tested here.
- **Synthetic-voice caveat.** DFN strongly suppresses the synthetic harmonic "voice" (STOI 0.43, −5.7 dB level) while RNNoise passes it. Since that signal isn't speech, this mainly says DFN is more aggressive on non-speech harmonic sounds (possibly music or tones). It isn't a quality verdict.
- **Latency measurement note.** DFN removes the impulse clicks entirely, so its 30 ms comes from speech cross-correlation; it was identical on all 50 files.

## Options

1. **Keep RNNoise (gate as written).** Stop Phase 1 and move to Phase 3/4 work: live metrics, AGC/limiter, devices. Revisit DFN later if real-world noise is worse than VoiceBank.
2. **Accept DFN on the consistent quality gain and go to 1B.** Install Rust, build libDF v0.5.6, measure per-frame p99 and confirm the C API matches the CLI. DFN would then be an *optional* "best quality" backend, with RNNoise kept as the default, as in the plan's Phase 2.
3. **Gather more evidence first.** Add harder noise fixtures (e.g. a DNS-Challenge noise subset, CC BY 4.0) and re-run 1A before deciding.
