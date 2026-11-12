#!/usr/bin/env python3
"""Validate and summarize a ParallelCrack benchmark run, and draw SVG charts.

Standard library only (no pandas/matplotlib), so it runs anywhere python3 does.

Takes the full-sweep and early-termination CSVs from one benchmark job, checks
them for internal consistency, and reports per (strategy, nprocs):

  * median wall time over the repetitions
  * min / max wall time and the relative spread (max - min) / median
  * coefficient of variation (sample stdev / mean)
  * speedup    S(p) = median T(1) / median T(p)   (same strategy, same experiment)
  * efficiency E(p) = S(p) / p
  * median hash rate, median comm_seconds, candidates evaluated (min..max)

Speedup is always relative to the same strategy's own 1-process median, so the
two strategies are never mixed in a ratio. Nothing is interpolated or invented:
only process counts present in the CSV appear in the tables and charts.

Usage:
    python3 benchmarks/analyze_results.py \
        --fullsweep results/lotus/job27836/quick_fullsweep_27836.csv \
        --early     results/lotus/job27836/quick_early_27836.csv \
        --keyspace 11881376 --early-index 5940688 \
        --outdir    results/lotus/job27836
"""

import argparse
import csv
import math
import os
import statistics
import sys

STRATEGIES = ["collective", "async"]
COLORS = {"collective": "#2a78d6", "async": "#eb6834"}
EXPERIMENT_LABEL = {"fullsweep": "Full sweep", "early": "Early termination"}


def load(path):
    with open(path, newline="") as handle:
        rows = list(csv.DictReader(handle))
    for row in rows:
        for key in ("nprocs", "found", "finder_rank", "candidates_evaluated",
                    "rounds", "length", "alphabet_size", "batch_size"):
            row[key] = int(row[key])
        for key in ("wall_seconds", "hashes_per_second", "comm_seconds"):
            row[key] = float(row[key])
    return rows


def validate(name, rows, keyspace, early_index):
    """Return a list of problems found; an empty list means the data checks out."""
    problems = []
    configs = {(r["length"], r["alphabet_size"], r["batch_size"]) for r in rows}
    if len(configs) != 1:
        problems.append("%s: workload parameters differ between rows: %s" % (name, configs))
    for i, r in enumerate(rows, start=2):
        where = "%s line %d (%s np=%d)" % (name, i, r["strategy"], r["nprocs"])
        if r["strategy"] not in STRATEGIES:
            problems.append("%s: unknown strategy" % where)
        if r["wall_seconds"] <= 0:
            problems.append("%s: non-positive wall time" % where)
        # hashes_per_second is written as an integer, so allow rounding slack.
        expected_rate = r["candidates_evaluated"] / r["wall_seconds"]
        if abs(expected_rate - r["hashes_per_second"]) > 0.001 * expected_rate + 1:
            problems.append("%s: hash rate %.0f != candidates/wall %.0f"
                            % (where, r["hashes_per_second"], expected_rate))
        if name == "fullsweep":
            if r["found"] != 0 or r["finder_rank"] != -1:
                problems.append("%s: full sweep reported a match" % where)
            if r["candidates_evaluated"] != keyspace:
                problems.append("%s: evaluated %d, expected exactly %d"
                                % (where, r["candidates_evaluated"], keyspace))
        else:
            if r["found"] != 1:
                problems.append("%s: target not found" % where)
            if r["finder_rank"] != early_index % r["nprocs"]:
                problems.append("%s: finder rank %d, expected owner %d"
                                % (where, r["finder_rank"], early_index % r["nprocs"]))
            if r["candidates_evaluated"] > keyspace:
                problems.append("%s: evaluated more than the keyspace" % where)
    return problems


def summarize(rows):
    groups = {}
    for r in rows:
        groups.setdefault((r["strategy"], r["nprocs"]), []).append(r)
    summary = []
    for strategy in STRATEGIES:
        procs = sorted(p for (s, p) in groups if s == strategy)
        if not procs:
            continue
        if procs[0] != 1:
            sys.exit("no 1-process baseline for %s" % strategy)
        base = statistics.median(r["wall_seconds"] for r in groups[(strategy, 1)])
        for p in procs:
            g = groups[(strategy, p)]
            walls = [r["wall_seconds"] for r in g]
            med = statistics.median(walls)
            mean = statistics.mean(walls)
            cv = statistics.stdev(walls) / mean if len(walls) > 1 else 0.0
            cands = [r["candidates_evaluated"] for r in g]
            summary.append({
                "strategy": strategy,
                "nprocs": p,
                "runs": len(g),
                "median_wall_s": med,
                "min_wall_s": min(walls),
                "max_wall_s": max(walls),
                "spread_pct": 100.0 * (max(walls) - min(walls)) / med,
                "cv_pct": 100.0 * cv,
                "speedup": base / med,
                "efficiency_pct": 100.0 * base / med / p,
                "median_hash_rate": statistics.median(r["hashes_per_second"] for r in g),
                "median_comm_s": statistics.median(r["comm_seconds"] for r in g),
                "min_candidates": min(cands),
                "max_candidates": max(cands),
            })
    return summary


def write_summary_csv(summary, path):
    fields = list(summary[0].keys())
    with open(path, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in summary:
            out = dict(row)
            for key, value in out.items():
                if isinstance(value, float):
                    out[key] = "%.6f" % value
            writer.writerow(out)


def markdown_table(summary):
    lines = [
        "| Strategy | p | Median wall (s) | Min–max (s) | CV | Speedup | Efficiency | Median comm (s) |",
        "|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for r in summary:
        lines.append("| %s | %d | %.3f | %.3f–%.3f | %.1f%% | %.2f× | %.1f%% | %.4f |" % (
            r["strategy"], r["nprocs"], r["median_wall_s"], r["min_wall_s"],
            r["max_wall_s"], r["cv_pct"], r["speedup"], r["efficiency_pct"],
            r["median_comm_s"]))
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# SVG drawing (hand-rolled so there are no plotting dependencies).
# ---------------------------------------------------------------------------

def nice_ticks(lo, hi, count=5):
    span = hi - lo
    raw = span / count
    mag = 10 ** math.floor(math.log10(raw))
    step = min((s * mag for s in (1, 2, 2.5, 5, 10) if s * mag >= raw))
    start = math.floor(lo / step) * step
    ticks = []
    t = start
    while t <= hi + 1e-9:
        if t >= lo - 1e-9:
            ticks.append(round(t, 10))
        t += step
    return ticks


def fmt_tick(v):
    return ("%g" % v)


def panel(x0, y0, w, h, title, ylabel, series, procs, ymin, ymax, reference=None,
          ref_label=None, log_y=False):
    """One chart panel. series: {strategy: [(p, y), ...]}. x axis is log2(p).

    With log_y the y axis is log2 as well (used for speedup, so that ideal
    linear speedup is a straight diagonal); ticks are then the process counts."""
    left, right, top, bottom = 56, 14, 34, 42
    pw, ph = w - left - right, h - top - bottom
    lx = [math.log2(p) for p in procs]
    xmin, xmax = min(lx), max(lx)

    def X(p):
        return x0 + left + (math.log2(p) - xmin) / (xmax - xmin) * pw

    def Y(v):
        if log_y:
            v, lo, hi = math.log2(v), math.log2(ymin), math.log2(ymax)
        else:
            lo, hi = ymin, ymax
        return y0 + top + (1 - (v - lo) / (hi - lo)) * ph

    out = []
    out.append('<text x="%.1f" y="%.1f" class="title">%s</text>' % (x0 + left, y0 + 20, title))
    for t in (procs if log_y else nice_ticks(ymin, ymax)):
        out.append('<line x1="%.1f" x2="%.1f" y1="%.1f" y2="%.1f" class="grid"/>'
                   % (x0 + left, x0 + left + pw, Y(t), Y(t)))
        out.append('<text x="%.1f" y="%.1f" class="tick" text-anchor="end">%s</text>'
                   % (x0 + left - 6, Y(t) + 4, fmt_tick(t)))
    for p in procs:
        out.append('<text x="%.1f" y="%.1f" class="tick" text-anchor="middle">%d</text>'
                   % (X(p), y0 + top + ph + 16, p))
    out.append('<line x1="%.1f" x2="%.1f" y1="%.1f" y2="%.1f" class="axis"/>'
               % (x0 + left, x0 + left + pw, y0 + top + ph, y0 + top + ph))
    out.append('<text x="%.1f" y="%.1f" class="label" text-anchor="middle">MPI processes (one node)</text>'
               % (x0 + left + pw / 2, y0 + h - 6))
    out.append('<text transform="translate(%.1f,%.1f) rotate(-90)" class="label" '
               'text-anchor="middle">%s</text>' % (x0 + 14, y0 + top + ph / 2, ylabel))
    if reference:
        pts = " ".join("%.1f,%.1f" % (X(p), Y(v)) for p, v in reference)
        out.append('<polyline points="%s" class="ref"/>' % pts)
        if ref_label:
            p, v = reference[-1]
            out.append('<text x="%.1f" y="%.1f" class="tick" text-anchor="end">%s</text>'
                       % (X(p) - 4, Y(v) - 8, ref_label))
    for strategy, pts in series.items():
        color = COLORS[strategy]
        path = " ".join("%.1f,%.1f" % (X(p), Y(v)) for p, v in pts)
        out.append('<polyline points="%s" fill="none" stroke="%s" stroke-width="2"/>'
                   % (path, color))
        for p, v in pts:
            out.append('<circle cx="%.1f" cy="%.1f" r="4" fill="%s" stroke="#fcfcfb" '
                       'stroke-width="2"><title>%s, p=%d: %.3f</title></circle>'
                       % (X(p), Y(v), color, strategy, p, v))
    return out


def chart(summary, experiment, path, meta_line):
    procs = sorted({r["nprocs"] for r in summary})
    by = {s: [r for r in summary if r["strategy"] == s] for s in STRATEGIES}
    W, H = 1080, 400
    pw = W / 3
    parts = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" '
        'font-family="-apple-system, Segoe UI, Helvetica, Arial, sans-serif">' % (W, H, W, H),
        "<style>"
        ".title{font-size:14px;font-weight:600;fill:#0b0b0b}"
        ".label{font-size:12px;fill:#52514e}"
        ".tick{font-size:11px;fill:#52514e}"
        ".grid{stroke:#e4e3df;stroke-width:1}"
        ".axis{stroke:#8a8983;stroke-width:1}"
        ".ref{fill:none;stroke:#8a8983;stroke-width:1.5;stroke-dasharray:5 4}"
        "</style>",
        '<rect width="100%" height="100%" fill="#fcfcfb"/>',
        '<text x="16" y="24" class="title" style="font-size:16px">ParallelCrack — %s '
        '(median of 3 runs per point)</text>' % EXPERIMENT_LABEL[experiment],
        '<text x="16" y="44" class="label">%s</text>' % meta_line,
    ]
    # Legend.
    lx = W - 250
    for i, s in enumerate(STRATEGIES):
        parts.append('<line x1="%d" x2="%d" y1="20" y2="20" stroke="%s" stroke-width="2"/>'
                     % (lx + i * 120, lx + i * 120 + 22, COLORS[s]))
        parts.append('<circle cx="%d" cy="20" r="4" fill="%s"/>' % (lx + i * 120 + 11, COLORS[s]))
        parts.append('<text x="%d" y="24" class="label">%s</text>' % (lx + i * 120 + 28, s))

    top = 52
    h = H - top
    tmax = max(r["max_wall_s"] for r in summary)
    parts += panel(0, top, pw, h, "Median wall time", "seconds",
                   {s: [(r["nprocs"], r["median_wall_s"]) for r in by[s]] for s in STRATEGIES},
                   procs, 0, tmax * 1.08)
    parts += panel(pw, top, pw, h, "Speedup vs. own 1-process median", "speedup (log scale)",
                   {s: [(r["nprocs"], r["speedup"]) for r in by[s]] for s in STRATEGIES},
                   procs, 0.9, max(procs) * 1.15, reference=[(p, p) for p in procs],
                   ref_label="ideal", log_y=True)
    emin = min(r["efficiency_pct"] for r in summary)
    emax = max(r["efficiency_pct"] for r in summary)
    parts += panel(2 * pw, top, pw, h, "Parallel efficiency", "efficiency (%)",
                   {s: [(r["nprocs"], r["efficiency_pct"]) for r in by[s]] for s in STRATEGIES},
                   procs, min(80, math.floor(emin / 5) * 5), max(105, math.ceil(emax / 5) * 5),
                   reference=[(p, 100) for p in procs], ref_label="ideal")
    parts.append("</svg>")
    with open(path, "w") as handle:
        handle.write("\n".join(parts) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--fullsweep", required=True)
    ap.add_argument("--early", required=True)
    ap.add_argument("--keyspace", type=int, required=True)
    ap.add_argument("--early-index", type=int, required=True)
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--label", default="Lotus compute14 · AMD EPYC 7352 (2×24 cores) · "
                    "length 5, lower-case alphabet, batch 2000 · SLURM job 27836")
    args = ap.parse_args()

    data = {"fullsweep": load(args.fullsweep), "early": load(args.early)}
    problems = []
    for name, rows in data.items():
        problems += validate(name, rows, args.keyspace, args.early_index)
    if problems:
        print("VALIDATION FAILED:")
        for p in problems:
            print("  " + p)
        return 1
    print("Validation passed: %d full-sweep rows, %d early-termination rows."
          % (len(data["fullsweep"]), len(data["early"])))

    for name, rows in data.items():
        summary = summarize(rows)
        write_summary_csv(summary, os.path.join(args.outdir, "summary_%s.csv" % name))
        chart(summary, name, os.path.join(args.outdir, "%s.svg" % name), args.label)
        print()
        print("## %s" % EXPERIMENT_LABEL[name])
        print()
        print(markdown_table(summary))
        print()
        print("candidates evaluated (min..max) and median hash rate:")
        for r in summary:
            print("  %-10s p=%d  %d..%d  %.2f M/s" % (r["strategy"], r["nprocs"],
                  r["min_candidates"], r["max_candidates"], r["median_hash_rate"] / 1e6))
    return 0


if __name__ == "__main__":
    sys.exit(main())
