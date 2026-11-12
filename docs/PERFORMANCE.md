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

## Two experiment types

The benchmark runs two distinct experiments, because "how fast does it scan
everything" and "how fast does it stop once it finds the answer" are different
questions:

* **Full-sweep (`--full-sweep`)** — the target is **unmatchable**, so every
  strategy scans the entire keyspace and evaluates *exactly* `keyspace` candidates
  with no early termination. This is the fixed, deterministic global workload used
  for strong scaling, and it guarantees both strategies do identical work. The
  candidate count is validated on every run.

  > This matters: an earlier version put a *findable* target at the last index.
  > With the asynchronous strategy, the rank that owns that index found it and
  > signalled the others to stop, so they abandoned their last few batches — a
  > timing-dependent undercount (e.g. 11,824,344 instead of 11,881,376 for length
  > 5). A valid fixed-work measurement must scan everything, so full-sweep now uses
  > an unmatchable target.

* **Early-termination (`--index K`)** — the target is a real candidate at a fixed
  index, identical for both strategies. The search finds it and ranks stop early.
  Here the asynchronous strategy evaluating fewer candidates than collective is the
  *measured effect*, not an error. This experiment measures how quickly each
  strategy terminates once the answer is found.

For either experiment, hold everything else constant across process counts: fix
`--length`, `--alphabet`, `--batch`, and `--poll`, use ≥ 3 repetitions, and let
`plot.py` average them.

## Execution modes on Lotus (SLURM)

* **Interactive allocation (`salloc`)** reserves nodes; you then run `srun` inside
  it. **Batch (`sbatch`)** queues a script to run on compute nodes later. In both,
  **`srun` inside the allocation** launches the MPI job across the reserved tasks.
  Running **`srun` without a valid allocation** would fall back to the login node —
  which must never be used for benchmarks; the provided scripts refuse to do so.
* ParallelCrack is launched with **`srun --mpi=pmix`** — the verified PMI. PMI2 was
  observed to start uncoordinated singleton processes.

## Process-count plan

* **Initial benchmark — single node only:** 1, 2, 4, 8, 16, 32, 48 processes,
  five repetitions each, run strictly sequentially via
  `benchmarks/lotus_benchmark.sbatch`.
* Larger, multi-node counts are intentionally out of scope for this stage. The
  professor's 35-node / 1680-task configuration is **not** used. Any future
  multi-node work (see the roadmap) is launched deliberately, never automatically.

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
