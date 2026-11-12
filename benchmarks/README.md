# benchmarks/

Reproducible benchmarking for ParallelCrack. The goal is to measure how each
communication strategy scales as the number of MPI processes grows, using the same
synthetic workload every time.

Scripts:

* **`run.sh`** — local sweep: runs a set of process counts a few times and appends
  one measured CSV row per run (uses `mpirun` locally).
* **`plot.py`** — reads the CSV(s) and draws wall time, speedup, efficiency, and
  hash rate (pandas + matplotlib).
* **`analyze_results.py`** — validates the full-sweep and early-termination CSVs
  from one job, computes medians, spread, speedup, and efficiency, and writes
  summary CSVs plus SVG charts (standard library only).
* **`lotus_smoke.sbatch`** — a safe 4-process smoke test for Lotus (SLURM).
* **`lotus_benchmark.sbatch`** — the single-node (1–48 process) benchmark for Lotus.

Measured CSVs and plots are written under `results/`, which is git-ignored. The
exception is `results/lotus/<job>/`, where archived cluster runs are kept with
their metadata so documented results can be traced to raw data. Nothing is
fabricated.

## Metrics collected

Each run records (see `results/README.md` for the column list): wall-clock search
time, candidates evaluated, hash rate, and the time spent inside MPI coordination
(`comm_seconds`, the communication overhead). From the wall times, `plot.py`
derives speedup and parallel efficiency relative to the smallest process count in
the data. The asynchronous polling interval is configurable with `--poll`.

## Quick local run (laptop / single node)

```sh
make                                   # build ./parallelcrack
# Sweep both strategies over a worst-case (full-sweep) length-5 keyspace.
benchmarks/run.sh --strategy collective --length 5 --full-sweep \
    --procs "1 2 4 8" --repeats 3 --csv results/collective.csv
benchmarks/run.sh --strategy async --length 5 --full-sweep \
    --procs "1 2 4 8" --repeats 3 --csv results/async.csv

# Plot both on the same axes.
python3 benchmarks/plot.py results/collective.csv results/async.csv \
    --out results/scaling.png
```

`run.sh` uses `mpirun --oversubscribe` by default so it works when the process
count exceeds the number of cores.

## Running on the Lotus cluster (SLURM)

ParallelCrack is launched on Lotus with **`srun --mpi=pmix`**. This is the verified
configuration: PMIx correctly starts a single, coordinated MPI job. Launching with
`--mpi=pmi2` was observed to start independent *singleton* processes (each rank
thinks it is running alone), which is wrong for this program — **always use
`--mpi=pmix`.** OpenMPI (`openmpi5`) is loaded by default on Lotus; load OpenSSL the
same way you did to build (`module load OpenSSL/3`).

### SLURM concepts used here

* **Interactive allocation (`salloc`)** reserves nodes and gives you a shell on
  them; you then run `srun` *inside* that allocation. Good for experimenting.
* **Batch script (`sbatch script.sbatch`)** queues a script to run later on
  allocated compute nodes, with the `#SBATCH` lines describing the resources. Good
  for repeatable runs. The body runs on a compute node, never the login node.
* **`srun` inside a valid allocation** launches your MPI program across the tasks
  the allocation reserved. This is the correct way to run.
* **`srun` without a valid allocation** (e.g. on a login node with no reservation)
  either refuses or would run on the login node — which you must not do for
  benchmarks. Both provided scripts refuse to run heavy work unless `SLURM_JOB_ID`
  is set (i.e. inside an allocation).

### Two experiment types

* **Full-sweep benchmarking (`--full-sweep`)** — the target is deliberately
  **unmatchable**, so every strategy scans the entire keyspace and evaluates
  *exactly* the same number of candidates with no early termination. This is the
  fixed global workload for strong scaling, and the candidate count is
  deterministic (checked on every run).
* **Early-termination benchmarking (`--index K`)** — the target is a real candidate
  at a fixed index, so the search finds it and ranks stop early. This measures how
  quickly each strategy terminates once the answer is found. Here the asynchronous
  strategy legitimately evaluates fewer candidates than collective — that is the
  effect being measured, not an error.

### Stage 1 — smoke test (run this first)

`benchmarks/lotus_smoke.sbatch`: one node, **4 tasks**, 5-minute limit; one
collective and one asynchronous full-sweep on a small keyspace, with a correctness
check. It submits nothing else and exits non-zero if a sweep does not evaluate the
whole keyspace.

```sh
cat benchmarks/lotus_smoke.sbatch      # inspect before submitting
sbatch benchmarks/lotus_smoke.sbatch   # you submit it; nothing is automatic
```

### Stage 2 — single-node benchmark (only after the smoke test passes)

`benchmarks/lotus_benchmark.sbatch`: process counts **1, 2, 4, 8, 16, 32, 48** on
**one node**, run strictly sequentially, 5 repetitions each, with separate
full-sweep and early-termination experiments on a fixed workload. It exits non-zero
if any correctness check fails, and it never submits other jobs, spawns background
work, retries, or requests GPUs.

Preview the plan without computing anything (prints every planned command and the
number of MPI launches):

```sh
DRYRUN=1 bash benchmarks/lotus_benchmark.sbatch
```

Reduce the workload before submitting by overriding variables (Lotus propagates
your environment into the job by default), or by editing the `PROCS`/`REPEATS`
defaults at the top of the script:

```sh
PROCS="1 2 4" REPEATS=3 sbatch benchmarks/lotus_benchmark.sbatch
```

Submit it:

```sh
cat benchmarks/lotus_benchmark.sbatch   # inspect first
sbatch benchmarks/lotus_benchmark.sbatch
```

The default plan is 2 experiments × 2 strategies × 7 process counts × 5 repeats =
**140 MPI launches**, all on one node.

### Resources requested (maximum)

| Resource     | Benchmark                              | Smoke        |
|--------------|----------------------------------------|--------------|
| Partition    | `compute` (from Lab 2/3 — verify `sinfo`) | `compute`  |
| Nodes        | 1                                      | 1            |
| Tasks (max)  | 48                                     | 4            |
| Walltime     | 30 minutes                             | 5 minutes    |
| GPUs         | none                                   | none         |

This is intentionally **not** the professor's 35-node / 1680-task Lab 3 setup.
Confirm with `sinfo` / `scontrol show node` that compute nodes really provide 48
usable cores before submitting; reduce `--ntasks` and `PROCS` otherwise.

### Monitoring a job

```sh
squeue --me                                            # your queued/running jobs
sacct -j <jobid> --format=JobID,JobName,State,Elapsed,ExitCode
tail -f results/bench-<jobid>.out                      # live stdout
```

### Exporting results to your Mac

The benchmark writes one archive per job containing the CSVs, the stdout/stderr
logs, and a metadata file (git commit, compiler and MPI versions, CPU info, SLURM
job id, task/node counts, configuration, and the correctness result):

```sh
# on your Mac
scp <user>@lotus:~/ParallelCrack/results/parallelcrack_results_<jobid>.tar.gz ./
tar xzf parallelcrack_results_<jobid>.tar.gz
python3 benchmarks/plot.py results/fullsweep_<jobid>.csv --out results/fullsweep.png
```

`plot.py` also prints the averaged speedup/efficiency table to stdout.

## Reproducibility notes

* Fix `--length`, `--alphabet`, `--batch`, and `--poll` across a sweep so the only
  variable is the process count (strong scaling: one fixed global workload).
* Compare both strategies on the **same** inputs at each process count.
* Use `--full-sweep` for the fixed-work strong-scaling measurement (it scans the
  whole keyspace deterministically); use `--index K` for the early-termination
  experiment.
* Keep repetitions at 3+ and let `plot.py` average them.
* Results are real measurements only — never hand-edit a CSV, and do not publish
  scaling claims that have not been measured on the target hardware.
