# Architecture

ParallelCrack is deliberately small. It has one shared search engine and a set of
interchangeable communication strategies that drive it. Everything else (crypto,
benchmarking, the CLI) is support code around those two ideas.

## Module map

```
            +--------------------+
            |      main.c        |   CLI parsing, config broadcast,
            |  (the driver)      |   strategy dispatch, reporting, CSV
            +----------+---------+
                       |
          selects one  |  strategy_fn
                       v
        +--------------+---------------+
        |     src/strategies/*.c       |   HOW the ranks coordinate
        |  collective.c  asynchronous.c|   (MPI lives here)
        +--------------+---------------+
                       |
          both call    |  the same functions
                       v
            +----------+---------+
            |     search.c       |   WHAT is searched: deterministic
            |  (shared workload) |   candidate generation + batch search
            +----------+---------+
                       |
          verifies with|
                       v
            +----------+---------+
            |     crypto.c       |   SHA-256 verification + AES-256-CBC
            |  (OpenSSL wrapper) |   file encryption/recovery
            +--------------------+

            +--------------------+
            |   benchmark.c      |   append one measured row to a CSV
            +--------------------+
```

The important structural rule: **MPI appears only in the strategy files and in
main.c**. The search engine and crypto code are plain serial C with no knowledge
of ranks or messages. That separation is what lets a new strategy reuse the exact
same workload.

## The interfaces

### `search.h` — the shared workload

* `search_config` holds the alphabet, candidate length, keyspace size, batch size,
  and the target digest. It is built once and never mutated during a search.
* `search_index_to_candidate()` is the deterministic bijection from an integer
  index to a candidate string. Every rank computes the same candidate for the same
  index, which is why ranks never disagree about the keyspace.
* `search_batch()` is the unit of work every strategy calls. Given a starting
  index and a stride, it tests up to `batch_size` candidates from a rank's cyclic
  slice and reports how many it evaluated, where to resume, and whether it matched.
* `search_check_index()` verifies a single candidate against the target.

### `strategies.h` — the strategy contract

Every strategy has the same signature:

```c
int strategy_run(const search_config *config, MPI_Comm comm, search_result *out);
```

and fills in a `search_result` (found flag, found index, finder rank, recovered
password, and the measured metrics). The function pointer type `strategy_fn` lets
`main.c` pick a strategy at runtime from a one-line dispatch.

Adding a strategy is three steps and never touches the workload:

1. Write `src/strategies/<name>.c` implementing the signature.
2. Declare it in `strategies.h`.
3. Add one branch to the dispatch in `main.c`.

### `crypto.h` — verification and file recovery

* `crypto_sha256()` is the verification primitive used by the search.
* `crypto_key_from_password()` + `crypto_encrypt_file()` / `crypto_decrypt_file()`
  implement the synthetic file-recovery demo with AES-256-CBC.

All OpenSSL usage is behind this header, using the modern EVP interface so the
build stays free of deprecation warnings.

### `benchmark.h` — measurement output

`benchmark_append_csv()` writes one measured row (self-describing header on first
write). It is the only place benchmark data is produced.

## Data flow of one run

1. Rank 0 parses the command line into a small plain-data `run_params`.
2. `run_params` is broadcast to all ranks with a single `MPI_Bcast`.
3. Every rank independently builds an identical `search_config` from those
   parameters — including hashing the target locally, so the digest is never sent.
4. The selected strategy runs, coordinating via MPI and calling the shared
   `search_batch()` for the actual work.
5. Rank 0 collects the metrics, prints them, optionally runs the file-recovery
   demo, and optionally appends a CSV row.

## Keyspace distribution

All strategies use **static cyclic** distribution: rank `r` of `P` owns indices
`r, r+P, r+2P, ...`. This is simple, requires no coordination to assign work, and
spreads candidates so that no single rank owns a contiguous block (which keeps the
load balanced even when the answer is clustered in one region). The unit test in
`tests/test_search.c` proves this partition covers every index exactly once.

Dynamic (on-demand) distribution is planned for v0.2; it will be a new strategy,
not a change to the search engine.
