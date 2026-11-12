# ParallelCrack — MPI Communication & Performance Benchmark

ParallelCrack is a small, self-contained HPC project for studying **how the choice
of MPI communication strategy affects the performance of a distributed keyspace
search**. It uses a synthetic password-search workload as a consistent, repeatable
benchmark so that the only thing that changes between experiments is the way the
processes coordinate with each other.

The research question is:

> How do different MPI communication and coordination strategies affect
> distributed keyspace search performance, scalability, and early termination?

This is a **performance-engineering project**, not a tool for attacking real
credentials. Everything it searches is generated locally: the target "password"
is a short string that the program picks itself, and any encrypted file it
recovers is one the program encrypted a moment earlier with a key it generated.
There are no real hashes, no third-party targets, and no user secrets involved.

## Why password search?

A brute-force keyspace search is a convenient benchmark for parallel coordination
because it has three properties that stress the communication layer in interesting
ways:

1. **The work is trivially parallel per candidate** — every candidate can be tested
   independently, so the interesting part is purely the coordination, not the math.
2. **It terminates early** — as soon as one process finds the answer, every other
   process should stop as quickly as possible. How fast that happens depends
   entirely on the communication strategy.
3. **It has a clean, verifiable answer** — we know exactly which candidate is
   correct, so we can check that every strategy returns the same result.

That makes it a good lens for comparing, say, a collective barrier-style
coordination against a message-driven asynchronous one.

## What it does

* Generates candidates **deterministically** from an integer index, so every rank
  agrees on exactly which candidate a given index maps to.
* Distributes the keyspace across MPI ranks with a **static cyclic** scheme.
* Verifies candidates with **SHA-256** (via OpenSSL), the same way in every strategy.
* Stops all ranks as soon as the target is found (**early termination**).
* Optionally recovers a locally generated **AES-256-CBC** encrypted file using the
  password it found, and checks the recovered bytes against the original.
* Reports real, measured metrics (wall time, candidates evaluated, hash rate,
  communication overhead) and can append them to a CSV for plotting.

## Communication strategies (v0.1)

| Strategy       | Key MPI calls                                        | Idea                                                                 |
|----------------|------------------------------------------------------|---------------------------------------------------------------------|
| `collective`   | `MPI_Allreduce`                                       | Every rank searches a batch, then all ranks agree on global state.  |
| `asynchronous` | `MPI_Iprobe`, `MPI_Irecv`, `MPI_Test`, `MPI_Send`, `MPI_Isend`, `MPI_Wait` | Rank 0 coordinates with messages while also doing real search work. |

Both strategies share the **exact same** candidate generator, keyspace, batch
loop, and verification logic. The only difference is how they coordinate. That is
what makes the comparison fair.

## Project layout

```
ParallelCrack/
├── include/            # Public headers (the interfaces between modules)
│   ├── crypto.h        #   hashing + file encryption helpers
│   ├── search.h        #   candidate generation + the shared search workload
│   ├── strategies.h    #   the common strategy interface + result struct
│   └── benchmark.h     #   timing and metrics helpers
├── src/
│   ├── main.c          # CLI parsing + orchestration
│   ├── crypto.c        # OpenSSL SHA-256 and AES-256-CBC
│   ├── search.c        # deterministic candidate generation + batch search
│   ├── benchmark.c     # metrics + CSV output
│   └── strategies/
│       ├── collective.c    # MPI_Allreduce strategy
│       └── asynchronous.c  # message-driven strategy
├── tests/              # correctness tests (compile + local MPI runs)
├── benchmarks/         # SLURM / mpirun scripts + plotting
├── results/            # (git-ignored) measured CSVs and plots live here
└── docs/               # architecture + MPI protocol notes + methodology
```

## Building

You need an MPI compiler (`mpicc`) and OpenSSL 3 development headers.

```sh
make                      # builds ./parallelcrack
make clean                # removes build output
```

On the Lotus cluster:

```sh
module load OpenSSL/3      # puts openssl/*.h and libcrypto on the search path
make
```

On macOS with Homebrew (used during development):

```sh
brew install open-mpi openssl@3
make OPENSSL_PREFIX=$(brew --prefix openssl@3)
```

## Running

```sh
# Quick correctness run: small keyspace, target found quickly, 4 processes
mpirun -np 4 ./parallelcrack --strategy collective --alphabet lower --length 4

# The asynchronous strategy on the same workload
mpirun -np 4 ./parallelcrack --strategy async --alphabet lower --length 4

# Force a target that no candidate matches (exercises the no-match path)
mpirun -np 4 ./parallelcrack --strategy async --length 4 --no-match

# Recover a locally generated encrypted file after finding the password
mpirun -np 4 ./parallelcrack --length 4 --recover-file /tmp/pc_demo.bin

# Append a metrics row to a CSV for later plotting
mpirun -np 8 ./parallelcrack --length 5 --full-sweep --csv results/run.csv
```

Run `./parallelcrack --help` for the full list of options.

## Benchmarking

`benchmarks/run.sh` sweeps a range of process counts, writes one CSV row per run,
and `benchmarks/plot.py` turns that CSV into speedup / efficiency / hash-rate
plots. See [`benchmarks/README.md`](benchmarks/README.md) for the reproducible
commands, including the SLURM batch form for Lotus.

Measured results are **not** committed and are **never** fabricated. The
`results/` directory holds a README explaining how to generate them; the actual
numbers come from your own runs.

## Roadmap

See [`ROADMAP.md`](ROADMAP.md). v0.1 delivers the collective and asynchronous
strategies; later versions add dynamic scheduling, ring communication, adaptive
polling, multi-node scaling, and a hybrid MPI + OpenMP mode.

## Documentation

* [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — modules and interfaces.
* [`docs/MPI_PROTOCOLS.md`](docs/MPI_PROTOCOLS.md) — the communication patterns, with diagrams.
* [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md) — measurement methodology.

## License

See [`LICENSE`](LICENSE) (MIT).
