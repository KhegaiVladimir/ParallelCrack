#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "search.h"
#include "strategies.h"
#include "benchmark.h"

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
    int         poll;            /* async stop-check interval, in batches (>=1) */
    int         quiet;           /* suppress the human-readable banner */
    int         recover;         /* 1 if a file-recovery demo was requested */
    char        recover_path[256]; /* base path for the file-recovery demo */
    int         csv;             /* 1 if a CSV row should be appended */
    char        csv_path[256];   /* where to append the CSV row */
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
    printf("  --poll <N>          Async: batches between stop-message checks (default 1)\n");
    printf("  --index <K>         Target is the candidate at index K (reproducible)\n");
    printf("  --password <STR>    Target is this explicit password\n");
    printf("  --no-match          Use a target no candidate can match (no-match test)\n");
    printf("  --full-sweep        Exhaustively scan the whole keyspace (unmatchable\n");
    printf("                      target; fixed deterministic work, no early stop)\n");
    printf("  --recover-file <P>  After finding the password, encrypt a synthetic\n");
    printf("                      file and recover it to prove the password is correct\n");
    printf("                      (writes P.plain, P.enc, P.recovered)\n");
    printf("  --csv <PATH>        Append one measured row to a CSV file (for plotting)\n");
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
    p.poll          = 1;
    p.quiet         = 0;
    p.recover       = 0;
    p.recover_path[0] = '\0';
    p.csv           = 0;
    p.csv_path[0]   = '\0';
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
        } else if (strcmp(argv[i], "--poll") == 0 && i + 1 < argc) {
            i++;
            p.poll = atoi(argv[i]);
            if (p.poll < 1) {
                p.poll = 1;
            }
        } else if (strcmp(argv[i], "--index") == 0 && i + 1 < argc) {
            i++;
            p.mode = TARGET_INDEX;
            p.target_index = atol(argv[i]);
        } else if (strcmp(argv[i], "--password") == 0 && i + 1 < argc) {
            i++;
            p.mode = TARGET_PASSWORD;
            /* snprintf null-terminates and bounds the write to the buffer size.
               A password longer than the configured length is rejected later by
               search_set_target_password, so bounding here is safe. */
            snprintf(p.target_password, sizeof(p.target_password), "%s", argv[i]);
        } else if (strcmp(argv[i], "--no-match") == 0) {
            p.mode = TARGET_ABSENT;
        } else if (strcmp(argv[i], "--full-sweep") == 0) {
            want_full_sweep = 1;
        } else if (strcmp(argv[i], "--recover-file") == 0 && i + 1 < argc) {
            i++;
            p.recover = 1;
            snprintf(p.recover_path, sizeof(p.recover_path), "%s", argv[i]);
        } else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc) {
            i++;
            p.csv = 1;
            snprintf(p.csv_path, sizeof(p.csv_path), "%s", argv[i]);
        } else if (strcmp(argv[i], "--quiet") == 0) {
            p.quiet = 1;
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
            p.ok = 0;
            return p;
        }
    }

    /* Full sweep means "scan the entire keyspace". We encode it with the
       sentinel index -2, which build_config turns into an unmatchable target so
       every strategy evaluates the whole keyspace deterministically (see the
       explanation in build_config). */
    if (want_full_sweep == 1) {
        p.mode = TARGET_INDEX;
        p.target_index = -2;
    }

    return p;
}

/* Resolve the target index for TARGET_INDEX mode, turning the two sentinels
   (-1 = default middle, -2 = full-sweep last) into a concrete index. */
static long resolve_target_index(const run_params *p, long keyspace)
{
    long index = p->target_index;
    if (index == -1) {
        index = keyspace / 2;       /* default: middle of the keyspace */
    } else if (index == -2) {
        index = keyspace - 1;       /* full sweep: the last candidate */
    }
    return index;
}

/* Build an identical search_config on any rank from the broadcast parameters.
   Returns 0 on success, -1 on an invalid combination. */
static int build_config(const run_params *p, search_config *config)
{
    if (search_config_init(config, p->alphabet, p->length, p->batch) != 0) {
        return -1;
    }

    /* Carry the async polling interval through to the strategy. */
    config->poll_interval = (p->poll >= 1) ? p->poll : 1;

    if (p->mode == TARGET_ABSENT) {
        search_set_target_absent(config);
    } else if (p->mode == TARGET_PASSWORD) {
        if (search_set_target_password(config, p->target_password) != 0) {
            return -1;
        }
    } else if (p->target_index == -2) {
        /* Full sweep: scan the ENTIRE keyspace. We use a target that no candidate
           can match, so every strategy evaluates exactly 'keyspace' candidates
           and no rank can terminate early. This gives a fixed, deterministic
           global workload -- the right basis for a strong-scaling comparison.
           A findable target here would let the asynchronous strategy signal stop
           and leave other ranks a few batches short, producing a timing-dependent
           undercount that is not a valid fixed-work measurement. */
        search_set_target_absent(config);
    } else {
        long index = resolve_target_index(p, config->keyspace);
        if (index < 0 || index >= config->keyspace) {
            return -1;
        }
        search_set_target_index(config, index);
    }

    return 0;
}

/*
 * Work out the secret password string that the target represents. Rank 0 uses
 * this to set up the file-recovery demo: it is the password that "someone"
 * encrypted the file with, and the search then independently has to find it.
 * Writes an empty string for the no-match target (nothing to recover).
 */
static void resolve_secret_password(const run_params *p, const search_config *config,
                                    char *secret_out)
{
    if (p->mode == TARGET_ABSENT) {
        secret_out[0] = '\0';
    } else if (p->mode == TARGET_PASSWORD) {
        /* snprintf always null-terminates and never writes past the buffer. The
           caller guarantees secret_out holds SEARCH_MAX_LENGTH + 1 bytes, which
           is exactly enough for a maximum-length password plus its terminator, so
           there is no truncation. */
        snprintf(secret_out, (size_t)SEARCH_MAX_LENGTH + 1, "%s", p->target_password);
    } else {
        long index = resolve_target_index(p, config->keyspace);
        search_index_to_candidate(config, index, secret_out);
    }
}

/*
 * The file-recovery demo. Runs on rank 0 only, entirely outside the search
 * timing. It:
 *   1. generates a small synthetic plaintext and writes it to <base>.plain,
 *   2. encrypts it to <base>.enc using the key derived from the SECRET password
 *      (this models an already-encrypted file that existed before the search),
 *   3. decrypts <base>.enc to <base>.recovered using the password the search
 *      FOUND, and
 *   4. compares the recovered bytes against the original.
 * Returns 1 if the recovered file matches the original, 0 otherwise.
 */
static int run_file_recovery(const char *secret, const char *found_password,
                             const char *base_path)
{
    char plain_path[300];
    char enc_path[300];
    char recovered_path[300];
    snprintf(plain_path, sizeof(plain_path), "%s.plain", base_path);
    snprintf(enc_path, sizeof(enc_path), "%s.enc", base_path);
    snprintf(recovered_path, sizeof(recovered_path), "%s.recovered", base_path);

    /* A fixed IV keeps the demo deterministic. A fixed IV is NOT appropriate for
       real encryption, but this is a self-contained correctness demo, not a
       security tool, so it is fine here. */
    static const unsigned char demo_iv[CRYPTO_AES_IV_LEN] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
    };

    /* Build a small, deterministic synthetic plaintext. Nothing here is real or
       sensitive: it is just bytes to prove the round trip works. */
    unsigned char plaintext[1024];
    for (int i = 0; i < (int)sizeof(plaintext); i++) {
        plaintext[i] = (unsigned char)('A' + (i % 26));
    }

    FILE *plain_file = fopen(plain_path, "wb");
    if (plain_file == NULL) {
        return 0;
    }
    fwrite(plaintext, 1, sizeof(plaintext), plain_file);
    fclose(plain_file);

    /* Step 2: encrypt with the secret password's key. */
    unsigned char secret_key[CRYPTO_AES_KEY_LEN];
    crypto_key_from_password(secret, secret_key);
    if (crypto_encrypt_file(secret_key, demo_iv, plain_path, enc_path) != 0) {
        return 0;
    }

    /* Step 3: decrypt with the FOUND password's key. If the search found the
       right password, this key equals the secret key and decryption succeeds. */
    unsigned char found_key[CRYPTO_AES_KEY_LEN];
    crypto_key_from_password(found_password, found_key);
    if (crypto_decrypt_file(found_key, demo_iv, enc_path, recovered_path) != 0) {
        return 0;
    }

    /* Step 4: compare recovered bytes against the original plaintext. */
    FILE *recovered_file = fopen(recovered_path, "rb");
    if (recovered_file == NULL) {
        return 0;
    }
    unsigned char recovered[1024];
    size_t recovered_len = fread(recovered, 1, sizeof(recovered), recovered_file);
    fclose(recovered_file);

    if (recovered_len != sizeof(plaintext)) {
        return 0;
    }
    if (memcmp(recovered, plaintext, sizeof(plaintext)) != 0) {
        return 0;
    }
    return 1;
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

    /* Pick the strategy. Adding a strategy is a one-line change here plus a new
       file under src/strategies/ -- the shared search engine is untouched. */
    strategy_fn strategy;
    if (params.strategy == 1) {
        strategy = strategy_asynchronous;
    } else {
        strategy = strategy_collective;
    }

    search_result result;
    strategy(&config, MPI_COMM_WORLD, &result);

    /* Optional file-recovery demo: rank 0 only, and entirely outside the search
       timing above (the search time is measured inside the strategy). We report
       whether the found password actually recovers a file encrypted with the
       secret. recovery_state: -1 = not requested, 0 = failed, 1 = verified. */
    int recovery_state = -1;
    if (rank == 0 && params.recover == 1) {
        if (result.found == 1) {
            char secret[SEARCH_MAX_LENGTH + 1];
            resolve_secret_password(&params, &config, secret);
            if (secret[0] != '\0') {
                recovery_state = run_file_recovery(secret, result.password,
                                                   params.recover_path);
            } else {
                recovery_state = 0;
            }
        } else {
            recovery_state = 0; /* nothing was found, so nothing can be recovered */
        }
    }

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
            if (recovery_state == 1) {
                printf("  file recovery : VERIFIED (recovered bytes match original)\n");
            } else if (recovery_state == 0) {
                printf("  file recovery : FAILED\n");
            }
        } else {
            printf("strategy=%s nprocs=%d length=%d alphabet_size=%d batch=%ld "
                   "found=%d found_index=%ld finder_rank=%d candidates=%ld "
                   "wall_seconds=%.6f hashes_per_second=%.0f comm_seconds=%.6f "
                   "recovery=%d\n",
                   strategy_name(params.strategy), size, config.length,
                   config.symbol_count, config.batch_size, result.found,
                   result.found_index, result.finder_rank,
                   result.candidates_evaluated, result.wall_seconds,
                   hashes_per_second, result.comm_seconds, recovery_state);
        }

        /* Append one measured row to the CSV, if requested. This is the only
           place benchmark data is produced, and it is always real measurement. */
        if (params.csv == 1) {
            benchmark_record record;
            record.strategy             = strategy_name(params.strategy);
            record.nprocs               = size;
            record.length               = config.length;
            record.alphabet_size        = config.symbol_count;
            record.batch_size           = config.batch_size;
            record.found                = result.found;
            record.finder_rank          = result.finder_rank;
            record.candidates_evaluated = result.candidates_evaluated;
            record.wall_seconds         = result.wall_seconds;
            record.hashes_per_second    = hashes_per_second;
            record.comm_seconds         = result.comm_seconds;
            record.rounds               = result.rounds;

            if (benchmark_append_csv(params.csv_path, &record) != 0) {
                fprintf(stderr, "Warning: could not write CSV row to %s\n",
                        params.csv_path);
            }
        }
    }

    MPI_Finalize();
    return 0;
}
