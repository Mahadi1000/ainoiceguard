#!/usr/bin/env python3
"""
Run ng_bench over fixture sets and score the results.

Fixture sets are directories containing a manifest.json written by
gen_fixtures.py (synthetic) or fetch_voicebank.py (real speech).

Usage:
  python bench/run_bench.py --bench deps/bench-build/Release/ng_bench.exe \
      [--fixtures bench/fixtures/synth bench/fixtures/voicebank] \
      [--backends rnnoise-raw rnnoise-chain] [--repeat 3] [--out bench/results/NAME] \
      [--dnsmos-model PATH]

Writes <out>/runs.json, <out>/<backend>/<file>.wav|.json, then calls score.py
to produce <out>/scores.json and <out>/summary.md.
"""

import argparse
import datetime
import json
import os
import platform
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def fixture_entries(fix_dir):
    with open(os.path.join(fix_dir, "manifest.json")) as f:
        man = json.load(f)
    entries = []
    if "dataset" in man:  # VoiceBank layout: clean/ and noisy/ pairs
        for fid in sorted(man["files"]):
            entries.append({
                "set": os.path.basename(fix_dir.rstrip("/\\")),
                "name": fid,
                "kind": "noisy",
                "input": os.path.join(fix_dir, "noisy", fid + ".wav"),
                "clean": os.path.join(fix_dir, "clean", fid + ".wav"),
                "snr_db": None,
            })
    else:  # synthetic layout
        for name, meta in sorted(man["files"].items()):
            if meta["kind"] == "clean":
                continue
            entries.append({
                "set": os.path.basename(fix_dir.rstrip("/\\")),
                "name": os.path.splitext(name)[0],
                "kind": meta["kind"],
                "input": os.path.join(fix_dir, name),
                "clean": os.path.join(fix_dir, meta["clean"]) if meta.get("clean") else None,
                "snr_db": meta.get("snr_db"),
            })
    return entries


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bench", required=True, help="path to ng_bench executable")
    ap.add_argument("--fixtures", nargs="+", default=[os.path.join(HERE, "fixtures", "synth")])
    ap.add_argument("--backends", nargs="+", default=["rnnoise-raw", "rnnoise-chain"])
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--out")
    ap.add_argument("--dnsmos-model")
    ap.add_argument("--ref-hpf80", action="store_true", help="passed through to score.py")
    args = ap.parse_args()
    args.bench = os.path.abspath(args.bench)
    if not os.path.isfile(args.bench):
        sys.exit(f"ng_bench not found: {args.bench}")

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = args.out or os.path.join(HERE, "results", stamp)
    os.makedirs(out, exist_ok=True)

    fixtures = []
    for d in args.fixtures:
        if not os.path.exists(os.path.join(d, "manifest.json")):
            sys.exit(f"{d}: no manifest.json (run gen_fixtures.py / fetch_voicebank.py first)")
        fixtures += fixture_entries(d)

    entries = []
    for be in args.backends:
        os.makedirs(os.path.join(out, be), exist_ok=True)
        for fx in fixtures:
            base = f"{fx['set']}__{fx['name']}"
            wav_rel = os.path.join(be, base + ".wav")
            json_rel = os.path.join(be, base + ".json")
            cmd = [args.bench, "process", "--backend", be, "--in", fx["input"],
                   "--out", os.path.join(out, wav_rel), "--json", os.path.join(out, json_rel),
                   "--repeat", str(args.repeat)]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                sys.exit(f"ng_bench failed on {fx['input']} ({be}):\n{r.stderr}")
            entries.append(dict(fx, backend=be, output=wav_rel, stats=json_rel))
        print(f"{be}: {len(fixtures)} files")

    meta = {
        "created": stamp,
        "bench": os.path.abspath(args.bench),
        "repeat": args.repeat,
        "backends": args.backends,
        "fixture_sets": [os.path.basename(d.rstrip("/\\")) for d in args.fixtures],
        "host": {"platform": platform.platform(), "machine": platform.machine(),
                 "processor": platform.processor(), "cpus": os.cpu_count()},
    }
    with open(os.path.join(out, "runs.json"), "w") as f:
        json.dump({"meta": meta, "entries": entries}, f, indent=2)

    score_cmd = [sys.executable, os.path.join(HERE, "score.py"), out]
    if args.dnsmos_model:
        score_cmd += ["--dnsmos-model", args.dnsmos_model]
    if args.ref_hpf80:
        score_cmd.append("--ref-hpf80")
    sys.exit(subprocess.run(score_cmd).returncode)


if __name__ == "__main__":
    main()
