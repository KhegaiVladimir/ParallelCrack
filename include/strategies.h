#ifndef PARALLELCRACK_STRATEGIES_H
#define PARALLELCRACK_STRATEGIES_H

#include <mpi.h>

#include "search.h"

/*
 * The common interface that every communication strategy implements.
 *
 * A strategy is handed a fully-built search_config and an MPI communicator. It
 * runs the search however it likes (collective, message-driven, ...) and fills in
 * a search_result. Because all strategies share this one signature and the shared
 * search engine, adding a new communication pattern is a new .c file plus one line
 * in the dispatch table in main.c -- the workload itself never changes.
 */

typedef struct {
    int    found;                            /* 1 if the target was found, else 0 */
    long   found_index;                      /* global index of the match, or -1 */
    int    finder_rank;                      /* rank that found it, or -1 */
    char   password[SEARCH_MAX_LENGTH + 1];  /* recovered password (valid on rank 0) */

    long   candidates_evaluated;             /* total candidates hashed, all ranks */
    double wall_seconds;                     /* measured search wall time (rank 0) */
    double comm_seconds;                     /* time in MPI coordination (max rank) */
    long   rounds;                           /* coordination rounds executed */
} search_result;

/*
 * Every strategy has this signature. "comm" is normally MPI_COMM_WORLD. The
 * function returns 0 on success. It fills *out on every rank; the password and
 * finder_rank are authoritative on rank 0, and the per-run metrics
 * (candidates_evaluated, wall_seconds, comm_seconds) are collected on rank 0.
 */
typedef int (*strategy_fn)(const search_config *config, MPI_Comm comm,
                           search_result *out);

/* Strategy A: collective synchronization via MPI_Allreduce. */
int strategy_collective(const search_config *config, MPI_Comm comm,
                        search_result *out);

/* Strategy B: asynchronous point-to-point coordination. */
int strategy_asynchronous(const search_config *config, MPI_Comm comm,
                          search_result *out);

#endif /* PARALLELCRACK_STRATEGIES_H */
