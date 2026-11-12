#include "strategies.h"

#include <limits.h>
#include <string.h>

/*
 * Collective synchronization strategy.
 *
 * Every rank repeats the same two-phase loop:
 *
 *   1. Compute phase:  search one batch of its own cyclic slice of the keyspace.
 *   2. Coordinate phase: all ranks meet at MPI_Allreduce and agree on two global
 *                        facts -- the smallest matching index found so far, and
 *                        whether any rank still has work left.
 *
 * Why Allreduce? Early termination needs every rank to learn, at the same point,
 * that the answer has been found so they can all stop. A collective call is the
 * natural fit: it is a single synchronization point that both combines the ranks'
 * local state and hands the combined result back to everyone. Because every rank
 * executes exactly the same number of Allreduce calls in the same order, the
 * collectives always stay matched and the strategy cannot deadlock.
 */

int strategy_collective(const search_config *config, MPI_Comm comm,
                        search_result *out)
{
    int rank;
    int size;
    MPI_Comm_rank(comm, &rank);
    MPI_Comm_size(comm, &size);

    /* Static cyclic distribution: rank r owns indices r, r+size, r+2*size, ...
       This hands adjacent candidates to different ranks, which spreads the work
       evenly even when the answer sits in one region of the keyspace. */
    long next_index      = rank;
    long stride          = size;
    long local_evaluated = 0;
    long local_match     = -1;   /* smallest index THIS rank matched, or -1 */

    long   rounds       = 0;
    double comm_seconds = 0.0;

    /* Start every rank's clock together so the measured wall time is comparable. */
    MPI_Barrier(comm);
    double start_time = MPI_Wtime();

    int  done               = 0;
    long global_match_index = -1;
    int  global_match_rank  = -1;

    while (done == 0) {
        /* ---- compute phase ---- */
        /* Only search if we have not already matched and still have candidates. */
        if (local_match < 0 && next_index < config->keyspace) {
            long evaluated = 0;
            long resume    = next_index;
            long match = search_batch(config, next_index, stride,
                                      config->batch_size, &evaluated, &resume);
            local_evaluated += evaluated;
            next_index = resume;
            if (match >= 0) {
                local_match = match;
            }
        }

        /* Are we still active: no local match yet and candidates remain? */
        int local_active;
        if (local_match < 0 && next_index < config->keyspace) {
            local_active = 1;
        } else {
            local_active = 0;
        }

        /* ---- coordinate phase ---- */
        /* Reduction 1: the smallest matching (index, rank) across all ranks.
           MPI_MINLOC on an {long, int} pair picks the lowest index and, on a tie,
           the lowest rank. Ranks with no match contribute LONG_MAX so they can
           never win. There is only one target digest, so in practice exactly one
           index ever matches; MINLOC simply locates it and names its owner. */
        struct {
            long index;
            int  rank;
        } local_pair;
        struct {
            long index;
            int  rank;
        } global_pair;

        if (local_match >= 0) {
            local_pair.index = local_match;
        } else {
            local_pair.index = LONG_MAX;
        }
        local_pair.rank = rank;

        double coord_start = MPI_Wtime();
        MPI_Allreduce(&local_pair, &global_pair, 1, MPI_LONG_INT, MPI_MINLOC, comm);

        /* Reduction 2: does ANY rank still have work? Summing the active flags
           tells every rank whether to keep going or stop. */
        int global_active = 0;
        MPI_Allreduce(&local_active, &global_active, 1, MPI_INT, MPI_SUM, comm);
        comm_seconds += MPI_Wtime() - coord_start;

        rounds++;

        if (global_pair.index != LONG_MAX) {
            /* A rank found the target; every rank now agrees on the same winner. */
            global_match_index = global_pair.index;
            global_match_rank  = global_pair.rank;
            done = 1;
        } else if (global_active == 0) {
            /* No match and no rank has work left: the keyspace is exhausted. */
            done = 1;
        } else {
            /* At least one rank still has candidates to try; go around again. */
            done = 0;
        }
    }

    double end_time = MPI_Wtime();

    /* ---- gather the metrics onto rank 0 ---- */
    long global_evaluated = 0;
    MPI_Reduce(&local_evaluated, &global_evaluated, 1, MPI_LONG, MPI_SUM, 0, comm);

    /* The largest per-rank coordination time is the communication overhead: it
       includes the time ranks spend waiting at Allreduce for the slowest rank. */
    double max_comm = 0.0;
    MPI_Reduce(&comm_seconds, &max_comm, 1, MPI_DOUBLE, MPI_MAX, 0, comm);

    memset(out, 0, sizeof(*out));
    out->found       = (global_match_index >= 0) ? 1 : 0;
    out->found_index = global_match_index;
    out->finder_rank = global_match_rank;
    out->rounds      = rounds;

    if (rank == 0) {
        out->candidates_evaluated = global_evaluated;
        out->wall_seconds         = end_time - start_time;
        out->comm_seconds         = max_comm;
        if (out->found == 1) {
            /* The index-to-candidate mapping is deterministic, so rank 0 simply
               regenerates the password from the agreed index -- no extra message
               is needed to carry the string back. */
            search_index_to_candidate(config, global_match_index, out->password);
        } else {
            out->password[0] = '\0';
        }
    }

    return 0;
}
