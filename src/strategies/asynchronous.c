#include "strategies.h"

#include <stdlib.h>
#include <string.h>

/*
 * Asynchronous coordination strategy.
 *
 * Instead of meeting at a collective call between batches, the ranks coordinate
 * with point-to-point messages, and rank 0 acts as the coordinator while also
 * doing real search work.
 *
 * The message protocol is deliberately simple and balanced so it can never
 * deadlock:
 *
 *   - Each worker sends EXACTLY ONE completion message to rank 0: either
 *     TAG_FOUND (with the matching index) if it found the target, or TAG_DONE if
 *     it finished its slice or was told to stop without finding anything.
 *   - Rank 0 sends EXACTLY ONE TAG_STOP message to each worker, once it knows the
 *     search is over (either because the target was found or because everyone is
 *     done).
 *   - Each worker consumes its one TAG_STOP either via MPI_Test while it is still
 *     searching (which lets it stop early) or via MPI_Wait after it has finished.
 *
 * Because the number of messages in each direction is fixed and known
 * (size-1 completions in, size-1 stops out), rank 0 can always drain every
 * message, no request is ever left dangling, and no rank waits forever.
 */

/* Message tags. Named constants make the intent of each message obvious. */
enum {
    TAG_FOUND = 1,  /* worker -> rank 0: "I found the target at this index" */
    TAG_DONE  = 2,  /* worker -> rank 0: "I stopped without finding it"      */
    TAG_STOP  = 3   /* rank 0 -> worker: "stop searching now"                */
};

/*
 * How many batches a worker searches between checks for a stop message is
 * configurable per run via config->poll_interval (the --poll option). A small
 * interval reacts to a stop quickly (low early-termination latency) but checks
 * more often; a larger interval spends less time checking but reacts more slowly.
 * This is exactly the knob the roadmap's "adaptive polling" work will tune.
 */

/*
 * Worker side (every rank except rank 0).
 * Searches its cyclic slice, reports its single completion message, and consumes
 * its single stop message. Returns the number of candidates it evaluated via
 * *evaluated and the time it spent in MPI calls via *comm_seconds.
 */
static void run_worker(const search_config *config, MPI_Comm comm, int rank,
                       int size, long *evaluated, double *comm_seconds)
{
    long next_index      = rank;   /* this worker's first index */
    long stride          = size;
    long local_evaluated = 0;
    double comm_time      = 0.0;

    int  found_local = 0;
    long found_index = -1;
    int  stopped     = 0;

    /* Post a non-blocking receive for the stop message up front. Posting it early
       means rank 0's stop can be delivered the moment it is sent, and we can check
       for it cheaply with MPI_Test between batches without blocking. */
    int stop_value = 0;
    MPI_Request stop_request;
    double t0 = MPI_Wtime();
    MPI_Irecv(&stop_value, 1, MPI_INT, 0, TAG_STOP, comm, &stop_request);
    comm_time += MPI_Wtime() - t0;

    /* How many batches between stop checks; always at least 1. */
    int poll_every = (config->poll_interval > 0) ? config->poll_interval : 1;
    int batches_since_check = 0;

    while (next_index < config->keyspace && found_local == 0 && stopped == 0) {
        long batch_evaluated = 0;
        long resume          = next_index;
        long match = search_batch(config, next_index, stride, config->batch_size,
                                  &batch_evaluated, &resume);
        local_evaluated += batch_evaluated;
        next_index = resume;

        if (match >= 0) {
            found_local = 1;
            found_index = match;
        } else {
            /* Periodically check whether rank 0 has told us to stop. MPI_Test is
               non-blocking: it reports whether the stop message has arrived yet
               without waiting, so a worker never stalls here. */
            batches_since_check++;
            if (batches_since_check >= poll_every) {
                batches_since_check = 0;
                int stop_ready = 0;
                double tt = MPI_Wtime();
                MPI_Test(&stop_request, &stop_ready, MPI_STATUS_IGNORE);
                comm_time += MPI_Wtime() - tt;
                if (stop_ready != 0) {
                    stopped = 1;
                }
            }
        }
    }

    /* Send exactly one completion message to rank 0. */
    double ts = MPI_Wtime();
    if (found_local == 1) {
        MPI_Send(&found_index, 1, MPI_LONG, 0, TAG_FOUND, comm);
    } else {
        long done_value = 0;
        MPI_Send(&done_value, 1, MPI_LONG, 0, TAG_DONE, comm);
    }
    comm_time += MPI_Wtime() - ts;

    /* Make sure our single stop message is consumed exactly once. If we already
       saw it via MPI_Test (stopped == 1), the request is complete; otherwise rank
       0 will send a stop to every worker at the end, and we wait for ours here. */
    if (stopped == 0) {
        double tw = MPI_Wtime();
        MPI_Wait(&stop_request, MPI_STATUS_IGNORE);
        comm_time += MPI_Wtime() - tw;
    }

    *evaluated     = local_evaluated;
    *comm_seconds  = comm_time;
}

/*
 * Coordinator side (rank 0).
 * Searches its own slice while polling for worker messages, then tells every
 * worker to stop and drains their completion messages. Fills in the global result
 * fields it can determine locally.
 */
static void run_coordinator(const search_config *config, MPI_Comm comm, int size,
                            long *evaluated, double *comm_seconds, long *rounds,
                            int *found_out, long *found_index_out, int *finder_out)
{
    long next_index      = 0;
    long stride          = size;
    long local_evaluated = 0;
    double comm_time      = 0.0;
    long batch_count      = 0;

    int  num_workers          = size - 1;
    int  completions_received = 0;

    int  found       = 0;
    long found_index = -1;
    int  finder_rank = -1;

    while (1) {
        if (found == 1) {
            break;
        }

        int local_has_work = (next_index < config->keyspace) ? 1 : 0;

        /* If we have nothing left to search and every worker has reported, the
           search is over (no match). */
        if (local_has_work == 0 && completions_received == num_workers) {
            break;
        }

        if (local_has_work == 1) {
            /* Do one batch of our own work. */
            long batch_evaluated = 0;
            long resume          = next_index;
            long match = search_batch(config, next_index, stride,
                                      config->batch_size, &batch_evaluated, &resume);
            local_evaluated += batch_evaluated;
            next_index = resume;
            batch_count++;
            if (match >= 0) {
                found       = 1;
                found_index = match;
                finder_rank = 0;
            }

            /* Non-blocking drain: pick up any worker messages that have arrived,
               but do not wait for them -- we still have work to do. */
            int message_ready = 1;
            while (message_ready != 0 && completions_received < num_workers) {
                MPI_Status status;
                double tp = MPI_Wtime();
                MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, comm, &message_ready, &status);
                if (message_ready != 0) {
                    long buffer = 0;
                    MPI_Recv(&buffer, 1, MPI_LONG, status.MPI_SOURCE, status.MPI_TAG,
                             comm, MPI_STATUS_IGNORE);
                    completions_received++;
                    if (status.MPI_TAG == TAG_FOUND && found == 0) {
                        found       = 1;
                        found_index = buffer;
                        finder_rank = status.MPI_SOURCE;
                    }
                }
                comm_time += MPI_Wtime() - tp;
            }
        } else {
            /* No local work left but workers are still running. Block for the next
               worker message instead of busy-waiting: we have nothing else to do. */
            MPI_Status status;
            double tp = MPI_Wtime();
            MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, comm, &status);
            long buffer = 0;
            MPI_Recv(&buffer, 1, MPI_LONG, status.MPI_SOURCE, status.MPI_TAG,
                     comm, MPI_STATUS_IGNORE);
            comm_time += MPI_Wtime() - tp;
            completions_received++;
            if (status.MPI_TAG == TAG_FOUND && found == 0) {
                found       = 1;
                found_index = buffer;
                finder_rank = status.MPI_SOURCE;
            }
        }
    }

    /* Tell every worker to stop, exactly once each. These are small non-blocking
       sends; each matches the stop receive the worker posted at startup. */
    if (num_workers > 0) {
        MPI_Request *stop_requests = malloc((size_t)num_workers * sizeof(MPI_Request));
        int stop_value = 1;
        double tsend = MPI_Wtime();
        for (int worker = 1; worker < size; worker++) {
            MPI_Isend(&stop_value, 1, MPI_INT, worker, TAG_STOP, comm,
                      &stop_requests[worker - 1]);
        }
        MPI_Waitall(num_workers, stop_requests, MPI_STATUSES_IGNORE);
        comm_time += MPI_Wtime() - tsend;
        free(stop_requests);

        /* Drain the remaining completion messages. We know exactly how many are
           still outstanding, so these blocking receives always terminate. A late
           TAG_FOUND still counts if we had not recorded a match yet. */
        while (completions_received < num_workers) {
            MPI_Status status;
            long buffer = 0;
            double td = MPI_Wtime();
            MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, comm, &status);
            MPI_Recv(&buffer, 1, MPI_LONG, status.MPI_SOURCE, status.MPI_TAG,
                     comm, MPI_STATUS_IGNORE);
            comm_time += MPI_Wtime() - td;
            completions_received++;
            if (status.MPI_TAG == TAG_FOUND && found == 0) {
                found       = 1;
                found_index = buffer;
                finder_rank = status.MPI_SOURCE;
            }
        }
    }

    *evaluated       = local_evaluated;
    *comm_seconds    = comm_time;
    *rounds          = batch_count;
    *found_out       = found;
    *found_index_out = found_index;
    *finder_out      = finder_rank;
}

int strategy_asynchronous(const search_config *config, MPI_Comm comm,
                          search_result *out)
{
    int rank;
    int size;
    MPI_Comm_rank(comm, &rank);
    MPI_Comm_size(comm, &size);

    long   local_evaluated = 0;
    double comm_seconds    = 0.0;
    long   rounds          = 0;

    int  found       = 0;
    long found_index = -1;
    int  finder_rank = -1;

    /* Start the clock together so the wall time is comparable across runs. */
    MPI_Barrier(comm);
    double start_time = MPI_Wtime();

    if (rank == 0) {
        run_coordinator(config, comm, size, &local_evaluated, &comm_seconds,
                        &rounds, &found, &found_index, &finder_rank);
    } else {
        run_worker(config, comm, rank, size, &local_evaluated, &comm_seconds);
    }

    double end_time = MPI_Wtime();

    /* Share the outcome so every rank's result struct agrees (matches how the
       collective strategy behaves). Only rank 0 determined it. */
    long outcome[2];
    if (rank == 0) {
        outcome[0] = (long)found_index;
        outcome[1] = (long)finder_rank;
    }
    MPI_Bcast(outcome, 2, MPI_LONG, 0, comm);
    found_index = outcome[0];
    finder_rank = (int)outcome[1];
    found = (found_index >= 0) ? 1 : 0;

    /* Collect the metrics on rank 0. */
    long global_evaluated = 0;
    MPI_Reduce(&local_evaluated, &global_evaluated, 1, MPI_LONG, MPI_SUM, 0, comm);

    double max_comm = 0.0;
    MPI_Reduce(&comm_seconds, &max_comm, 1, MPI_DOUBLE, MPI_MAX, 0, comm);

    memset(out, 0, sizeof(*out));
    out->found       = found;
    out->found_index = found_index;
    out->finder_rank = finder_rank;
    out->rounds      = rounds;

    if (rank == 0) {
        out->candidates_evaluated = global_evaluated;
        out->wall_seconds         = end_time - start_time;
        out->comm_seconds         = max_comm;
        if (found == 1) {
            search_index_to_candidate(config, found_index, out->password);
        } else {
            out->password[0] = '\0';
        }
    }

    return 0;
}
