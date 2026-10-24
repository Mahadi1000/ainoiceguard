#!/usr/bin/env python3
"""
Extract a fixed 50-file real-speech subset from the VoiceBank+DEMAND test set.

Dataset: Valentini-Botinhao, C. (2017). Noisy speech database for training
speech enhancement algorithms and TTS models. University of Edinburgh.
https://doi.org/10.7488/ds/2117   License: CC BY 4.0 (attribution required).

This script does NOT download anything by default. Download these two files
from the dataset page yourself and pass their paths:
    clean_testset_wav.zip
    noisy_testset_wav.zip

Output (not committed to git):
    bench/fixtures/voicebank/clean/<id>.wav
    bench/fixtures/voicebank/noisy/<id>.wav
    bench/fixtures/voicebank/manifest.json   (file list + SHA-256, committed)

The subset is chosen deterministically (sorted ids, fixed seed), so everyone
who runs this against the same zips gets the same 50 files. Use --check to
verify extracted files against an existing manifest.

Files that are not 48 kHz mono are rejected: ng_bench only accepts 48 kHz.

Usage:
  python bench/fetch_voicebank.py --clean-zip PATH --noisy-zip PATH [--check]
"""

import argparse
import hashlib
import io
import json
import os
import sys
import zipfile

import numpy as np
from scipy.io import wavfile

SUBSET_SIZE = 50
SEED = 2117


def wav_ids(zf):
    return sorted(os.path.splitext(os.path.basename(n))[0]
                  for n in zf.namelist() if n.lower().endswith(".wav"))


def member(zf, file_id):
    for n in zf.namelist():
        if os.path.basename(n) == file_id + ".wav":
            return n
    raise KeyError(file_id)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clean-zip", required=True)
    ap.add_argument("--noisy-zip", required=True)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "fixtures", "voicebank"))
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    manifest_path = os.path.join(args.out, "manifest.json")
    previous = None
    if args.check:
        if not os.path.exists(manifest_path):
            sys.exit(f"--check: {manifest_path} not found")
        with open(manifest_path) as f:
            previous = json.load(f)

    with zipfile.ZipFile(args.clean_zip) as cz, zipfile.ZipFile(args.noisy_zip) as nz:
        common = sorted(set(wav_ids(cz)) & set(wav_ids(nz)))
        if len(common) < SUBSET_SIZE:
            sys.exit(f"only {len(common)} paired files found")
        rng = np.random.default_rng(SEED)
        chosen = sorted(rng.choice(common, size=SUBSET_SIZE, replace=False).tolist())

        files = {}
        for kind in ("clean", "noisy"):
            os.makedirs(os.path.join(args.out, kind), exist_ok=True)
        for fid in chosen:
            entry = {}
            for kind, zf in (("clean", cz), ("noisy", nz)):
                data = zf.read(member(zf, fid))
                sr, x = wavfile.read(io.BytesIO(data))
                if sr != 48000 or x.ndim != 1:
                    sys.exit(f"{kind}/{fid}.wav is {sr} Hz, {x.ndim}-D; need 48 kHz mono")
                with open(os.path.join(args.out, kind, fid + ".wav"), "wb") as f:
                    f.write(data)
                entry[kind + "_sha256"] = sha256(data)
            entry["seconds"] = round(len(x) / 48000.0, 3)
            files[fid] = entry

    if previous is not None:
        bad = [k for k, v in files.items() if previous["files"].get(k) != v]
        if bad or set(previous["files"]) != set(files):
            sys.exit("MISMATCH: " + ", ".join(bad or ["file list differs"]))
        print(f"OK: {len(files)} pairs match manifest")
        return

    manifest = {
        "dataset": "VoiceBank+DEMAND test set (Valentini-Botinhao 2017)",
        "doi": "10.7488/ds/2117",
        "license": "CC BY 4.0",
        "sample_rate": 48000,
        "seed": SEED,
        "files": files,
    }
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)
    print(f"extracted {len(files)} clean/noisy pairs to {args.out}")


if __name__ == "__main__":
    main()
