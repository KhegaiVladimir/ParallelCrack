# Lotus quick benchmark — SLURM job 27836

First real cluster run of ParallelCrack. These files are kept in the repository
(unlike the rest of `results/`) so the numbers quoted in the README and
`docs/PERFORMANCE.md` can be traced back to raw data.

## Run details

| | |
|---|---|
| Date (UTC) | 2026-11-12, started 08:09:07 |
| Cluster / node | Lotus, `compute14` (one node) |
| CPU | AMD EPYC 7352, 2 sockets × 24 cores |
| Allocation | 1 node, 8 tasks (`benchmarks/lotus_quick.sbatch`) |
| Launcher | `srun --mpi=pmix` |
| Compiler | GCC 13.3.0 (`mpicc`) |
| SLURM | 25.11.4 |
| OpenSSL | 3.2.1 |
| Code version | `36f4cdb` (`v0.1.0-7-g36f4cdb`) |
| Workload | length 5, lower-case alphabet (26), keyspace 11,881,376, batch 2000, poll 1 |
| Early-termination target | index 5,940,688 (middle of the keyspace) |
| Plan | 2 experiments × 2 strategies × p ∈ {1, 2, 4, 8} × 3 repetitions = 48 launches |
| Outcome | 48/48 launches, exit 0, `correctness_validation=PASSED` |

## Files

Raw output, unmodified from the job's results archive:

* `quick_fullsweep_27836.csv` — 24 full-sweep runs
* `quick_early_27836.csv` — 24 early-termination runs
* `quick_meta_27836.txt` — environment metadata written by the job
* `quick_run_27836.log` — per-launch log with the program's own output line

Derived (regenerate with the command below):

* `summary_fullsweep.csv`, `summary_early.csv` — medians, spread, CV, speedup, efficiency
* `fullsweep.svg`, `early.svg` — charts drawn from the summaries

```sh
python3 benchmarks/analyze_results.py \
    --fullsweep results/lotus/job27836/quick_fullsweep_27836.csv \
    --early     results/lotus/job27836/quick_early_27836.csv \
    --keyspace 11881376 --early-index 5940688 \
    --outdir    results/lotus/job27836
```

The script re-checks the data before summarizing it: identical workload settings
in every row, exactly 11,881,376 candidates and no match for every full-sweep
row, the correct index and owning rank for every early-termination row, and
`hashes_per_second` consistent with `candidates_evaluated / wall_seconds`.

## Checksums (SHA-256)

```
51b7c0ed2eb3c5c5026911e8422095b75d719ee3813064309d681840476cc6fc  parallelcrack_quick_27836.tar.gz (original archive, not committed)
d5f3b04e473d6e29c60430aae3755b55a9d22dc1c720665de70cec0615b91f01  quick_fullsweep_27836.csv
0c0638e07c4c82f5c84f43dbd1a815b798bc3792c5f974bba6eb79022d7bab1e  quick_early_27836.csv
52da1dc9e1312b5b81e76c55703f13a0bfdf1cade0b14b26c2a848da46eabca8  quick_meta_27836.txt
b6c981111d8a2f53f8fb8e5bdc2c5bb54c4413f328b715d9c63de5d6d75d41bd  quick_run_27836.log
```

The SLURM `.out` / `.err` files were not part of the archive and are not included.
