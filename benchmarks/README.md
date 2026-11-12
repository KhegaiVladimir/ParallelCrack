# benchmarks/

Reproducible benchmarking for ParallelCrack. The goal is to measure how each
communication strategy scales as the number of MPI processes grows, using the same
synthetic workload every time.

Two scripts:

* **`run.sh`** — sweeps a set of process counts, runs each one a few times, and
  appends one measured CSV row per run.
* **`plot.py`** — reads the CSV(s) and draws wall time, speedup, efficiency, and
  hash rate (pandas + matplotlib).

Measured CSVs and plots are written under `results/`, which is git-ignored. No
measured numbers are committed, and none are fabricated.

## Metrics collected

Each run records (see `results/README.md` for the column list): wall-clock search
time, candidates evaluated, hash rate, and the time spent inside MPI coordination
(`comm_seconds`, the communication overhead). From the wall times, `plot.py`
derives speedup and parallel efficiency relative to the smallest process count in
the data. The polling interval for the asynchronous strategy is fixed in v0.1
(one batch; see the roadmap for adaptive polling).

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

## Running on the Lotus cluster

The target process counts for the scaling study are **1, 2, 4, 8, 16, 32, 48** on
a single node, with multi-node runs (96–960) planned for a later version. Load the
same modules used to build, then run the sweep inside a job allocation.

### Interactive allocation

```sh
module load OpenSSL/3          # same module used to build
make

# Grab an interactive allocation, then sweep. Use srun as the launcher.
salloc --nodes=1 --ntasks=48 --time=00:30:00
MPIRUN="srun --mpi=pmix" benchmarks/run.sh \
    --strategy collective --length 6 --full-sweep \
    --procs "1 2 4 8 16 32 48" --repeats 3 --csv results/collective_lotus.csv
```

### Batch job (sbatch)

Save this as `benchmarks/sweep.sbatch` and submit it with `sbatch`. It does **not**
launch automatically — you submit it yourself when you are ready.

```bash
#!/usr/bin/env bash
#SBATCH --job-name=parallelcrack
#SBATCH --nodes=1
#SBATCH --ntasks=48
#SBATCH --time=00:30:00
#SBATCH --output=slurm-%j.out

module load OpenSSL/3
make

for strat in collective async; do
    MPIRUN="srun --mpi=pmix" benchmarks/run.sh \
        --strategy "$strat" --length 6 --full-sweep \
        --procs "1 2 4 8 16 32 48" --repeats 3 \
        --csv "results/${strat}_${SLURM_JOB_ID}.csv"
done
```

Submit with:

```sh
sbatch benchmarks/sweep.sbatch
```

### Multi-node (planned, v0.5)

For 96–960 processes across multiple nodes, request more nodes and set `--ntasks`
accordingly (for example `--nodes=20 --ntasks=960`), then pass the larger
`--procs` list. Do not schedule these large jobs automatically; launch them
deliberately and keep each CSV labelled with its job id.

## Reproducibility notes

* Fix `--length`, `--alphabet`, and `--batch` across a sweep so the only variable
  is the process count.
* Use `--full-sweep` (target at the last index) or `--no-match` for the longest,
  most stable measurement; a target found early finishes before scaling effects
  are visible.
* Keep `--repeats` at 3 or more and let `plot.py` average them.
