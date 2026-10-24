#!/usr/bin/env python3
"""
(Re)generate the golden regression files in test/golden/.

Only run this when a change to the processing output is INTENDED. The golden
test (test/golden.test.js) fails whenever ng_bench output drifts from these
files by more than the RMS tolerance stored in test/golden/golden.json.

Inputs are short slices of the synthetic fixtures (PCM16):
  voice_pink_snr5_3s.wav  3 s of voice + pink noise at +5 dB SNR
                          (covers the 1.5 s gate calibration and gating)
  noise_fan_2s.wav        2 s of fan noise only

Usage:
  python bench/gen_fixtures.py
  python bench/make_golden.py --bench deps/bench-build/Release/ng_bench.exe
"""

import argparse
import json
import os
import subprocess
import sys

from scipy.io import wavfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SYNTH = os.path.join(HERE, "fixtures", "synth")
GOLDEN = os.path.join(ROOT, "test", "golden")
SR = 48000
TOL = 1e-4

CLIPS = [
    ("voice_pink_snr5_3s.wav", "voice_synth_pink_snr+5.wav", 0.0, 3.0),
    ("noise_fan_2s.wav", "noise_only_fan.wav", 0.0, 2.0),
]
BACKENDS = ["rnnoise-raw", "rnnoise-chain"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bench", required=True)
    args = ap.parse_args()
    bench = os.path.abspath(args.bench)

    os.makedirs(GOLDEN, exist_ok=True)
    cases = []
    for out_name, src, start, dur in CLIPS:
        sr, x = wavfile.read(os.path.join(SYNTH, src))
        if sr != SR:
            sys.exit(f"{src}: {sr} Hz")
        clip = x[int(start * SR):int((start + dur) * SR)]
        wavfile.write(os.path.join(GOLDEN, out_name), SR, clip)

        for be in BACKENDS:
            expected = f"{os.path.splitext(out_name)[0]}.{be}.expected.wav"
            cmd = [bench, "process", "--backend", be, "--format", "s16",
                   "--in", os.path.join(GOLDEN, out_name),
                   "--out", os.path.join(GOLDEN, expected)]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                sys.exit(r.stderr)
            cases.append({"input": out_name, "backend": be, "expected": expected})

    with open(os.path.join(GOLDEN, "golden.json"), "w") as f:
        json.dump({
            "tolerance_rms": TOL,
            "format": "s16",
            "rnnoise_commit": "a9afcccb3c4f01e6e76a1ef67ccb58b7799856bf",
            "cases": cases,
        }, f, indent=2)
    print(f"wrote {len(cases)} golden cases to {GOLDEN}")


if __name__ == "__main__":
    main()
