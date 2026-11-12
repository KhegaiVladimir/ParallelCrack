# results/

This directory holds **measured** benchmark output: CSV files produced by real
runs and the plots generated from them.

Local output in here is not committed to git (see the repository `.gitignore`),
and nothing in here is fabricated. The one exception is `results/lotus/`, which
keeps the raw CSVs, metadata, and run logs of archived Lotus cluster runs (one
directory per SLURM job) so that results quoted in the documentation stay
reproducible. Each of those directories has its own README with the run details
and checksums. The numbers come from running the benchmark on real
hardware. To generate them:

```sh
# from the repository root
benchmarks/run.sh --strategy collective --length 5 --full-sweep \
    --procs "1 2 4 8" --csv results/collective.csv

python3 benchmarks/plot.py results/collective.csv --out results/collective.png
```

See [`../benchmarks/README.md`](../benchmarks/README.md) for the full set of
commands, including the SLURM batch form for the Lotus cluster.

Each CSV row is one run and records at least:

| column               | meaning                                             |
|----------------------|-----------------------------------------------------|
| `strategy`           | `collective` or `async`                             |
| `nprocs`             | number of MPI processes                             |
| `length`             | candidate length                                    |
| `alphabet_size`      | number of symbols in the candidate alphabet         |
| `batch_size`         | candidates searched between coordination points     |
| `found`              | 1 if the target was found, 0 otherwise              |
| `finder_rank`        | which rank found the target (-1 if not found)       |
| `candidates_evaluated` | total candidates hashed across all ranks          |
| `wall_seconds`       | measured wall-clock search time                     |
| `hashes_per_second`  | `candidates_evaluated / wall_seconds`               |
| `comm_seconds`       | time spent inside MPI coordination calls            |
