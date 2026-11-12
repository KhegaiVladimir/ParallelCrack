# MPI Communication Protocols

This document describes exactly how the two v0.1 strategies coordinate, including
the message flow, why each MPI call is used, and how each one avoids deadlock.

Both strategies solve the same three coordination problems:

1. **Split the work** — who searches which candidates. (Both: static cyclic.)
2. **Stop early** — when one rank finds the target, make everyone stop promptly.
3. **Detect "no match"** — agree that the whole keyspace was searched in vain.

They differ only in how they solve problems 2 and 3.

---

## Strategy A — Collective (`MPI_Allreduce`)

### Idea

Every rank runs the same loop: search one batch, then meet all the other ranks at
a collective call to agree on global state. There is no coordinator; all ranks are
symmetric.

### The loop

```
repeat:
    search one batch of my cyclic slice        (compute)
    Allreduce MINLOC -> smallest matching (index, rank)   (coordinate)
    Allreduce SUM    -> how many ranks still have work     (coordinate)
    if someone matched:        stop (everyone agrees on the winner)
    else if nobody has work:   stop (keyspace exhausted, no match)
    else:                      continue
```

### Message flow (3 ranks, target found by rank 1)

```
          batch 0         batch 1 (rank 1 finds)
rank 0 :  search ──┐      search ──┐
rank 1 :  search ──┼─Allreduce     search(match) ──┼─Allreduce   stop
rank 2 :  search ──┘      (none)   search ──────────┘ (winner=1)  stop
                   ^                                  ^
                   |                                  |
             all ranks meet here                all ranks learn the
             (no match yet)                     winner at the SAME point
```

### Why these calls

* **`MPI_Allreduce` with `MPI_MINLOC` on an `MPI_LONG_INT` pair** — combines every
  rank's best match into one global answer *and* hands it back to everyone in a
  single call. MINLOC returns the smallest index together with the rank that holds
  it, so rank 0 learns both the answer and who found it. Ranks with no match send
  `LONG_MAX` so they never win.
* **`MPI_Allreduce` with `MPI_SUM`** on a 0/1 "still active" flag — tells every
  rank whether any rank still has candidates left, which is how the no-match case
  terminates.
* **`MPI_Reduce`** at the end — collects the total candidate count and the maximum
  per-rank coordination time onto rank 0 for reporting.

### Why it cannot deadlock

Every rank executes the **same number of collective calls in the same order**. A
collective completes only when all ranks have called it, and since no rank can
exit the loop without the collectives telling all ranks to exit together, the
calls always stay matched. There is no point where one rank waits for a message
another rank never sends.

### Cost characteristic

Each batch pays for a full synchronization. The coordination time therefore
includes the time ranks spend *waiting* for the slowest rank at each Allreduce.
That shows up as higher `comm_seconds` and is the price of the simple, symmetric
design.

---

## Strategy B — Asynchronous (point-to-point, non-blocking)

### Idea

Rank 0 is a coordinator that **also does real search work**. Workers search their
slices and only send a message when they finish (found or exhausted). Rank 0
drives termination with messages instead of a global barrier, so a worker that is
busy computing is never forced to stop and synchronize.

### Message types (tags)

```
TAG_FOUND  worker -> rank 0 : "I found the target at index X"
TAG_DONE   worker -> rank 0 : "I stopped without finding it"
TAG_STOP   rank 0 -> worker : "stop searching now"
```

### The protocol, stated as invariants

* Each worker sends **exactly one** completion message (`TAG_FOUND` or `TAG_DONE`).
* Rank 0 sends **exactly one** `TAG_STOP` to **each** worker.
* Each worker consumes its one `TAG_STOP` either via `MPI_Test` while still
  searching (early stop) or via `MPI_Wait` after it has finished.

Because the message counts are fixed and known (`P-1` in each direction), rank 0
can always drain every message and no request is ever left dangling.

### Message flow (3 ranks, target found by worker 2)

```
rank 0 (coord+worker):  search ... Iprobe(no) ... search ...        receive FOUND(2)
                                                                     │
rank 1 (worker): Irecv(stop) search search ...  (Test: no stop yet) │
rank 2 (worker): Irecv(stop) search(MATCH) ─ Send FOUND ────────────┘
                                                             │
rank 0 then: Isend STOP to rank1, rank2 ; Waitall ; drain remaining DONE/FOUND
rank 1: ... Test sees STOP ─ breaks ─ Send DONE ─ (stop already consumed)
rank 2: already sent FOUND ─ Wait(stop) consumes its STOP
```

### Why these calls

* **`MPI_Irecv` (posted up front by each worker) for `TAG_STOP`** — posting the
  receive early means rank 0's stop can land immediately, and the worker can check
  for it cheaply between batches without blocking.
* **`MPI_Test`** — non-blocking check of that stop receive between batches. It lets
  a worker react to a stop promptly *without* stalling if no stop has arrived. This
  is what makes early termination fast.
* **`MPI_Iprobe` + `MPI_Recv`** on rank 0 — lets the coordinator pick up worker
  messages that have arrived *without* waiting, so it can keep searching its own
  slice while it coordinates.
* **`MPI_Probe` + `MPI_Recv`** on rank 0 when it has no local work left — there is
  nothing else to do, so it blocks for the next worker message instead of
  busy-waiting.
* **`MPI_Send`** (worker completion) — a worker has exactly one small message to
  send and nothing to do afterward except wait for the stop, so a blocking send is
  the clear choice.
* **`MPI_Isend` + `MPI_Waitall`** (rank 0's stops) — rank 0 fires a stop to every
  worker at once and waits for those sends to complete together.

### Why it cannot deadlock

* Rank 0 never blocks on a worker that will not send: it only ever blocks in
  `MPI_Probe` when it is *certain* a worker message is still outstanding
  (`completions_received < P-1`).
* A worker never blocks forever on the stop: rank 0 is guaranteed to send exactly
  one stop to every worker at the end, and the worker consumes exactly one.
* The send/receive counts are balanced by construction (`P-1` each way), so the
  final drain loop always terminates and no request leaks.

### Cost characteristic

Workers are not forced to synchronize every batch, so coordination overhead is
much lower than the collective strategy — but the coordinator does slightly less
search work because it also services messages. The interesting trade-off this
project studies is exactly how those two effects balance as `P` grows.

---

## Summary

| Concern              | Collective                         | Asynchronous                            |
|----------------------|------------------------------------|-----------------------------------------|
| Coordination point   | global barrier every batch         | messages only on completion / stop      |
| Who coordinates      | all ranks (symmetric)              | rank 0 (coordinator + worker)           |
| Early-stop mechanism | MINLOC result seen by all          | `TAG_STOP` via `MPI_Test`               |
| No-match detection   | SUM of active flags reaches 0      | all `P-1` workers report `TAG_DONE`     |
| Deadlock safety      | identical collective call sequence | balanced, fixed message counts          |
| Overhead growth      | synchronization wait per batch     | per-message, lower but coordinator-bound|
