#!/usr/bin/env python3
"""
latency histogram analysis.

reads JSON output from latency_bench --json and prints a summary table.
optionally generates CDF plots if matplotlib is available.

usage:
    python3 scripts/analyze.py results/bench_*.json
    python3 scripts/analyze.py results/bench_*.json --plot
"""

import sys
import json
import argparse
from pathlib import Path
from typing import Any

try:
    import matplotlib.pyplot as plt
    import matplotlib.gridspec as gridspec
    HAS_PLOT = True
except ImportError:
    HAS_PLOT = False


def load(path: str) -> dict[str, Any]:
    with open(path) as f:
        return json.load(f)


def print_table(results: list[dict[str, Any]]) -> None:
    cols = ["benchmark", "n", "p50_ns", "p95_ns", "p99_ns", "p999_ns", "p9999_ns", "mean_ns", "min_ns", "max_ns"]
    widths = [28, 8, 8, 8, 8, 9, 10, 8, 8, 10]

    header = "  ".join(f"{c:<{w}}" for c, w in zip(cols, widths))
    print(header)
    print("-" * len(header))

    for run in results:
        meta = run.get("meta", {})
        print(f"\n  [{meta.get('timestamp', '?')}  {meta.get('cpu', '?')[:40]}  "
              f"tsc={meta.get('tsc_ghz', 0):.3f}GHz  core={meta.get('pin_core', '?')}]")

        for b in run.get("benchmarks", []):
            # format manually so we get consistent decimal places
            row = (
                f"  {b['name']:<{widths[0]}}"
                f"  {b['samples']:>{widths[1]}}"
                f"  {b['p50_ns']:>{widths[2]}.1f}"
                f"  {b['p95_ns']:>{widths[3]}.1f}"
                f"  {b['p99_ns']:>{widths[4]}.1f}"
                f"  {b['p999_ns']:>{widths[5]}.1f}"
                f"  {b['p9999_ns']:>{widths[6]}.1f}"
                f"  {b['mean_ns']:>{widths[7]}.1f}"
                f"  {b['min_ns']:>{widths[8]}.1f}"
                f"  {b['max_ns']:>{widths[9]}.1f}"
            )
            print(row)


def plot_cdf(results: list[dict[str, Any]], out_path: str) -> None:
    if not HAS_PLOT:
        print("[warn] matplotlib not available, skipping plot", file=sys.stderr)
        return

    fig = plt.figure(figsize=(14, 7))
    gs  = gridspec.GridSpec(1, 2, figure=fig)
    ax_cdf  = fig.add_subplot(gs[0, 0])
    ax_tail = fig.add_subplot(gs[0, 1])

    colors = ["#2196F3", "#4CAF50", "#FF9800", "#E91E63", "#9C27B0", "#00BCD4"]

    for ri, run in enumerate(results):
        cpu = run.get("meta", {}).get("cpu", "?")[:20]
        for bi, b in enumerate(run.get("benchmarks", [])):
            color = colors[(ri * 4 + bi) % len(colors)]
            label = f"{b['name']} ({cpu})"

            # approximate CDF from the percentile points we have.
            # not statistically rigorous but fine for visual inspection.
            pcts = [0.50, 0.95, 0.99, 0.999, 0.9999]
            vals = [b["p50_ns"], b["p95_ns"], b["p99_ns"], b["p999_ns"], b["p9999_ns"]]

            ax_cdf.plot(vals, [p * 100 for p in pcts],
                        marker="o", markersize=4, color=color, label=label)
            ax_tail.plot(vals[2:], [(1 - p) * 100 for p in pcts[2:]],
                         marker="s", markersize=5, color=color, label=label)

    ax_cdf.set(xlabel="Latency (ns)", ylabel="Percentile (%)",
               title="Latency CDF", ylim=(0, 100))
    ax_cdf.legend(fontsize=7)
    ax_cdf.grid(alpha=0.3)

    ax_tail.set(xlabel="Latency (ns)", ylabel="Tail probability (%)",
                title="Tail (p99+)", yscale="log")
    ax_tail.legend(fontsize=7)
    ax_tail.grid(alpha=0.3, which="both")

    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    print(f"plot -> {out_path}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--plot", action="store_true")
    ap.add_argument("--out", default="results/latency_analysis.png")
    args = ap.parse_args()

    results = [load(f) for f in args.files]
    print_table(results)

    if args.plot:
        plot_cdf(results, args.out)


if __name__ == "__main__":
    main()
