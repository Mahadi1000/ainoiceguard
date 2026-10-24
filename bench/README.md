# Offline noise-suppression benchmark

Dev-only tooling for comparing suppression backends on identical 48 kHz mono
WAV fixtures. Nothing here is shipped with the app. See
[docs/implementation-plan.md](../docs/implementation-plan.md) (Phase 0/1).

## Pieces

| Path | Purpose |
|---|---|
| `native/bench/ng_bench.cpp` | C++ CLI: runs a backend frame-by-frame (480 samples / 10 ms), writes output WAV + timing JSON; `compare` mode for golden tests |
| `native/bench/wav_io.h` | Minimal WAV reader/writer (PCM16/24/32, float32) |
| `bench/gen_fixtures.py` | Deterministic synthetic fixtures + `manifest.json` (SHA-256) |
| `bench/fetch_voicebank.py` | Extracts a fixed 50-file real-speech subset from VoiceBank+DEMAND zips you download |
| `bench/run_bench.py` | Runs every fixture through every backend, then scores |
| `bench/score.py` | Delay, SI-SDR, STOI, DNSMOS, attenuation, clipping, timing summary |
| `bench/compare.py` | Paired per-file differences vs a baseline backend, with bootstrap CIs |
| `bench/make_golden.py` | Regenerates `test/golden/` (only when an output change is intended) |
| `test/golden.test.js` | Golden regression test (skipped if `ng_bench` is not built) |

Backends: `rnnoise-raw` (model only) and `rnnoise-chain` (the full
`RNNoiseWrapper` the app runs: HPF, blend, makeup gain, VAD gate, comfort noise).
With `--dfn-cli PATH` (the official DeepFilterNet `deep-filter` binary),
`run_bench.py` also accepts `dfn-cli`, `dfn-cli-pf`, `dfn-cli-a18`,
`dfn-cli-a12`, `dfn-cli-a6` and `dfn-cli-a12-pf`. These have wall-clock/self-reported
RTF only (no per-frame timing). DFN skips near-silent frames, so compare its
speed on speech fixtures only.

**DeepFilterNet C API (`dfn`):** build with `scripts/build-libdf.ps1`
(libDF v0.5.6 plus two patches in `native/bench/`), then pass
`--bench deps/bench-build-dfn/Release/ng_bench.exe --dfn-model <DeepFilterNet3_onnx.tar.gz>`.
This gives per-frame timing like the RNNoise backends. Use `--priority high`
for timing: at normal priority, OS pre-emption dominates the tails
(see docs/phase1b-results.md).

## Quick start (Windows)

```bash
cmake -S native -B deps/bench-build -G "Visual Studio 18 2026" -A x64 -DNG_BUILD_BENCH=ON
```

```bash
cmake --build deps/bench-build --config Release --target ng_bench
```

```bash
pip install -r bench/requirements.txt
```

```bash
python bench/gen_fixtures.py
```

```bash
python bench/run_bench.py --bench deps/bench-build/Release/ng_bench.exe --repeat 3
```

On Linux/macOS the binary is `deps/bench-build/ng_bench`.

Results land in `bench/results/<timestamp>/summary.md` (git-ignored).

## Real speech (needed for quality conclusions)

The synthetic "voice" is a harmonic source with formants, **not real speech**;
RNNoise's VAD and any learned model behave differently on it. Use it for
silence, noise, latency, clipping and regression checks only.

For STOI/DNSMOS conclusions, download `clean_testset_wav.zip` and
`noisy_testset_wav.zip` from the VoiceBank+DEMAND page
(https://doi.org/10.7488/ds/2117, CC BY 4.0), then:

```bash
python bench/fetch_voicebank.py --clean-zip PATH --noisy-zip PATH
```

```bash
python bench/run_bench.py --bench deps/bench-build/Release/ng_bench.exe --fixtures bench/fixtures/voicebank --ref-hpf80
```

## Measurement notes

- **Timing:** `--repeat N` runs N passes with fresh state; the first is a
  warm-up and is excluded. Without it the first backend run is penalised by
  CPU clock ramp-up (observed: "raw" slower than "chain").
- **Delay:** `delay_ms` is the cross-correlation lag. For `rnnoise-chain` on
  voice it reads ~11.6 ms vs exactly 10.0 ms on the impulse fixture: the extra
  ~1.6 ms is the 80 Hz high-pass's phase shift, not added latency.
- **SI-SDR:** penalises non-linear-phase filters. `--ref-hpf80` scores
  against a high-passed reference so `rnnoise-chain` is comparable. STOI and
  DNSMOS are the primary quality metrics for the Phase 1 decision gate.
- **PESQ** is deliberately not included (ITU-T P.862 licensing).
