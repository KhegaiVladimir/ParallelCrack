#ifndef PARALLELCRACK_BENCHMARK_H
#define PARALLELCRACK_BENCHMARK_H

/*
 * Benchmark output.
 *
 * A single run of the program produces one row of measurements. Collecting many
 * runs (across process counts and strategies) into a CSV file lets us compute
 * speedup and efficiency and plot them later. This module only writes the raw
 * measured values -- it never invents or derives experimental numbers.
 */

typedef struct {
    const char *strategy;             /* "collective" or "async" */
    int    nprocs;                    /* number of MPI processes */
    int    length;                    /* candidate length */
    int    alphabet_size;             /* symbols in the alphabet */
    long   batch_size;                /* candidates per batch */
    int    found;                     /* 1 if the target was found */
    int    finder_rank;               /* rank that found it, or -1 */
    long   candidates_evaluated;      /* total candidates hashed, all ranks */
    double wall_seconds;              /* measured search wall time */
    double hashes_per_second;         /* candidates_evaluated / wall_seconds */
    double comm_seconds;              /* time spent in MPI coordination */
    long   rounds;                    /* coordination rounds / batches */
} benchmark_record;

/*
 * Append one record as a CSV row. If the file does not yet exist (or is empty),
 * a header line is written first so the CSV is self-describing. Returns 0 on
 * success, -1 on failure.
 */
int benchmark_append_csv(const char *path, const benchmark_record *record);

#endif /* PARALLELCRACK_BENCHMARK_H */
