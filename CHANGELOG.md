# Changelog

All notable changes to ParallelCrack are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and the project aims to follow
semantic versioning once it reaches a tagged release.

## [Unreleased]

### Added
- First Lotus cluster results (SLURM job 27836, one node, 1–8 processes): raw
  CSVs, metadata, and run log archived under `results/lotus/job27836/`.
- `benchmarks/analyze_results.py`: validates a job's CSVs and reports medians,
  spread, speedup, and efficiency, with SVG charts (no third-party packages).
- Preliminary results in the README and `docs/PERFORMANCE.md`.

## [0.1.0] — 2026-11-12

First independent release: a working comparison of two MPI communication
strategies over a shared, deterministic keyspace search.

### Added
- Project foundation: directory layout, build system, and documentation.
- Deterministic keyspace search engine with SHA-256 candidate verification and
  static cyclic work distribution.
- Collective MPI strategy using `MPI_Allreduce` for coordinated early termination.
- Asynchronous MPI strategy using non-blocking point-to-point coordination
  (`MPI_Iprobe` / `MPI_Irecv` / `MPI_Test` / `MPI_Send` / `MPI_Isend` / `MPI_Wait`).
- Synthetic AES-256-CBC encrypted file generation and recovery, verified against
  the original bytes.
- Unit tests (candidate coverage, determinism) and multi-process integration
  tests for both strategies.
- `mpirun` / SLURM benchmarking scripts and a pandas + matplotlib plotting script
  that derives speedup and parallel efficiency from measured data.
- Architecture, MPI protocol, and performance-methodology documentation.

### Notes
- Builds cleanly under `mpicc -std=c11 -Wall -Wextra -Wpedantic -Werror`.
- Measured benchmark data is never committed (the `results/` directory is
  git-ignored) and is never fabricated.
