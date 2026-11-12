# ParallelCrack Roadmap

The project grows one communication idea at a time. Each version adds a new
coordination strategy (or a new axis of scaling) **without rewriting the shared
search workload**. The candidate generator, keyspace layout, and verification
logic stay fixed, so every version is directly comparable to the previous ones.

## v0.1 — Collective vs. asynchronous communication *(current)*

The baseline comparison.

* **Collective strategy** — ranks search in batches and use `MPI_Allreduce` to
  agree on global state (found / still-active) between batches.
* **Asynchronous strategy** — rank 0 coordinates with point-to-point messages
  (`MPI_Iprobe` / `MPI_Irecv` / `MPI_Test` / `MPI_Send`) while also searching.
* Shared, deterministic workload with SHA-256 verification.
* Synthetic AES-256-CBC encrypted file recovery.
* Correctness tests and a benchmarking + plotting pipeline.

## v0.2 — Dynamic scheduling

Replace static cyclic distribution with a work-queue model: a coordinator hands
out keyspace chunks on demand so that faster ranks pick up more work. This targets
the load imbalance that shows up when per-candidate cost varies or when ranks run
on heterogeneous hardware.

## v0.3 — Ring-based communication

Coordinate termination by passing a token around a logical ring instead of using a
central coordinator or a global collective. Useful for studying how coordination
latency scales when you avoid an all-to-all synchronization point.

## v0.4 — Adaptive polling

In the asynchronous strategy the polling interval (how often a worker checks for a
stop message) is a tunable knob: poll too often and you waste time on communication,
poll too rarely and early termination is slow. v0.4 makes the interval adapt at
runtime based on observed message latency and batch cost.

## v0.5 — Multi-node scaling

Extend the benchmarking methodology to multiple Lotus nodes (96–960 ranks) and
study how each strategy's communication overhead behaves across node boundaries,
where message latency is higher than within a single node.

## v0.6 — Hybrid MPI + OpenMP

Add a shared-memory layer: one MPI rank per node (or per socket) with OpenMP
threads searching within each rank. This reduces the number of MPI endpoints that
have to coordinate and lets us compare pure-MPI against hybrid decomposition on the
same workload.

---

### Design rule that keeps the versions comparable

Every strategy implements the same interface declared in
[`include/strategies.h`](include/strategies.h):

```c
int strategy_run(const search_config *config, MPI_Comm comm, search_result *out_result);
```

A new strategy is a new `.c` file under `src/strategies/` plus one line in the
strategy dispatch table. It reuses the shared search engine in `src/search.c`
unchanged, so adding a communication pattern never touches the workload definition.
