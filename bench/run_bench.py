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
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

# Official DeepFilterNet CLI (Phase 1A). Backend name -> extra CLI arguments.
# No delay compensation (-D) on purpose: score.py measures and removes delay.
DFN_CLI_VARIANTS = {
    "dfn-cli": [],                      # embedded DFN3, no attenuation limit (100 dB)
    "dfn-cli-pf": ["--pf"],             # + post-filter (beta 0.02)
    "dfn-cli-a18": ["-a", "18"],
    "dfn-cli-a12": ["-a", "12"],
    "dfn-cli-a6": ["-a", "6"],
    "dfn-cli-a12-pf": ["-a", "12", "--pf"],
    # CLI forced to the thresholds that the v0.5.6 C API (RuntimeParams::default_with_ch)
    # uses; the CLI's own defaults are -15/35/35.
    "dfn-cli-capi-thresh": ["--min-db-thresh=-10", "--max-db-erb-thresh=30", "--max-db-df-thresh=20"],
}
SR = 48000


def _write_silence(path, seconds):
    import struct
    n = int(seconds * SR)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + 2 * n) + b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 1, SR, 2 * SR, 2, 16))
        f.write(b"data" + struct.pack("<I", 2 * n) + b"\x00\x00" * n)


_RTF_RE = re.compile(r"Enhanced audio file .* in ([0-9.]+) \(RTF: ([0-9.eE+-]+)\)")


def _run_cli_once(cli, extra, src, tmp_dir):
    """Run deep-filter on one file.

    Returns (wall_seconds, reported_seconds, reported_rtf, output_path). The
    reported values come from the CLI's own `-v` log line and exclude process
    start and model load; they are None if the line is missing.
    """
    for f in os.listdir(tmp_dir):
        os.remove(os.path.join(tmp_dir, f))
    t0 = time.perf_counter()
    r = subprocess.run([cli, "-v", *extra, "-o", tmp_dir, src], capture_output=True, text=True)
    dt = time.perf_counter() - t0
    if r.returncode != 0:
        sys.exit(f"deep-filter failed on {src}:\n{r.stdout}\n{r.stderr}")
    wavs = [f for f in os.listdir(tmp_dir) if f.lower().endswith(".wav")]
    if len(wavs) != 1:
        sys.exit(f"deep-filter produced {wavs!r} for {src}")
    m = _RTF_RE.search(r.stderr + r.stdout)
    rep_s, rep_rtf = (float(m.group(1)), float(m.group(2))) if m else (None, None)
    return dt, rep_s, rep_rtf, os.path.join(tmp_dir, wavs[0])


def dfn_cli_startup(cli, out, repeat):
    """Median wall time on a 0.1 s silent file = process start + model load."""
    tmp = os.path.join(out, "_dfn_tmp")
    os.makedirs(tmp, exist_ok=True)
    probe = os.path.join(out, "_dfn_probe.wav")
    _write_silence(probe, 0.1)
    times = sorted(_run_cli_once(cli, [], probe, tmp)[0] for _ in range(max(repeat, 3)))
    return times[len(times) // 2]


def _median(v):
    v = sorted(x for x in v if x is not None)
    return v[len(v) // 2] if v else None


def run_dfn_cli(cli, extra, src, out_wav, out_json, repeat, startup_s):
    """Process one file with the official CLI. No per-frame timing (that needs
    the libDF C API, Phase 1B). RTF is the CLI's self-reported value (median
    over passes); wall clock minus the startup probe is kept as a cross-check.
    Note: DFN skips near-silent frames, so RTF on silent fixtures is not
    representative -- use speech fixtures for speed comparisons."""
    tmp = os.path.join(os.path.dirname(out_wav), "..", "_dfn_tmp")
    os.makedirs(tmp, exist_ok=True)
    walls, reps, rtfs = [], [], []
    for _ in range(max(repeat, 1)):
        dt, rep_s, rep_rtf, produced = _run_cli_once(cli, extra, src, tmp)
        walls.append(dt)
        reps.append(rep_s)
        rtfs.append(rep_rtf)
    shutil.move(produced, out_wav)

    from scipy.io import wavfile
    sr, x = wavfile.read(src, mmap=True)
    seconds = len(x) / float(sr)
    wall = _median(walls)
    rtf = _median(rtfs)
    stats = {
        "tool": "deep-filter (official CLI)",
        "args": extra,
        "audio_seconds": seconds,
        "wall_seconds_median": wall,
        "startup_seconds": startup_s,
        "reported_seconds_median": _median(reps),
        "timing_us": {"first_frame": None, "mean": None, "p50": None, "p95": None,
                      "p99": None, "max": None, "overruns_over_10ms": None},
        "real_time_factor": rtf if rtf is not None else
        (max(wall - startup_s, 0.0) / seconds if seconds else None),
        "rtf_wall_minus_startup": max(wall - startup_s, 0.0) / seconds if seconds else None,
        "timing_method": "CLI self-reported RTF (-v), median of passes",
    }
    with open(out_json, "w") as f:
        json.dump(stats, f, indent=2)


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
    ap.add_argument("--dfn-cli", help="official deep-filter executable (for dfn-cli* backends)")
    ap.add_argument("--dfn-model", help="DeepFilterNet tar.gz model for the ng_bench 'dfn' backend "
                    "(needs ng_bench built with -DNG_WITH_DFN=ON)")
    ap.add_argument("--priority", choices=["normal", "high"], default="normal",
                    help="process priority for ng_bench (Windows: HIGH_PRIORITY_CLASS). 'high' "
                         "reduces OS pre-emption in timing tails, closer to an MMCSS audio thread")
    args = ap.parse_args()
    if "dfn" in args.backends:
        if not args.dfn_model:
            sys.exit("backend 'dfn' needs --dfn-model PATH")
        args.dfn_model = os.path.abspath(args.dfn_model)
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

    cli_backends = [be for be in args.backends if be in DFN_CLI_VARIANTS]
    if cli_backends:
        if not args.dfn_cli:
            sys.exit(f"{', '.join(cli_backends)} need --dfn-cli PATH (official deep-filter binary)")
        args.dfn_cli = os.path.abspath(args.dfn_cli)
        startup_s = dfn_cli_startup(args.dfn_cli, out, args.repeat)
        print(f"deep-filter startup overhead: {startup_s * 1000:.0f} ms (subtracted from RTF)")

    entries = []
    for be in args.backends:
        os.makedirs(os.path.join(out, be), exist_ok=True)
        for fx in fixtures:
            base = f"{fx['set']}__{fx['name']}"
            wav_rel = os.path.join(be, base + ".wav")
            json_rel = os.path.join(be, base + ".json")
            if be in DFN_CLI_VARIANTS:
                run_dfn_cli(args.dfn_cli, DFN_CLI_VARIANTS[be], fx["input"],
                            os.path.join(out, wav_rel), os.path.join(out, json_rel),
                            args.repeat, startup_s)
            else:
                cmd = [args.bench, "process", "--backend", be, "--in", fx["input"],
                       "--out", os.path.join(out, wav_rel), "--json", os.path.join(out, json_rel),
                       "--repeat", str(args.repeat)]
                if be == "dfn":
                    cmd += ["--dfn-model", args.dfn_model]
                flags = 0
                if args.priority == "high" and os.name == "nt":
                    flags = subprocess.HIGH_PRIORITY_CLASS
                r = subprocess.run(cmd, capture_output=True, text=True, creationflags=flags)
                if r.returncode != 0:
                    sys.exit(f"ng_bench failed on {fx['input']} ({be}):\n{r.stderr}")
            entries.append(dict(fx, backend=be, output=wav_rel, stats=json_rel))
        print(f"{be}: {len(fixtures)} files")

    meta = {
        "created": stamp,
        "bench": os.path.abspath(args.bench),
        "repeat": args.repeat,
        "backends": args.backends,
        "dfn_cli": args.dfn_cli,
        "dfn_model": args.dfn_model,
        "priority": args.priority,
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
