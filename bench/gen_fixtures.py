#!/usr/bin/env python3
"""
Generate deterministic synthetic 48 kHz mono fixtures for ng_bench.

Same seed + same numpy version -> bit-identical PCM16 files (checked against
the SHA-256 values recorded in the manifest when --check is given).

IMPORTANT: the "voice" here is a synthetic harmonic source with formants and a
syllabic envelope. It is NOT real speech. Use these fixtures for:
  * silence / noise-only behaviour
  * latency (impulse train)
  * clipping / headroom
  * regression (golden) tests
Speech-quality scores (STOI, DNSMOS) must come from real speech fixtures
(see fetch_voicebank.py).

Usage:
  python bench/gen_fixtures.py [--out bench/fixtures/synth] [--check]
"""

import argparse
import hashlib
import json
import os
import sys

import numpy as np
from scipy.io import wavfile
from scipy.signal import lfilter

SR = 48000
SEED = 20261024


def to_pcm16(x):
    x = np.clip(x, -1.0, 1.0)
    return np.round(x * 32767.0).astype(np.int16)


def rms(x):
    return float(np.sqrt(np.mean(np.square(x, dtype=np.float64)))) if len(x) else 0.0


def db_to_lin(db):
    return 10.0 ** (db / 20.0)


def pink_noise(rng, n):
    """Voss-McCartney-style pink noise via the Kellet IIR approximation."""
    white = rng.standard_normal(n)
    b = [0.049922035, -0.095993537, 0.050612699, -0.004408786]
    a = [1.0, -2.494956002, 2.017265875, -0.522189400]
    return lfilter(b, a, white)


def brown_noise(rng, n):
    """Fan / HVAC-like rumble: leaky-integrated white noise."""
    white = rng.standard_normal(n)
    return lfilter([1.0], [1.0, -0.995], white)


def hum(n, f0=50.0, harmonics=6):
    t = np.arange(n) / SR
    out = np.zeros(n)
    for k in range(1, harmonics + 1):
        out += (1.0 / k) * np.sin(2 * np.pi * f0 * k * t + 0.3 * k)
    return out


def synth_voice(rng, seconds):
    """Harmonic glottal-like source, vowel formants, syllable envelope, pitch drift."""
    n = int(seconds * SR)
    t = np.arange(n) / SR
    f0 = 120.0 + 25.0 * np.sin(2 * np.pi * 0.7 * t) + 8.0 * np.sin(2 * np.pi * 3.1 * t)
    phase = 2 * np.pi * np.cumsum(f0) / SR
    src = np.zeros(n)
    for k in range(1, 40):
        src += (1.0 / k) * np.sin(k * phase)

    # Formant filtering: switch vowel every ~250 ms.
    vowels = [(730, 1090, 2440), (270, 2290, 3010), (300, 870, 2240), (530, 1840, 2480)]
    seg = int(0.25 * SR)
    out = np.zeros(n)
    for i, start in enumerate(range(0, n, seg)):
        block = src[start:start + seg]
        y = np.zeros_like(block)
        for fc in vowels[i % len(vowels)]:
            r = np.exp(-np.pi * 90.0 / SR)
            w = 2 * np.pi * fc / SR
            y += lfilter([1 - r], [1, -2 * r * np.cos(w), r * r], block)
        out[start:start + seg] = y

    # Syllabic envelope (~4 Hz) with pauses, so there are speech-free gaps.
    env = np.clip(np.sin(2 * np.pi * 2.0 * t), 0, None) ** 0.6
    pauses = (np.floor(t / 1.5) % 3 == 2)  # every third 1.5 s block is silent
    env[pauses] = 0.0
    out *= env
    return out / (np.max(np.abs(out)) + 1e-12)


def mix_at_snr(clean, noise, snr_db):
    """Scale noise so that rms(clean_active)/rms(noise) == snr_db."""
    active = np.abs(clean) > 1e-4
    c_rms = rms(clean[active]) if active.any() else rms(clean)
    n_rms = rms(noise)
    noise = noise * (c_rms / (n_rms * db_to_lin(snr_db)))
    return clean + noise, noise


def normalize_peak(x, peak_dbfs):
    return x * (db_to_lin(peak_dbfs) / (np.max(np.abs(x)) + 1e-12))


def build(out_dir):
    rng = np.random.default_rng(SEED)
    files = {}

    def put(name, x, kind, **meta):
        path = os.path.join(out_dir, name)
        wavfile.write(path, SR, to_pcm16(x))
        files[name] = {"kind": kind, **meta}

    seconds = 9.0
    n = int(seconds * SR)

    voice = normalize_peak(synth_voice(rng, seconds), -6.0) * db_to_lin(-12.0) / db_to_lin(-6.0)
    put("voice_synth_clean.wav", voice, "clean", note="synthetic voice, not real speech")

    noises = {
        "pink": pink_noise(rng, n),
        "fan": brown_noise(rng, n),
        "hum": hum(n) + 0.1 * pink_noise(rng, n),
    }
    for nname, nsig in noises.items():
        for snr in (-5, 0, 5, 10, 20):
            mix, _ = mix_at_snr(voice, nsig, snr)
            put(f"voice_synth_{nname}_snr{snr:+d}.wav", mix, "mixture",
                clean="voice_synth_clean.wav", noise=nname, snr_db=snr)
        noise_only = normalize_peak(nsig, -20.0)
        put(f"noise_only_{nname}.wav", noise_only, "noise_only", noise=nname)

    put("silence_digital.wav", np.zeros(5 * SR), "silence")
    put("silence_floor_-70dBFS.wav",
        rng.standard_normal(5 * SR) * db_to_lin(-70.0), "silence", note="white floor at -70 dBFS RMS")

    # Impulse train for delay measurement: 1 click per second, 5 clicks.
    imp = np.zeros(6 * SR)
    for k in range(1, 6):
        imp[k * SR] = 0.5
    put("impulse_train.wav", imp, "impulse", interval_s=1.0, count=5)

    # Near-clipping: loud synthetic voice + pink noise, peak at -0.1 dBFS.
    loud, _ = mix_at_snr(voice, noises["pink"], 15)
    put("near_clip_voice_pink.wav", normalize_peak(loud, -0.1), "near_clip",
        clean="voice_synth_clean.wav")

    return files


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "fixtures", "synth"))
    ap.add_argument("--check", action="store_true",
                    help="verify generated files against the existing manifest hashes")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    manifest_path = os.path.join(args.out, "manifest.json")
    previous = None
    if args.check and os.path.exists(manifest_path):
        with open(manifest_path) as f:
            previous = json.load(f)

    files = build(args.out)
    for name, meta in files.items():
        meta["sha256"] = sha256(os.path.join(args.out, name))

    manifest = {"sample_rate": SR, "seed": SEED, "numpy": np.__version__, "files": files}

    if previous is not None:
        bad = [k for k, v in files.items()
               if previous["files"].get(k, {}).get("sha256") != v["sha256"]]
        if bad:
            print("MISMATCH:", ", ".join(bad), file=sys.stderr)
            sys.exit(1)
        print(f"OK: {len(files)} fixtures match manifest")
        return

    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)
    print(f"wrote {len(files)} fixtures + manifest.json to {args.out}")


if __name__ == "__main__":
    main()
