#!/usr/bin/env python3
"""Plot ParallelCrack benchmark results.

Reads one or more CSV files produced by the program's --csv option (see
benchmarks/run.sh) and draws four panels:

  1. wall-clock search time vs. process count
  2. speedup vs. process count (with the ideal linear line for reference)
  3. parallel efficiency vs. process count
  4. hash rate (candidates/second) vs. process count

Speedup and efficiency are computed from the MEASURED data only:
    speedup(p)    = T(baseline) / T(p)
    efficiency(p) = speedup(p) / (p / baseline_p)
where the baseline is the smallest process count present for that strategy.
Repeated runs at the same process count are averaged.

Nothing here invents data: if the CSV is missing a configuration, it simply is
not plotted.

Usage:
    python3 benchmarks/plot.py results/run.csv [more.csv ...] --out results/run.png
"""

import argparse
import sys

import pandas as pd
import matplotlib

matplotlib.use("Agg")  # render to a file without needing a display
import matplotlib.pyplot as plt


def load_data(paths):
    """Load and concatenate one or more benchmark CSV files."""
    frames = []
    for path in paths:
        frame = pd.read_csv(path)
        frames.append(frame)
    data = pd.concat(frames, ignore_index=True)
    return data


def summarize(data):
    """Average repeated runs, then add speedup and efficiency per strategy."""
    # Average the repeated runs at each (strategy, nprocs) point.
    grouped = (
        data.groupby(["strategy", "nprocs"], as_index=False)
        .agg(
            wall_seconds=("wall_seconds", "mean"),
            hashes_per_second=("hashes_per_second", "mean"),
            comm_seconds=("comm_seconds", "mean"),
            candidates_evaluated=("candidates_evaluated", "mean"),
        )
        .sort_values(["strategy", "nprocs"])
    )

    # Compute speedup and efficiency relative to each strategy's own baseline
    # (its smallest process count in the data).
    pieces = []
    for strategy, group in grouped.groupby("strategy"):
        group = group.sort_values("nprocs").copy()
        baseline_procs = group["nprocs"].iloc[0]
        baseline_time = group["wall_seconds"].iloc[0]
        group["speedup"] = baseline_time / group["wall_seconds"]
        group["efficiency"] = group["speedup"] / (group["nprocs"] / baseline_procs)
        pieces.append(group)

    return pd.concat(pieces, ignore_index=True)


def make_plots(summary, out_path):
    """Draw the four panels and save to out_path."""
    figure, axes = plt.subplots(2, 2, figsize=(12, 9))
    figure.suptitle("ParallelCrack — MPI communication strategy benchmark", fontsize=14)

    strategies = sorted(summary["strategy"].unique())

    # Panel 1: wall time.
    axis = axes[0][0]
    for strategy in strategies:
        rows = summary[summary["strategy"] == strategy]
        axis.plot(rows["nprocs"], rows["wall_seconds"], marker="o", label=strategy)
    axis.set_title("Search wall time")
    axis.set_xlabel("MPI processes")
    axis.set_ylabel("seconds")
    axis.grid(True, alpha=0.3)
    axis.legend()

    # Panel 2: speedup, with the ideal line.
    axis = axes[0][1]
    all_procs = sorted(summary["nprocs"].unique())
    baseline_p = all_procs[0]
    ideal = [p / baseline_p for p in all_procs]
    axis.plot(all_procs, ideal, linestyle="--", color="gray", label="ideal")
    for strategy in strategies:
        rows = summary[summary["strategy"] == strategy]
        axis.plot(rows["nprocs"], rows["speedup"], marker="o", label=strategy)
    axis.set_title("Speedup (relative to smallest process count)")
    axis.set_xlabel("MPI processes")
    axis.set_ylabel("speedup")
    axis.grid(True, alpha=0.3)
    axis.legend()

    # Panel 3: efficiency.
    axis = axes[1][0]
    axis.axhline(1.0, linestyle="--", color="gray", label="ideal")
    for strategy in strategies:
        rows = summary[summary["strategy"] == strategy]
        axis.plot(rows["nprocs"], rows["efficiency"], marker="o", label=strategy)
    axis.set_title("Parallel efficiency")
    axis.set_xlabel("MPI processes")
    axis.set_ylabel("efficiency (1.0 = perfect)")
    axis.grid(True, alpha=0.3)
    axis.legend()

    # Panel 4: hash rate.
    axis = axes[1][1]
    for strategy in strategies:
        rows = summary[summary["strategy"] == strategy]
        axis.plot(rows["nprocs"], rows["hashes_per_second"], marker="o", label=strategy)
    axis.set_title("Hash rate")
    axis.set_xlabel("MPI processes")
    axis.set_ylabel("candidates / second")
    axis.grid(True, alpha=0.3)
    axis.legend()

    figure.tight_layout(rect=(0, 0, 1, 0.96))
    figure.savefig(out_path, dpi=120)
    print("Wrote %s" % out_path)


def main():
    parser = argparse.ArgumentParser(description="Plot ParallelCrack benchmark CSVs")
    parser.add_argument("csv", nargs="+", help="input CSV file(s)")
    parser.add_argument("--out", default="results/benchmark.png", help="output image path")
    args = parser.parse_args()

    data = load_data(args.csv)
    if data.empty:
        print("No rows found in the given CSV file(s).", file=sys.stderr)
        return 1

    summary = summarize(data)

    # Print a compact table so the numbers are visible without opening the image.
    print(summary.to_string(index=False))
    print()

    make_plots(summary, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
