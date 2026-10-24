# Phase 1B — DeepFilterNet 3 via the libDF C API

**Date:** 2026-10-24 · **Branch:** `phase1b-dfn-capi` (from `phase0-baseline`) · **Machine:** HP ProDesk 400 G4 (4 cores), Windows 11
**Plan:** [implementation-plan.md](implementation-plan.md) §3 Phase 1 · **Previous:** [phase1a-results.md](phase1a-results.md) (official CLI)

## What was built

| Item | Detail |
|---|---|
| libDF | DeepFilterNet tag **v0.5.6** (`978576a`), `cargo rustc -p deep_filter --release --lib --features capi --crate-type staticlib`, Rust 1.99.1 MSVC, `+crt-static` to match the app's `/MT`. Output: `df.lib`, 77 MB (includes the embedded default model) |
| Patches | **`libdf-v0.5.6-cargo-lock.patch`:** `time` 0.3.28 → 0.3.36. 0.3.28 fails to compile on Rust ≥ 1.80 (E0282). It's only used for log timestamps. |
| | **`libdf-v0.5.6-capi-thresholds.patch`:** `df_create` uses the CLI's stage thresholds (−15/35/35) instead of `default_with_ch()`'s −10/30/20. One line; see below. |
| Integration | `ng_bench` backend `dfn` (`-DNG_WITH_DFN=ON`, off by default). Hand-written declarations in `native/bench/dfn_capi.h` for the v0.5.6 API: `df_create(path, atten_lim)` with two arguments, unlike later versions. One `DFState` per pass, kept across frames. Frame length is read from `df_get_frame_length()` (480). |
| Reproduce | `scripts/build-libdf.ps1`: clone at tag, verify commit, apply patches idempotently, build, resolve system libs, build `ng_bench` |
| Model | `models/DeepFilterNet3_onnx.tar.gz` from the tagged source (8.0 MB), loaded in ~270–400 ms |

## Correctness: C API vs official CLI (plan T1.1)

| Comparison (file p232_001, delay-aligned) | SI-SDR between outputs | max abs diff |
|---|---:|---:|
| C API (unpatched) vs CLI defaults | 4.8 dB | 0.36 |
| C API (unpatched) vs CLI with `--min-db-thresh=-10 --max-db-erb-thresh=30 --max-db-df-thresh=20` | **72.4 dB** | 3.1e-5 |
| **C API (patched) vs CLI defaults** | **75.0 dB** | 3.1e-5 (one PCM16 LSB) |

The integration is exact. The only differences between the two paths at v0.5.6 are the default stage thresholds. Those decide when the network is skipped, based on each frame's estimated local SNR.

### Why the threshold patch is needed

Across all 50 files, the unpatched C API damages speech, and the official CLI forced to the same thresholds reproduces it. So it's the configuration, not the integration:

| | DNSMOS OVRL | DNSMOS SIG | STOI |
|---|---:|---:|---:|
| noisy input | 2.81 | 3.40 | 0.934 |
| C API, unpatched (−10/30/20) | 2.62 | 2.90 | **0.847** |
| CLI forced to −10/30/20 | 2.61 | 2.89 | 0.847 |
| C API, patched (−15/35/35) | **3.21** | **3.46** | **0.951** |

The patched values match what later upstream `df_create()` uses (checked in `main`'s `capi.rs`), so the patch aligns v0.5.6 with upstream rather than inventing new behaviour.

## Results: 50 VoiceBank+DEMAND files

**Quality** (patched C API vs `rnnoise-chain`, paired; reproduce with `python bench/compare.py bench/results/phase1b-voicebank`):

| metric | rnnoise-chain | dfn (C API) | difference [95% CI] | DFN higher in |
|---|---:|---:|---|---:|
| DNSMOS OVRL | 3.00 | 3.21 | **+0.214 [+0.154, +0.279]** | 90 % |
| DNSMOS SIG | 3.40 | 3.46 | +0.068 [+0.029, +0.109] | 62 % |
| DNSMOS BAK | 3.76 | 4.10 | +0.341 [+0.258, +0.426] | 92 % |
| STOI | 0.932 | 0.951 | +0.019 [+0.012, +0.026] | 84 % |

These are identical to the Phase 1A CLI numbers, as expected given the 75 dB match.

**Per-frame processing time** (10 ms budget per 480-sample frame, `--repeat 3`, 2 measured passes = 24,856 frames per backend):

| run | backend | mean | p50 | p99 (median file) | p99 (worst file) | max | frames > 10 ms | RTF |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| normal priority | rnnoise-chain | 353 µs | 280 µs | 1.0 ms | 10.4 ms | 66.4 ms | 16 | 0.035 |
| normal priority | dfn | 1683 µs | 1476 µs | 3.1 ms | 9.1 ms | 32.1 ms | 21 | 0.169 |
| **high priority** | rnnoise-chain | 276 µs | 239 µs | 0.33 ms | 1.07 ms | 1.44 ms | **0** | 0.028 |
| **high priority** | **dfn** | **1483 µs** | 1482 µs | **1.83 ms** | **2.62 ms** | **2.85 ms** | **0** | **0.149** |

At normal priority, both backends showed tail spikes of the same kind: RNNoise, at a fifth of DFN's cost, had the larger maximum. So the spikes are OS pre-emption, not model cost. Running at `HIGH_PRIORITY_CLASS` (closer to the MMCSS audio thread the app should use, plan L9) removes them entirely. DFN's worst frame is then 2.85 ms, under 30 % of the budget.

**Latency:** 30 ms algorithmic (one hop of STFT delay, 10 ms, plus a 2-frame lookahead, 20 ms), measured identically on all 50 files and matching the CLI's own formula. That is +20 ms over RNNoise.

## Decision gate, updated with 1B measurements

| criterion | threshold | measured (dfn, C API, patched) | result |
|---|---|---|---|
| DNSMOS OVRL vs rnnoise-chain | ≥ +0.3 | +0.214 [+0.154, +0.279] | **Fail** |
| STOI vs rnnoise-chain | no worse than −0.01 | +0.019 | Pass |
| p99 frame time | ≤ 5 ms | 2.62 ms worst file (high priority); 9.1 ms (normal) | Pass at high priority |
| Real-time factor (1 core) | ≤ 0.3 | 0.149–0.169 | Pass |
| Added algorithmic latency | ≤ 40 ms total | 30 ms | Pass |
| C API matches official CLI | SI-SDR > 40 dB | 75.0 dB (after patch) | Pass |
| Silence / noise artifacts, NaN | none | none | Pass |

**Outcome:** the only criterion DFN misses is the +0.3 DNSMOS OVRL bar, the same result as Phase 1A. Everything that 1B could newly measure (correctness, per-frame cost, tails) passes, provided processing runs at elevated priority.

## Costs if DFN were integrated (Phase 2)

- **CPU:** ~5.4× RNNoise (1.48 vs 0.28 ms per frame), about 15 % of one core.
- **Latency:** +20 ms, for 30 ms total.
- **Build:** a Rust toolchain for every platform, plus two small patches to maintain until upgrading past v0.5.6.
- **Size:** `df.lib` is 77 MB static. The shipped binary would be smaller after linking, but must be measured. The model is 8 MB.
- **Robustness:** a Rust panic (bad model file, invalid input) aborts the whole process. The backend checks the model path first, but Phase 2 would also need input sanitising (NaN/Inf) and possibly process isolation.
- **Patched upstream code:** the threshold patch means we no longer run *exactly* the released binary; it's documented and matches later upstream behaviour.

## Reproduce

```bash
powershell -ExecutionPolicy Bypass -File ./scripts/build-libdf.ps1
```

```bash
python bench/run_bench.py --bench deps/bench-build-dfn/Release/ng_bench.exe --dfn-model deps/downloads/deepfilternet-src/models/DeepFilterNet3_onnx.tar.gz --dnsmos-model deps/downloads/dnsmos/sig_bak_ovr.onnx --fixtures bench/fixtures/voicebank --backends rnnoise-chain dfn --repeat 3 --priority high
```
