#!/usr/bin/env python3
"""
Acceptance checks for the AGC + limiter (plan T4.1 / T4.2) on real speech.

Builds ~30 s test signals from the VoiceBank clean subset (fetch_voicebank.py),
runs ng_bench rnnoise-chain with AGC on and off, and checks:

  T4.2  speech at -35 dBFS and -10 dBFS (active-speech RMS), and -35 dBFS
        speech in pink noise at 5 dB SNR, comes out at -18 +/- 3 dBFS once
        settled (measured over the last 15 s)
  T4.1  speech with +6 dBFS peaks (float WAV, peaks at 2.0) comes out with
        peak <= -1 dBFS and no samples at or above 0.999 (no clipping)

Active speech = 10 ms frames within 40 dB of the loudest frame of the clean
signal (the same mask is applied to the output after delay alignment).

Usage:
  python bench/agc_check.py --bench deps/bench-build/Release/ng_bench.exe
Exit code 0 = all checks passed.
"""

import argparse
import json
import os
import subprocess
import sys

import numpy as np
from scipy.io import wavfile
from scipy.signal import lfilter

HERE = os.path.dirname(os.path.abspath(__file__))
SR = 48000
FRAME = 480
DELAY = 480  # RNNoise algorithmic delay (samples), measured in Phase 0


def frames_rms_db(x):
    n = len(x) // FRAME
    f = x[: n * FRAME].reshape(n, FRAME)
    return 20 * np.log10(np.sqrt(np.mean(f * f, axis=1)) + 1e-12)


def active_mask(clean):
    db = frames_rms_db(clean)
    return db > db.max() - 40.0


def active_level_db(x, mask):
    n = min(len(mask), len(x) // FRAME)
    f = x[: n * FRAME].reshape(n, FRAME)[mask[:n]]
    return 20 * np.log10(np.sqrt(np.mean(f * f)) + 1e-12)


def build_speech(seconds=30.0):
    clean_dir = os.path.join(HERE, "fixtures", "voicebank", "clean")
    if not os.path.isdir(clean_dir):
        sys.exit("VoiceBank subset not found; run bench/fetch_voicebank.py first")
    parts, total = [], 0
    for name in sorted(os.listdir(clean_dir)):
        sr, x = wavfile.read(os.path.join(clean_dir, name))
        assert sr == SR
        x = x.astype(np.float64) / 32768.0 if x.dtype == np.int16 else x.astype(np.float64)
        parts.append(x)
        total += len(x)
        if total >= seconds * SR:
            break
    return np.concatenate(parts)


def run(bench, src, out, agc):
    r = subprocess.run([bench, "process", "--backend", "rnnoise-chain", "--agc", "1" if agc else "0",
                        "--in", src, "--out", out, "--json", out + ".json"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(r.stderr)
    sr, y = wavfile.read(out)
    return y.astype(np.float64)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bench", required=True)
    ap.add_argument("--out", default=os.path.join(HERE, "results", "agc-check"))
    args = ap.parse_args()
    bench = os.path.abspath(args.bench)
    os.makedirs(args.out, exist_ok=True)

    speech = build_speech()
    mask = active_mask(speech)
    base_db = active_level_db(speech, mask)
    settle = int(15 * SR / FRAME)  # measure over the last 15 s
    tail = np.zeros_like(mask)
    tail[-settle:] = True

    # Stationary pink noise (Kellet IIR), for the speech-in-noise case.
    rng = np.random.default_rng(7)
    pink = lfilter([0.049922035, -0.095993537, 0.050612699, -0.004408786],
                   [1.0, -2.494956002, 2.017265875, -0.522189400], rng.standard_normal(len(speech)))

    results, ok = {}, True
    cases = (("quiet_-35dBFS", -35.0, None), ("loud_-10dBFS", -10.0, None),
             ("quiet_-35dBFS_pink_snr5", -35.0, 5.0))
    for label, level_db, snr in cases:
        x = speech * 10 ** ((level_db - base_db) / 20)
        if snr is not None:
            n_rms = np.sqrt(np.mean(pink ** 2))
            x = x + pink * (10 ** ((level_db - snr) / 20) / n_rms)
        src = os.path.join(args.out, label + ".wav")
        wavfile.write(src, SR, x.astype(np.float32))
        for agc in (True, False):
            y = run(bench, src, os.path.join(args.out, f"{label}.agc{int(agc)}.out.wav"), agc)
            y_al = y[DELAY:]
            out_db = active_level_db(y_al, mask & tail)
            key = f"{label} agc={'on' if agc else 'off'}"
            passed = bool(abs(out_db - -18.0) <= 3.0) if agc else None
            results[key] = {"input_dbfs": level_db, "output_speech_dbfs": round(float(out_db), 2),
                            "T4.2_pass": passed}
            if agc and not passed:
                ok = False

    # T4.1: +6 dBFS peaks
    hot = speech * (2.0 / np.max(np.abs(speech)))
    src = os.path.join(args.out, "hot_+6dBFS_peaks.wav")
    wavfile.write(src, SR, hot.astype(np.float32))
    for agc in (True, False):
        y = run(bench, src, os.path.join(args.out, f"hot.agc{int(agc)}.out.wav"), agc)
        peak = float(np.max(np.abs(y)))
        clipped = int(np.count_nonzero(np.abs(y) >= 0.999))
        passed = bool(peak <= 10 ** (-1 / 20) + 1e-6 and clipped == 0)
        results[f"hot_+6dBFS agc={'on' if agc else 'off'}"] = {
            "input_peak_dbfs": 6.02, "output_peak_dbfs": round(float(20 * np.log10(peak + 1e-12)), 2),
            "clipped_samples": clipped, "T4.1_pass": passed}
        ok = ok and passed

    with open(os.path.join(args.out, "agc_check.json"), "w") as f:
        json.dump(results, f, indent=2)
    for k, v in results.items():
        print(f"{k:28} {v}")
    print("ALL PASSED" if ok else "FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
