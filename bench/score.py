#!/usr/bin/env python3
"""
Score ng_bench outputs against their inputs and clean references.

Reads a run directory produced by run_bench.py (runs.json + output WAVs and
per-file ng_bench JSON) and writes:
    <run>/scores.json    per-file metrics
    <run>/summary.md     per-backend aggregate table

Metrics (all computed after estimating and removing the output delay):
  delay_ms           lag that maximises cross-correlation(input, output)
  si_sdr_in/out_db   scale-invariant SDR of noisy input / output vs clean
  stoi_in/out        STOI (pystoi; skipped if not installed)
  dnsmos_*           DNSMOS P.835 SIG/BAK/OVRL (only with --dnsmos-model)
  speech_level_db    output vs clean RMS on speech-active samples
  attenuation_db     output vs input RMS on noise-only fixtures (negative = quieter)
  out_rms_dbfs       output RMS on silence fixtures
  clipped_samples    |x| >= 0.999 in output
  non_finite         NaN/Inf samples in output

Synthetic-voice fixtures are a regression proxy only; use real speech
(fetch_voicebank.py) for quality conclusions.

SI-SDR caveat: it penalises any non-linear-phase filter. rnnoise-chain's
80 Hz high-pass alone costs ~10-15 dB of SI-SDR at high SNR on the synthetic
voice, although its effect is inaudible phase shift. --ref-hpf80 scores against
a clean reference passed through the same high-pass, which makes rnnoise-chain
comparable; STOI and DNSMOS are the primary quality metrics.

Usage:
  python bench/score.py RUN_DIR [--dnsmos-model sig_bak_ovr.onnx] [--ref-hpf80]
"""

import argparse
import json
import math
import os
import sys

import numpy as np
from scipy.io import wavfile
from scipy.signal import fftconvolve, lfilter, resample_poly

try:
    from pystoi import stoi as _stoi
except ImportError:  # optional
    _stoi = None

SR = 48000
MAX_LAG_S = 0.2

# Same 2nd-order 80 Hz Butterworth high-pass as RNNoiseWrapper::initFilters().
HPF80_B = [0.992631, -1.985261, 0.992631]
HPF80_A = [1.0, -1.985199, 0.985323]


def read(path):
    sr, x = wavfile.read(path)
    if sr != SR:
        raise ValueError(f"{path}: {sr} Hz, expected {SR}")
    if x.dtype == np.int16:
        x = x.astype(np.float64) / 32768.0
    elif x.dtype == np.int32:
        x = x.astype(np.float64) / 2147483648.0
    else:
        x = x.astype(np.float64)
    if x.ndim != 1:
        raise ValueError(f"{path}: not mono")
    return x


def db(x, floor=1e-12):
    return 20.0 * math.log10(max(x, floor))


def rms(x):
    return float(np.sqrt(np.mean(np.square(x)))) if len(x) else 0.0


def estimate_delay(ref, out):
    """Non-negative lag (samples) by which `out` trails `ref`."""
    max_lag = int(MAX_LAG_S * SR)
    n = min(len(ref), len(out))
    if n == 0 or rms(ref[:n]) < 1e-9 or rms(out[:n]) < 1e-9:
        return None
    corr = fftconvolve(out[:n], ref[:n][::-1], mode="full")
    zero = n - 1
    window = corr[zero:zero + max_lag + 1]
    return int(np.argmax(np.abs(window)))


def align(ref, out, lag):
    """Shift `out` earlier by lag and trim both to equal length."""
    if lag:
        out = out[lag:]
    n = min(len(ref), len(out))
    return ref[:n], out[:n]


def si_sdr(ref, est):
    ref = ref - np.mean(ref)
    est = est - np.mean(est)
    denom = np.dot(ref, ref)
    if denom <= 0:
        return None
    target = (np.dot(est, ref) / denom) * ref
    noise = est - target
    return 10.0 * math.log10(max(np.dot(target, target), 1e-20) / max(np.dot(noise, noise), 1e-20))


def stoi(ref, est):
    if _stoi is None:
        return None
    return float(_stoi(ref, est, SR, extended=False))


class Dnsmos:
    """DNSMOS P.835 (sig_bak_ovr.onnx from microsoft/DNS-Challenge, DNSMOS/).

    Mirrors the non-personalised path of DNS-Challenge dnsmos_local.py:
    16 kHz input, 9.01 s windows with 1 s hop, polynomial calibration.
    Verify results against the reference script before relying on them.
    """

    INPUT_LEN = int(9.01 * 16000)
    P_SIG = np.poly1d([-0.08397278, 1.22083953, 0.0052439])
    P_BAK = np.poly1d([-0.13166888, 1.60915514, -0.39604546])
    P_OVR = np.poly1d([-0.06766283, 1.11546468, 0.04602535])

    def __init__(self, model_path):
        import onnxruntime as ort
        self.sess = ort.InferenceSession(model_path, providers=["CPUExecutionProvider"])
        self.input_name = self.sess.get_inputs()[0].name

    def __call__(self, x48):
        x = resample_poly(x48, 1, 3).astype(np.float32)
        while len(x) < self.INPUT_LEN:
            x = np.concatenate([x, x])
        hops = int(np.floor(len(x) / 16000) - 9.01) + 1
        sig, bak, ovr = [], [], []
        for i in range(max(hops, 1)):
            seg = x[i * 16000:i * 16000 + self.INPUT_LEN]
            if len(seg) < self.INPUT_LEN:
                break
            s, b, o = self.sess.run(None, {self.input_name: seg[np.newaxis, :]})[0][0]
            sig.append(self.P_SIG(s))
            bak.append(self.P_BAK(b))
            ovr.append(self.P_OVR(o))
        return {"dnsmos_sig": float(np.mean(sig)), "dnsmos_bak": float(np.mean(bak)),
                "dnsmos_ovrl": float(np.mean(ovr))}


def score_one(entry, run_dir, dnsmos, ref_hpf80=False):
    inp = read(entry["input"])
    out = read(os.path.join(run_dir, entry["output"]))
    clean = read(entry["clean"]) if entry.get("clean") else None
    if clean is not None and ref_hpf80:
        clean = lfilter(HPF80_B, HPF80_A, clean)
    kind = entry["kind"]

    m = {
        "non_finite": int(np.count_nonzero(~np.isfinite(out))),
        "clipped_samples": int(np.count_nonzero(np.abs(out) >= 0.999)),
        "out_peak_dbfs": db(float(np.max(np.abs(out))) if len(out) else 0.0),
    }
    out = np.nan_to_num(out)

    lag = estimate_delay(inp, out)
    m["delay_ms"] = None if lag is None else lag * 1000.0 / SR

    if kind in ("mixture", "noisy") and clean is not None:
        # Align against the reference actually being scored, not the input:
        # with --ref-hpf80 the filter's phase is already in the reference, so
        # the input-based lag (which includes that phase) would misalign.
        ref_lag = estimate_delay(clean, out)
        m["align_ms"] = None if ref_lag is None else ref_lag * 1000.0 / SR
        c_in, x_in = align(clean, inp, estimate_delay(clean, inp) or 0)
        c_out, x_out = align(clean, out, ref_lag or 0)
        m["si_sdr_in_db"] = si_sdr(c_in, x_in)
        m["si_sdr_out_db"] = si_sdr(c_out, x_out)
        if m["si_sdr_in_db"] is not None and m["si_sdr_out_db"] is not None:
            m["si_sdr_delta_db"] = m["si_sdr_out_db"] - m["si_sdr_in_db"]
        m["stoi_in"] = stoi(c_in, x_in)
        m["stoi_out"] = stoi(c_out, x_out)
        active = np.abs(c_out) > 10 ** (-40 / 20)
        if active.any():
            m["speech_level_db"] = db(rms(x_out[active])) - db(rms(c_out[active]))
        if dnsmos is not None:
            m.update({k + "_out": v for k, v in dnsmos(out).items()})
            m.update({k + "_in": v for k, v in dnsmos(inp).items()})
    elif kind == "noise_only":
        m["attenuation_db"] = db(rms(out)) - db(rms(inp))
    elif kind == "silence":
        m["out_rms_dbfs"] = db(rms(out))
    return m


def mean(vals):
    vals = [v for v in vals if v is not None]
    return float(np.mean(vals)) if vals else None


def maxn(vals):
    vals = [v for v in vals if v is not None]
    return max(vals) if vals else None


def fmt(v, nd=2):
    return "—" if v is None else f"{v:.{nd}f}"


def summarize(entries):
    by_backend = {}
    for e in entries:
        by_backend.setdefault(e["backend"], []).append(e)

    rows = []
    for be, es in sorted(by_backend.items()):
        mix = [e for e in es if e["kind"] in ("mixture", "noisy")]
        low = [e for e in mix if e.get("snr_db") is None or e["snr_db"] <= 10]
        s = lambda key, pool: mean([e["metrics"].get(key) for e in pool])
        rows.append({
            "backend": be,
            "files": len(es),
            "delay_ms": s("delay_ms", mix),
            "si_sdr_delta_db_snr_le10": s("si_sdr_delta_db", low),
            "stoi_out_snr_le10": s("stoi_out", low),
            "stoi_delta_snr_le10": mean([
                (e["metrics"].get("stoi_out") - e["metrics"]["stoi_in"])
                if e["metrics"].get("stoi_out") is not None and e["metrics"].get("stoi_in") is not None
                else None for e in low]),
            "dnsmos_ovrl_out": s("dnsmos_ovrl_out", low),
            "speech_level_db": s("speech_level_db", mix),
            "noise_attenuation_db": s("attenuation_db", [e for e in es if e["kind"] == "noise_only"]),
            "silence_out_rms_dbfs": s("out_rms_dbfs", [e for e in es if e["kind"] == "silence"]),
            "clipped_samples": int(sum(e["metrics"]["clipped_samples"] for e in es)),
            "non_finite": int(sum(e["metrics"]["non_finite"] for e in es)),
            # Per-frame timing is absent for wall-clock-only backends (dfn-cli*).
            "frame_us_mean": mean([e["timing_us"]["mean"] for e in es]),
            "frame_us_p99_max": maxn([e["timing_us"]["p99"] for e in es]),
            "frame_us_max": maxn([e["timing_us"]["max"] for e in es]),
            "rtf_mean": mean([e["real_time_factor"] for e in es]),
            "overruns": maxn([sum(e["timing_us"]["overruns_over_10ms"] for e in es)]
                             if all(e["timing_us"]["overruns_over_10ms"] is not None for e in es)
                             else []),
        })
    return rows


def write_summary(rows, path, meta):
    lines = [
        "# ng_bench summary", "",
        f"- fixtures: {', '.join(meta.get('fixture_sets', []))}",
        f"- repeat: {meta.get('repeat')}, ng_bench: `{meta.get('bench')}`",
        f"- STOI: {'pystoi' if _stoi else 'not installed (skipped)'}; "
        f"DNSMOS: {'enabled' if meta.get('dnsmos') else 'not enabled'}; "
        f"reference: {'80 Hz high-passed clean' if meta.get('ref_hpf80') else 'clean'}",
        "",
        "| backend | files | delay ms | ΔSI-SDR dB (SNR≤10) | STOI out | ΔSTOI | DNSMOS OVRL | speech level dB "
        "| noise atten dB | silence dBFS | clipped | NaN | frame µs mean | p99 (worst file) | max | RTF | overruns |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for r in rows:
        lines.append(
            f"| {r['backend']} | {r['files']} | {fmt(r['delay_ms'], 1)} | {fmt(r['si_sdr_delta_db_snr_le10'])} "
            f"| {fmt(r['stoi_out_snr_le10'], 3)} | {fmt(r['stoi_delta_snr_le10'], 3)} | {fmt(r['dnsmos_ovrl_out'])} "
            f"| {fmt(r['speech_level_db'])} | {fmt(r['noise_attenuation_db'], 1)} | {fmt(r['silence_out_rms_dbfs'], 1)} "
            f"| {r['clipped_samples']} | {r['non_finite']} | {fmt(r['frame_us_mean'], 0)} | {fmt(r['frame_us_p99_max'], 0)} "
            f"| {fmt(r['frame_us_max'], 0)} | {fmt(r['rtf_mean'], 4)} | {fmt(r['overruns'], 0)} |")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_dir")
    ap.add_argument("--dnsmos-model", help="path to DNSMOS sig_bak_ovr.onnx (optional)")
    ap.add_argument("--ref-hpf80", action="store_true",
                    help="score against an 80 Hz high-passed clean reference (see SI-SDR caveat)")
    args = ap.parse_args()

    with open(os.path.join(args.run_dir, "runs.json")) as f:
        runs = json.load(f)
    dnsmos = Dnsmos(args.dnsmos_model) if args.dnsmos_model else None

    entries = []
    for e in runs["entries"]:
        with open(os.path.join(args.run_dir, e["stats"])) as f:
            st = json.load(f)
        e = dict(e, timing_us=st["timing_us"], real_time_factor=st["real_time_factor"])
        e["metrics"] = score_one(e, args.run_dir, dnsmos, args.ref_hpf80)
        entries.append(e)

    with open(os.path.join(args.run_dir, "scores.json"), "w") as f:
        json.dump({"meta": runs["meta"], "entries": entries}, f, indent=2)
    rows = summarize(entries)
    meta = dict(runs["meta"], dnsmos=bool(dnsmos), ref_hpf80=args.ref_hpf80)
    write_summary(rows, os.path.join(args.run_dir, "summary.md"), meta)
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")  # Windows consoles default to cp1252
    print(open(os.path.join(args.run_dir, "summary.md"), encoding="utf-8").read())
    if any(r["non_finite"] for r in rows):
        sys.exit(1)


if __name__ == "__main__":
    main()
