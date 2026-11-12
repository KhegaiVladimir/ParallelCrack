#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "search.h"
#include "strategies.h"

/*
 * Command-line driver for ParallelCrack.
 *
 * Rank 0 parses the command line into a small plain-data "run_params" struct and
 * broadcasts it to every rank. Each rank then builds an identical search_config
 * from those parameters on its own. This is deliberate: it means we never have to
 * broadcast a pointer or a precomputed digest -- every rank derives the exact same
 * configuration (alphabet, keyspace, target hash) from the same inputs.
 */

/* How the target is chosen. */
typedef enum {
    TARGET_INDEX = 0,    /* target is the candidate at a specific index */
    TARGET_PASSWORD = 1, /* target is an explicit password string */
    TARGET_ABSENT = 2    /* target is guaranteed not to exist (no-match test) */
} target_mode;

typedef struct {
    int         strategy;        /* 0 = collective, 1 = asynchronous */
    int         alphabet;        /* an alphabet_id */
    int         length;          /* candidate length */
    long        batch;           /* batch size */
    target_mode mode;            /* how the target was specified */
    long        target_index;    /* used when mode == TARGET_INDEX (-1 = default) */
    char        target_password[SEARCH_MAX_LENGTH + 1]; /* used when mode == TARGET_PASSWORD */
    int         quiet;           /* suppress the human-readable banner */
    int         ok;              /* 0 if parsing failed and we should exit */
} run_params;

static const char *strategy_name(int strategy)
{
    if (strategy == 1) {
        return "async";
    } else {
        return "collective";
    }
}

static const char *alphabet_name(int alphabet)
{
    if (alphabet == ALPHABET_ALNUM) {
        return "alnum";
    } else if (alphabet == ALPHABET_PRINTABLE) {
        return "printable";
    } else {
        return "lower";
    }
}

static void print_usage(const char *program)
{
    printf("Usage: mpirun -np <N> %s [options]\n", program);
    printf("\n");
    printf("Options:\n");
    printf("  --strategy <collective|async>  Coordination strategy (default collective)\n");
    printf("  --alphabet <lower|alnum|printable>  Candidate alphabet (default lower)\n");
    printf("  --length <N>        Candidate length (default 4)\n");
    printf("  --batch <N>         Candidates per batch between coordination (default 2000)\n");
    printf("  --index <K>         Target is the candidate at index K (reproducible)\n");
    printf("  --password <STR>    Target is this explicit password\n");
    printf("  --no-match          Use a target no candidate can match (no-match test)\n");
    printf("  --full-sweep        Put the target at the last index (worst-case sweep)\n");
    printf("  --quiet             Only print the one-line metrics summary\n");
    printf("  --help              Show this help and exit\n");
}

/*
 * Parse argv on rank 0 into run_params. Returns a filled struct; params.ok is 0
 * if the user asked for help or passed something invalid. "full_sweep" is handled
 * after we know the keyspace, so here we just remember it as a special index value.
 */
static run_params parse_args(int argc, char **argv)
{
    run_params p;
    p.strategy      = 0;
    p.alphabet      = ALPHABET_LOWER;
    p.length        = 4;
    p.batch         = 2000;
    p.mode          = TARGET_INDEX;
    p.target_index  = -1;   /* -1 means "pick a sensible default once keyspace is known" */
    p.target_password[0] = '\0';
    p.quiet         = 0;
    p.ok            = 1;

    int want_full_sweep = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            p.ok = 0;
            return p;
        } else if (strcmp(argv[i], "--strategy") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "async") == 0 || strcmp(argv[i], "asynchronous") == 0) {
                p.strategy = 1;
            } else if (strcmp(argv[i], "collective") == 0) {
                p.strategy = 0;
            } else {
                fprintf(stderr, "Unknown strategy: %s\n", argv[i]);
                p.ok = 0;
                return p;
            }
        } else if (strcmp(argv[i], "--alphabet") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "lower") == 0) {
                p.alphabet = ALPHABET_LOWER;
            } else if (strcmp(argv[i], "alnum") == 0) {
                p.alphabet = ALPHABET_ALNUM;
            } else if (strcmp(argv[i], "printable") == 0) {
                p.alphabet = ALPHABET_PRINTABLE;
            } else {
                fprintf(stderr, "Unknown alphabet: %s\n", argv[i]);
                p.ok = 0;
                return p;
            }
        } else if (strcmp(argv[i], "--length") == 0 && i + 1 < argc) {
            i++;
            p.length = atoi(argv[i]);
        } else if (strcmp(argv[i], "--batch") == 0 && i + 1 < argc) {
            i++;
            p.batch = atol(argv[i]);
        } else if (strcmp(argv[i], "--index") == 0 && i + 1 < argc) {
            i++;
            p.mode = TARGET_INDEX;
            p.target_index = atol(argv[i]);
        } else if (strcmp(argv[i], "--password") == 0 && i + 1 < argc) {
            i++;
            p.mode = TARGET_PASSWORD;
            strncpy(p.target_password, argv[i], SEARCH_MAX_LENGTH);
            p.target_password[SEARCH_MAX_LENGTH] = '\0';
        } else if (strcmp(argv[i], "--no-match") == 0) {
            p.mode = TARGET_ABSENT;
        } else if (strcmp(argv[i], "--full-sweep") == 0) {
            want_full_sweep = 1;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            p.quiet = 1;
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
            p.ok = 0;
            return p;
        }
    }

    /* Full sweep means "target at the very last index", so the search has to scan
       the entire keyspace. We encode it as index -2 and resolve it below once the
       keyspace is known. */
    if (want_full_sweep == 1) {
        p.mode = TARGET_INDEX;
        p.target_index = -2;
    }

    return p;
}

/* Build an identical search_config on any rank from the broadcast parameters.
   Returns 0 on success, -1 on an invalid combination. */
static int build_config(const run_params *p, search_config *config)
{
    if (search_config_init(config, p->alphabet, p->length, p->batch) != 0) {
        return -1;
    }

    if (p->mode == TARGET_ABSENT) {
        search_set_target_absent(config);
    } else if (p->mode == TARGET_PASSWORD) {
        if (search_set_target_password(config, p->target_password) != 0) {
            return -1;
        }
    } else {
        /* TARGET_INDEX, possibly with a sentinel to resolve. */
        long index = p->target_index;
        if (index == -1) {
            index = config->keyspace / 2;   /* default: middle of the keyspace */
        } else if (index == -2) {
            index = config->keyspace - 1;   /* full sweep: the last candidate */
        }
        if (index < 0 || index >= config->keyspace) {
            return -1;
        }
        search_set_target_index(config, index);
    }

    return 0;
}

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);

    int rank;
    int size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    /* Rank 0 owns the command line; everyone else receives the parsed parameters. */
    run_params params;
    if (rank == 0) {
        params = parse_args(argc, argv);
    }
    MPI_Bcast(&params, sizeof(params), MPI_BYTE, 0, MPI_COMM_WORLD);

    if (params.ok == 0) {
        /* Help was shown or parsing failed on rank 0; all ranks exit cleanly. */
        MPI_Finalize();
        return (rank == 0) ? 0 : 0;
    }

    search_config config;
    if (build_config(&params, &config) != 0) {
        if (rank == 0) {
            fprintf(stderr, "Invalid configuration (length/batch/target out of range).\n");
        }
        MPI_Finalize();
        return 1;
    }

    /* Pick the strategy. The asynchronous strategy is wired in once it exists. */
    strategy_fn strategy;
    if (params.strategy == 1) {
        if (rank == 0) {
            fprintf(stderr, "The 'async' strategy is not available yet; "
                            "use --strategy collective.\n");
        }
        MPI_Finalize();
        return 1;
    } else {
        strategy = strategy_collective;
    }

    search_result result;
    strategy(&config, MPI_COMM_WORLD, &result);

    /* Only rank 0 reports. */
    if (rank == 0) {
        double hashes_per_second = 0.0;
        if (result.wall_seconds > 0.0) {
            hashes_per_second = (double)result.candidates_evaluated / result.wall_seconds;
        }

        if (params.quiet == 0) {
            printf("ParallelCrack run\n");
            printf("  strategy      : %s\n", strategy_name(params.strategy));
            printf("  processes     : %d\n", size);
            printf("  alphabet      : %s (%d symbols)\n",
                   alphabet_name(params.alphabet), config.symbol_count);
            printf("  length        : %d\n", config.length);
            printf("  keyspace      : %ld\n", config.keyspace);
            printf("  batch size    : %ld\n", config.batch_size);
            if (result.found == 1) {
                printf("  result        : FOUND\n");
                printf("  password      : \"%s\"\n", result.password);
                printf("  found index   : %ld\n", result.found_index);
                printf("  finder rank   : %d\n", result.finder_rank);
            } else {
                printf("  result        : not found (keyspace exhausted)\n");
            }
            printf("  candidates    : %ld\n", result.candidates_evaluated);
            printf("  wall seconds  : %.6f\n", result.wall_seconds);
            printf("  hashes/sec    : %.0f\n", hashes_per_second);
            printf("  comm seconds  : %.6f\n", result.comm_seconds);
            printf("  rounds        : %ld\n", result.rounds);
        } else {
            printf("strategy=%s nprocs=%d length=%d alphabet_size=%d batch=%ld "
                   "found=%d finder_rank=%d candidates=%ld wall_seconds=%.6f "
                   "hashes_per_second=%.0f comm_seconds=%.6f\n",
                   strategy_name(params.strategy), size, config.length,
                   config.symbol_count, config.batch_size, result.found,
                   result.finder_rank, result.candidates_evaluated,
                   result.wall_seconds, hashes_per_second, result.comm_seconds);
        }
    }

    MPI_Finalize();
    return 0;
}
