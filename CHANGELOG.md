# Changelog

All notable changes to ParallelCrack are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and the project aims to follow
semantic versioning once it reaches a tagged release.

## [Unreleased]

### Added
- Project foundation: directory layout, build system, and documentation skeleton.
- Deterministic keyspace search engine with SHA-256 candidate verification.
- Collective MPI strategy using `MPI_Allreduce` for coordinated early termination.
- Asynchronous MPI strategy using non-blocking point-to-point coordination.
- Synthetic AES-256-CBC encrypted file generation and recovery.
- Correctness tests for single- and multi-process runs.
- SLURM / mpirun benchmarking scripts and a pandas + matplotlib plotting script.

This section will be cut into a `v0.1.0` entry when the first release is tagged.
