# v0.2 — Dynamic Scheduling (design)

This document specifies the next strategy before any code is written. It follows
the design rule from the roadmap: a new strategy is a new `.c` file under
`src/strategies/` plus one line in the dispatch table in `main.c`. The shared
search engine in `src/search.c` and the workload definition stay untouched, so the
dynamic strategy stays directly comparable to collective and asynchronous.

## Why

v0.1 splits the keyspace with a static cyclic layout: rank `r` owns candidates
`r, r + P, r + 2P, ...`. That is perfectly balanced only when every candidate
costs the same and every rank runs at the same speed. It breaks down when:

* per-candidate cost varies (e.g. a future verifier with early-exit paths), or
* ranks run on heterogeneous or contended hardware (one slow node drags the whole
  run, because the last rank to finish its fixed slice sets the wall time).

Dynamic scheduling replaces the fixed slice with a **work queue**: a coordinator
hands out keyspace chunks on demand, so a faster rank simply asks for the next
chunk sooner and ends up doing more work. No rank is stuck waiting on a slow peer's
pre-assigned slice.

## Roles

One coordinator (rank 0), the rest are workers. The coordinator also searches
between handing out chunks, so it is not a wasted rank at small `P`.

* **Coordinator** — owns a single cursor into the keyspace and answers chunk
  requests. It never searches a chunk it has handed to someone else.
* **Worker** — loops: request a chunk, search it, repeat until told the keyspace is
  exhausted or a match was found.

A chunk is a half-open global index range `[start, start + chunk_size)`, clamped to
the keyspace end. `chunk_size` is a tunable (`--chunk`, default TBD after a sweep),
analogous to `batch`/`poll` in the existing strategies.

## Messages

Four logical message types, distinguished by MPI tag:

```
TAG_REQUEST   worker -> coordinator   "give me work" (payload: none / finished-chunk stats)
TAG_GRANT     coordinator -> worker   a chunk [start, len], or len == 0 meaning "done"
TAG_FOUND     worker -> coordinator   "I found it at global index i" (+ password bytes)
TAG_STOP      coordinator -> worker   "stop now, match already found"
```

Point-to-point only; no collective on the hot path. A final `MPI_Reduce` collects
totals onto rank 0 for reporting, exactly as the other strategies do.

## Coordinator loop (pseudocode)

```
cursor      = 0
live        = number of workers (ranks that still need a "done" grant)
found       = false
winner_idx  = LONG_MAX

while live > 0 or coordinator still has a local chunk to search:

    # 1. Drain any pending worker messages without blocking.
    while Iprobe(ANY_SOURCE):
        recv the message
        if tag == TAG_REQUEST:
            if found:
                send TAG_GRANT(len = 0) to that worker      # tell it to stop
                live -= 1
            else if cursor >= keyspace:
                send TAG_GRANT(len = 0) to that worker       # keyspace exhausted
                live -= 1
            else:
                start = cursor
                len   = min(chunk_size, keyspace - cursor)
                cursor += len
                send TAG_GRANT(start, len) to that worker
        else if tag == TAG_FOUND:
            if msg.index < winner_idx:                       # keep the smallest index
                winner_idx = msg.index
                winner_pw  = msg.password
            found = true
            # outstanding workers are told to stop when they next REQUEST,
            # or proactively below

    # 2. If a match exists, proactively tell any worker we have not stopped yet.
    if found:
        send TAG_STOP to every worker not yet marked done   # idempotent; see note

    # 3. Do a slice of useful work ourselves so rank 0 is not idle.
    if not found and cursor < keyspace:
        grab one chunk the same way a worker would (advance cursor locally)
        search it; if it matches, set found / winner_idx as above

report winner_idx, winner_pw, totals via MPI_Reduce
```

## Worker loop (pseudocode)

```
loop:
    send TAG_REQUEST to coordinator
    recv TAG_GRANT                       # blocking: we have nothing to do until we get work

    if grant.len == 0:
        break                            # coordinator says: exhausted or already found

    # Search the granted chunk, but stay responsive to an early TAG_STOP.
    for each sub-batch of [grant.start, grant.start + grant.len):
        search the sub-batch
        if match found in this sub-batch:
            send TAG_FOUND(index, password) to coordinator
            break out of the chunk           # let coordinator pick the global winner
        if Iprobe(coordinator, TAG_STOP):
            recv TAG_STOP
            goto done                        # someone else found it
    # loop back and request the next chunk

done:
    participate in the final MPI_Reduce
```

The chunk is searched in sub-batches (reuse the existing `batch` size) so a worker
checks for `TAG_STOP` at the same cadence the async strategy polls — early
termination latency stays comparable across strategies.

## Termination — the three problems, restated

1. **Split the work** — on demand via the coordinator's cursor (this is the whole
   point of the version).
2. **Stop early** — `TAG_FOUND` to the coordinator, which fans out `TAG_STOP` and
   refuses further chunks. The coordinator picks the smallest index among all
   `TAG_FOUND` messages, so the result is identical to the deterministic global
   winner the other strategies agree on.
3. **No match** — the cursor reaches `keyspace`; every subsequent `TAG_REQUEST` is
   answered with a zero-length grant, each worker exits, `live` hits 0, done.

## Correctness invariants

* **Full coverage, no overlap** — the cursor is the single source of truth for what
  has been handed out. Every granted range is disjoint and contiguous, and their
  union is exactly `[0, keyspace)` when the run completes a full sweep. A unit test
  asserts that the multiset of granted indices equals `[0, keyspace)` for a small
  keyspace with several workers and an artificially tiny chunk size.
* **Deterministic winner** — the reported match is the smallest matching index
  regardless of which rank found it or in what order `TAG_FOUND` messages arrive,
  matching collective/async output byte-for-byte on the same workload.
* **No deadlock** — the coordinator never blocks on a worker (all probes are
  non-blocking); workers only ever block on a `TAG_GRANT` they are guaranteed to
  receive, because every `TAG_REQUEST` is answered exactly once.

## Risks / open questions

* **Coordinator as a bottleneck** — at high `P` with a small `chunk_size`, request
  traffic to rank 0 could dominate. Mitigation: larger chunks, or (later version)
  a hierarchy of coordinators. The `--chunk` sweep in the benchmark is meant to
  expose this.
* **Stragglers at the tail** — the last few chunks can still leave one worker
  running alone. A shrinking `chunk_size` near the end of the keyspace (guided
  self-scheduling) is a candidate refinement, deferred to keep v0.2 minimal.
* **Rank 0 double duty** — coordinator work vs. searching. Start simple (search one
  chunk per drain cycle); revisit if profiling shows rank 0 starved either way.

## Plan of record

1. `include/strategies.h` — declare `strategy_dynamic`.
2. `src/strategies/dynamic.c` — implement the two loops above against the existing
   search engine.
3. `src/main.c` — one line in the dispatch, accept `--strategy dynamic` and
   `--chunk <n>`.
4. Tests — coverage/no-overlap unit test, plus the existing multi-process
   integration test extended to the new strategy.
5. Benchmark — rerun the Lotus quick sweep with `dynamic` alongside the other two,
   add a `--chunk` sweep, update `docs/PERFORMANCE.md`.
