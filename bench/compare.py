#!/usr/bin/env python3
"""
Paired comparison of backends in a scored run (after score.py).

For every backend vs a baseline backend, over files present in both:
mean per-file difference, 95% bootstrap CI (5000 resamples, fixed seed) and
the share of files where the backend scores higher. Also prints absolute
means, including the unprocessed input where the metric has an *_in value.

Usage:
  python bench/compare.py RUN_DIR [--baseline rnnoise-chain]
      [--metrics dnsmos_ovrl_out dnsmos_sig_out dnsmos_bak_out stoi_out]
"""

import argparse
import json
import os

import numpy as np

DEFAULT_METRICS = ["dnsmos_ovrl_out", "dnsmos_sig_out", "dnsmos_bak_out", "stoi_out"]


def bootstrap_ci(x, rng, n=5000):
    x = np.asarray(x, dtype=float)
    means = rng.choice(x, size=(n, len(x)), replace=True).mean(axis=1)
    return float(x.mean()), float(np.percentile(means, 2.5)), float(np.percentile(means, 97.5))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_dir")
    ap.add_argument("--baseline", default="rnnoise-chain")
    ap.add_argument("--metrics", nargs="+", default=DEFAULT_METRICS)
    args = ap.parse_args()

    with open(os.path.join(args.run_dir, "scores.json")) as f:
        scores = json.load(f)
    by = {}
    for e in scores["entries"]:
        by.setdefault(e["backend"], {})[e["name"]] = e["metrics"]
    if args.baseline not in by:
        raise SystemExit(f"baseline {args.baseline!r} not in run")

    def has(m, k):
        return m.get(k) is not None

    metrics = [k for k in args.metrics
               if any(has(m, k) for m in by[args.baseline].values())]

    print("Absolute means")
    base_files = by[args.baseline]
    row = ["input".ljust(16)]
    for k in metrics:
        vals = [m.get(k.replace("_out", "_in")) for m in base_files.values()]
        vals = [v for v in vals if v is not None]
        row.append(f"{k}={np.mean(vals):.3f}" if vals else f"{k}=—")
    print("  " + "  ".join(row))
    for be in sorted(by):
        row = [be.ljust(16)]
        for k in metrics:
            vals = [m[k] for m in by[be].values() if has(m, k)]
            row.append(f"{k}={np.mean(vals):.3f}" if vals else f"{k}=—")
        print("  " + "  ".join(row))

    print(f"\nPaired difference vs {args.baseline} (mean [95% CI], % files higher)")
    rng = np.random.default_rng(0)
    for be in sorted(by):
        if be == args.baseline:
            continue
        common = sorted(set(by[be]) & set(base_files))
        for k in metrics:
            diff = [by[be][n][k] - base_files[n][k] for n in common
                    if has(by[be][n], k) and has(base_files[n], k)]
            if not diff:
                continue
            mu, lo, hi = bootstrap_ci(diff, rng)
            print(f"  {be:<16} {k:<16} {mu:+.3f} [{lo:+.3f}, {hi:+.3f}]  "
                  f"higher in {100 * np.mean(np.array(diff) > 0):.0f}% (n={len(diff)})")


if __name__ == "__main__":
    main()
