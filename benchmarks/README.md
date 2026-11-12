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

Lotus uses SLURM, and MPI programs are launched with `srun --mpi=pmi2` (the same
launcher as the Lab 3 batch file). OpenMPI (`openmpi5`) is loaded by default; load
OpenSSL the same way you did to build.

### Process counts and node mapping

The strong-scaling study uses the Lab 4 process counts:

```
1, 2, 4, 8, 16, 32, 48, 96, 192, 384, 768, 960
```

With `--ntasks-per-node=48` (48 cores per compute node, as in the Lab 2/Lab 3
batch files), the node counts are:

| Processes | Nodes (`-N`) | Processes | Nodes (`-N`) |
|-----------|--------------|-----------|--------------|
| 1 – 48    | 1            | 192       | 4            |
| 96        | 2            | 384       | 8            |
| —         | —            | 768       | 16           |
| —         | —            | 960       | 20           |

The largest run (960 tasks) needs 20 nodes. Lab 3 ran on 35 nodes, so these counts
are within the compute partition's capacity — but **confirm `sinfo` on Lotus**
before requesting large allocations, since partition limits can change.

### Interactive run (a single process count)

```sh
module load OpenSSL/3
make

# Example: 8 processes, both strategies, full-sweep workload.
srun --mpi=pmi2 -n 8 ./parallelcrack --strategy collective --full-sweep
srun --mpi=pmi2 -n 8 ./parallelcrack --strategy async --full-sweep
```

### Interactive sweep (single node, up to 48)

```sh
salloc --partition=compute --nodes=1 --ntasks=48 --time=00:30:00
module load OpenSSL/3
make
MPIRUN="srun --mpi=pmi2" benchmarks/run.sh \
    --strategy collective --length 6 --full-sweep \
    --procs "1 2 4 8 16 32 48" --repeats 3 --csv results/collective_lotus.csv
MPIRUN="srun --mpi=pmi2" benchmarks/run.sh \
    --strategy async --length 6 --full-sweep \
    --procs "1 2 4 8 16 32 48" --repeats 3 --csv results/async_lotus.csv
```

### Full benchmark (batch job, multi-node)

Save this as `benchmarks/sweep.sbatch` and submit it yourself with `sbatch` — it
does **not** launch automatically. It follows the Lab 3 batch style
(`#SBATCH -n`, `-N`, `--ntasks-per-node=48`). This example covers the single-node
counts; for the multi-node counts (96–960) raise `-N`/`-n` per the table above and
submit a separate job, keeping each CSV labelled with its job id.

```bash
#!/usr/bin/env bash
#SBATCH --job-name=parallelcrack
#SBATCH --partition=compute
#SBATCH -N 1
#SBATCH -n 48
#SBATCH --ntasks-per-node=48
#SBATCH --time=00:30:00
#SBATCH --output=slurm-%j.out

module load OpenSSL/3
make

# Strong scaling: fixed global workload (length 6, full sweep), both strategies,
# identical inputs, repeated 3x. Each run writes one CSV row.
for strat in collective async; do
    MPIRUN="srun --mpi=pmi2" benchmarks/run.sh \
        --strategy "$strat" --length 6 --full-sweep \
        --procs "1 2 4 8 16 32 48" --repeats 3 \
        --csv "results/${strat}_${SLURM_JOB_ID}.csv"
done
```

Submit with:

```sh
sbatch benchmarks/sweep.sbatch
```

For the multi-node counts, change the header to the row you need, e.g. for 960
processes: `#SBATCH -N 20`, `#SBATCH -n 960`, `#SBATCH --ntasks-per-node=48`, and
set `--procs "96 192 384 768 960"`.

## Exporting results for analysis

The CSVs under `results/` are plain text and ready for analysis anywhere:

```sh
# Combine strategy CSVs and plot speedup / efficiency / hash rate.
python3 benchmarks/plot.py results/collective_lotus.csv results/async_lotus.csv \
    --out results/scaling.png

# Or copy the CSVs off the cluster for analysis elsewhere.
scp lotus:~/ParallelCrack/results/*_lotus.csv ./local-results/
```

`plot.py` also prints the averaged table (speedup and efficiency per process
count) to stdout, so the numbers are visible without opening the image.

## Reproducibility notes

* Fix `--length`, `--alphabet`, `--batch`, and `--poll` across a sweep so the only
  variable is the process count (strong scaling: one fixed global workload).
* Compare both strategies on the **same** inputs at each process count.
* Use `--full-sweep` (target at the last index) or `--no-match` for the longest,
  most stable measurement; a target found early finishes before scaling effects
  are visible.
* Keep `--repeats` at 3 or more and let `plot.py` average them.
* Results are real measurements only — never hand-edit a CSV.
