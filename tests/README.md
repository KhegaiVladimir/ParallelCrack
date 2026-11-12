# tests/

Two layers of tests:

## 1. Unit tests — `test_search.c`

Pure, single-process checks of the shared search engine (no MPI). The key property
they verify is that the static cyclic distribution **covers every candidate in the
keyspace exactly once** for a range of process counts (1, 2, 3, 4, 5, 8), including
counts that do not evenly divide the keyspace. They also check deterministic
candidate generation, keyspace sizing (including overflow rejection), and target
setting/verification.

## 2. Integration tests — `run_tests.sh`

Run the real MPI program with `mpirun` and check the one-line output for each
required scenario, for **both** strategies:

| Requirement                         | How it is tested                                  |
|-------------------------------------|---------------------------------------------------|
| Single / two / four MPI processes   | `-np 1`, `-np 2`, `-np 4` runs that find the target |
| Target found by rank 0              | `--index 0` → `finder_rank=0`                      |
| Target found by a worker            | `--index 3` with 4 ranks → `finder_rank=3`        |
| Target not found                    | `--no-match` → `found=0`, full keyspace scanned   |
| Keyspace smaller than ranks         | length 1 (26 candidates) with 8 ranks             |
| Uneven work distribution            | 3 ranks over a 17576 keyspace, full scan          |
| Correct file recovery               | `--recover-file` → `recovery=1`                   |
| No missing/duplicated candidates    | unit test coverage check + full-scan counts       |
| Both strategies agree               | collective vs async finder cross-check            |
| Full-sweep evaluates whole keyspace | `--full-sweep` candidate count == keyspace, both strategies, np 1–4 |

The full-sweep check is important: a full sweep must evaluate **exactly** the whole
keyspace, deterministically, for every strategy. An earlier version let the
asynchronous strategy's early-termination race undercount the sweep (because a
findable last-index target let one rank stop the others a few batches short). The
fix makes `--full-sweep` use an unmatchable target so no rank terminates early, and
these tests assert the exact count.

## Running

From the repository root:

```sh
make test
```

That builds the program and the unit-test binary, runs the unit tests, then runs
the integration script. The integration script defaults to
`mpirun --oversubscribe` so it works on a laptop with fewer cores than the process
counts it exercises.

To run the pieces by hand:

```sh
# unit tests only
make build/test_search && build/test_search

# integration tests only (override the launcher if you are on a cluster)
MPIRUN="mpirun --oversubscribe" PROG=./parallelcrack bash tests/run_tests.sh
```

## A note on MPI request cleanup and deadlock freedom

The integration suite runs many process counts and scenarios, and every run
completes and calls `MPI_Finalize()` cleanly (launcher exit code 0). That is
strong practical evidence that no requests leak and no messages are left
unreceived, because the asynchronous strategy is written so that message counts
are exactly balanced (each worker sends one completion and consumes one stop).

This is **not** a formal proof of deadlock freedom. Verifying that rigorously
would need a tool such as MUST or Intel Trace Analyzer on the target cluster;
passing tests here means "did not deadlock in these runs", not "cannot deadlock".
