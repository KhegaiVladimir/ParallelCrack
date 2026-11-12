# Performance Methodology

This document defines *how* ParallelCrack is measured so that results are
reproducible and honest. The actual experimental findings are intentionally left
blank until they are produced on real hardware — see the placeholder section at
the end.

## What is measured

Each run records the following, written as one CSV row (see `results/README.md`):

| Metric                 | Definition                                                        |
|------------------------|-------------------------------------------------------------------|
| `wall_seconds`         | Wall-clock time of the search, measured on rank 0.                 |
| `candidates_evaluated` | Total candidates hashed across all ranks.                         |
| `hashes_per_second`    | `candidates_evaluated / wall_seconds`.                            |
| `comm_seconds`         | Max per-rank time spent inside MPI coordination (overhead).       |
| `rounds`               | Coordination rounds (collective) or batches by rank 0 (async).    |
| `found`, `finder_rank` | Outcome of the search.                                            |

From a sweep across process counts, `benchmarks/plot.py` derives:

* **Speedup** `S(p) = T(baseline) / T(p)`
* **Parallel efficiency** `E(p) = S(p) / (p / baseline_p)`

where the baseline is the smallest process count in the data (normally 1).

## How the timing is taken

* The search is bracketed by `MPI_Barrier` then `MPI_Wtime` at the start, and
  `MPI_Wtime` at the end, inside each strategy. The barrier ensures all ranks
  start the timed region together, so the wall time reflects the parallel search
  and not startup skew.
* **File decryption is never inside the timed region.** The file-recovery demo
  runs on rank 0 after the search completes, so encryption/decryption time does
  not pollute the search measurement.
* `comm_seconds` is accumulated by wrapping only the MPI coordination calls
  (`Allreduce` / `Iprobe` / `Test` / `Send` / `Wait` …) with `MPI_Wtime` and then
  taking the maximum across ranks. It therefore includes synchronization wait time,
  which is a real part of coordination cost.

## Controlling the workload

For a scaling study, hold everything constant except the process count:

* Fix `--length`, `--alphabet`, and `--batch`.
* Use `--full-sweep` (target at the last index) or `--no-match` so the search is
  long and stable. A target found early finishes before scaling behavior is
  visible and produces noisy timings.
* Use `--repeats 3` (or more) and let `plot.py` average the repeats.

## Process-count plan

* **Single node:** 1, 2, 4, 8, 16, 32, 48 processes.
* **Multi-node (planned, v0.5):** 96–960 processes. These are launched
  deliberately, never scheduled automatically.

## Expected effects to look for

These are *hypotheses to test*, not results:

* The collective strategy should show rising `comm_seconds` as `p` grows, because
  every batch pays for a full synchronization whose cost grows with `p`.
* The asynchronous strategy should show lower coordination overhead, but rank 0
  doing double duty (coordinator + worker) may limit its own search throughput.
* Early-termination latency (how quickly all ranks stop after the first match)
  should favor the asynchronous strategy at large `p`, since it does not wait for a
  global collective to propagate the result.

## Threats to validity

* **Oversubscription:** running more MPI processes than physical cores (as the
  local test launcher does) distorts timings. Treat laptop numbers as smoke tests;
  take real measurements on dedicated cluster cores.
* **Warm-up and caching:** the first run of a configuration can be slower. Keep
  `--repeats` ≥ 3 and consider discarding the first.
* **Hash cost dominates:** the per-candidate SHA-256 is cheap and uniform, so this
  benchmark stresses *coordination*, not computation. That is intentional, but it
  means conclusions are about communication strategy, not raw crypto throughput.

---

## Experimental results

> Not yet filled in. Run the sweeps in `benchmarks/README.md` on the target
> hardware, commit the generated plots outside this repository (the `results/`
> directory is git-ignored), and summarize the findings here with the hardware
> description, process counts, and date of the runs.

| Date | Hardware | Strategy | Length | Max procs | Peak speedup | Notes |
|------|----------|----------|--------|-----------|--------------|-------|
|      |          |          |        |           |              |       |
