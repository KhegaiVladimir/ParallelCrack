#ifndef PARALLELCRACK_SEARCH_H
#define PARALLELCRACK_SEARCH_H

#include "crypto.h"

/*
 * The shared search workload.
 *
 * Every communication strategy uses this exact engine, so the only thing that
 * differs between strategies is how the processes coordinate -- not what they
 * search or how they verify a candidate. That is what makes the benchmark fair.
 *
 * The keyspace is the set of all strings of a fixed length over a fixed alphabet.
 * Each string is identified by an integer index in [0, keyspace). The mapping
 * from index to string is deterministic and identical on every rank, so two ranks
 * never disagree about which candidate a given index represents.
 */

/* Longest candidate we support. Keeps every candidate buffer a fixed small size. */
#define SEARCH_MAX_LENGTH 12

/* The alphabets the program can search over. Smaller alphabets keep quick
   correctness tests fast; the printable set is there for larger sweeps. */
typedef enum {
    ALPHABET_LOWER,     /* a-z              (26 symbols) */
    ALPHABET_ALNUM,     /* a-z A-Z 0-9      (62 symbols) */
    ALPHABET_PRINTABLE  /* ASCII 32..126    (95 symbols) */
} alphabet_id;

/*
 * Everything a rank needs to run the search. This struct is filled on rank 0 from
 * the command line and then broadcast unchanged to every other rank, so all ranks
 * share one identical configuration.
 */
typedef struct {
    const char   *symbols;      /* pointer to the alphabet characters */
    int           symbol_count; /* number of symbols in the alphabet */
    int           length;       /* candidate length */
    long          keyspace;     /* symbol_count ^ length */
    long          batch_size;   /* candidates searched between coordination points */
    int           poll_interval;/* batches between stop-message checks (async only) */
    unsigned char target_digest[CRYPTO_SHA256_LEN]; /* SHA-256 of the secret target */
} search_config;

/* Look up a named alphabet's characters and size. */
const char *alphabet_symbols(alphabet_id id);
int         alphabet_count(alphabet_id id);

/*
 * Initialize a config's alphabet, length, batch size, and keyspace.
 * Returns 0 on success, or -1 if length/batch are out of range or the keyspace
 * would overflow a signed long. The target digest is set separately below.
 */
int search_config_init(search_config *config, alphabet_id id, int length,
                       long batch_size);

/*
 * Map an index in [0, keyspace) to its candidate string.
 * The index is treated as a base-"symbol_count" number with the most significant
 * digit first, so index 0 is "aaaa", index 1 is "aaab", and so on. "out" must
 * have room for length + 1 bytes (the +1 is for the terminating '\0').
 */
void search_index_to_candidate(const search_config *config, long index, char *out);

/* Set the target to the candidate at a specific index (fully reproducible runs). */
void search_set_target_index(search_config *config, long index);

/*
 * Set the target to an explicit password.
 * Returns 0 on success, or -1 if the password's length does not match the config
 * or it contains a character outside the configured alphabet.
 */
int search_set_target_password(search_config *config, const char *password);

/* Set a target digest that no candidate in the keyspace can produce, so the
   search is guaranteed to exhaust without a match (used to test the no-match path). */
void search_set_target_absent(search_config *config);

/* Test a single candidate index against the target. Returns 1 on match, else 0. */
int search_check_index(const search_config *config, long index);

/*
 * Search one batch of a rank's cyclic slice of the keyspace.
 *
 * A rank owns the indices start, start + stride, start + 2*stride, ... This
 * function tests up to "batch" of them (stopping early if it runs past the end of
 * the keyspace). It writes the number of candidates actually hashed to
 * *evaluated, and the index to resume from next time to *next_index. A caller
 * knows its slice is exhausted when *next_index >= keyspace.
 *
 * Returns the matching global index if a candidate in this batch matched the
 * target, or -1 if none did.
 */
long search_batch(const search_config *config, long start, long stride, long batch,
                  long *evaluated, long *next_index);

#endif /* PARALLELCRACK_SEARCH_H */
