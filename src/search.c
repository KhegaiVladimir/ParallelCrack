#include "search.h"

#include <limits.h>
#include <string.h>

/*
 * The three alphabets. Declared here as plain static strings so that the index ->
 * candidate mapping is completely defined by this file and nothing else.
 */
static const char ALPHABET_LOWER_SYMBOLS[]     = "abcdefghijklmnopqrstuvwxyz";
static const char ALPHABET_ALNUM_SYMBOLS[]     =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

/* Printable ASCII, codes 32 ('space') through 126 ('~'): 95 characters. We build
   it once at first use so we do not have to type all 95 characters by hand. */
static char ALPHABET_PRINTABLE_SYMBOLS[96];

static void build_printable_alphabet(void)
{
    if (ALPHABET_PRINTABLE_SYMBOLS[0] != '\0') {
        return; /* already built */
    }
    int index = 0;
    for (int code = 32; code <= 126; code++) {
        ALPHABET_PRINTABLE_SYMBOLS[index] = (char)code;
        index++;
    }
    ALPHABET_PRINTABLE_SYMBOLS[index] = '\0';
}

const char *alphabet_symbols(alphabet_id id)
{
    if (id == ALPHABET_LOWER) {
        return ALPHABET_LOWER_SYMBOLS;
    } else if (id == ALPHABET_ALNUM) {
        return ALPHABET_ALNUM_SYMBOLS;
    } else {
        build_printable_alphabet();
        return ALPHABET_PRINTABLE_SYMBOLS;
    }
}

int alphabet_count(alphabet_id id)
{
    if (id == ALPHABET_LOWER) {
        return 26;
    } else if (id == ALPHABET_ALNUM) {
        return 62;
    } else {
        return 95;
    }
}

int search_config_init(search_config *config, alphabet_id id, int length,
                       long batch_size)
{
    if (length < 1 || length > SEARCH_MAX_LENGTH) {
        return -1;
    }
    if (batch_size < 1) {
        return -1;
    }

    config->symbols       = alphabet_symbols(id);
    config->symbol_count  = alphabet_count(id);
    config->length        = length;
    config->batch_size    = batch_size;
    config->poll_interval = 1;  /* default: check for a stop after every batch */

    /*
     * Compute keyspace = symbol_count ^ length, checking for overflow as we go.
     * We multiply step by step and bail out if the next multiply would exceed
     * LONG_MAX. This keeps every index representable as a signed long.
     */
    long keyspace = 1;
    for (int i = 0; i < length; i++) {
        if (keyspace > LONG_MAX / config->symbol_count) {
            return -1; /* would overflow */
        }
        keyspace = keyspace * config->symbol_count;
    }
    config->keyspace = keyspace;

    /* Leave the target digest zeroed until a search_set_target_* call fills it. */
    memset(config->target_digest, 0, CRYPTO_SHA256_LEN);
    return 0;
}

void search_index_to_candidate(const search_config *config, long index, char *out)
{
    /*
     * Interpret "index" as a base-symbol_count number, most significant digit
     * first. We fill the string from the last position backwards, peeling off one
     * base-symbol_count digit at a time, which is the standard way to convert an
     * integer into a fixed-width representation in another base.
     */
    long remaining = index;
    for (int position = config->length - 1; position >= 0; position--) {
        int digit = (int)(remaining % config->symbol_count);
        out[position] = config->symbols[digit];
        remaining = remaining / config->symbol_count;
    }
    out[config->length] = '\0';
}

void search_set_target_index(search_config *config, long index)
{
    char candidate[SEARCH_MAX_LENGTH + 1];
    search_index_to_candidate(config, index, candidate);
    crypto_sha256((const unsigned char *)candidate, (size_t)config->length,
                  config->target_digest);
}

int search_set_target_password(search_config *config, const char *password)
{
    int password_length = (int)strlen(password);
    if (password_length != config->length) {
        return -1;
    }

    /* Every character must belong to the configured alphabet, otherwise the
       search could never generate it and the run would be meaningless. */
    for (int i = 0; i < password_length; i++) {
        int found = 0;
        for (int s = 0; s < config->symbol_count; s++) {
            if (config->symbols[s] == password[i]) {
                found = 1;
                break;
            }
        }
        if (found == 0) {
            return -1;
        }
    }

    crypto_sha256((const unsigned char *)password, (size_t)password_length,
                  config->target_digest);
    return 0;
}

void search_set_target_absent(search_config *config)
{
    /*
     * Hash a string that the search can never generate (it is longer than any
     * candidate the engine produces), guaranteeing no candidate index can match.
     * This lets us exercise the "searched everything, found nothing" code path.
     */
    static const char impossible[] = "this-target-is-not-in-the-keyspace";
    crypto_sha256((const unsigned char *)impossible, sizeof(impossible) - 1,
                  config->target_digest);
}

int search_check_index(const search_config *config, long index)
{
    char candidate[SEARCH_MAX_LENGTH + 1];
    unsigned char digest[CRYPTO_SHA256_LEN];

    search_index_to_candidate(config, index, candidate);
    crypto_sha256((const unsigned char *)candidate, (size_t)config->length, digest);

    if (memcmp(digest, config->target_digest, CRYPTO_SHA256_LEN) == 0) {
        return 1;
    } else {
        return 0;
    }
}

long search_batch(const search_config *config, long start, long stride, long batch,
                  long *evaluated, long *next_index)
{
    long count = 0;          /* how many candidates we actually hashed */
    long index = start;      /* the index we are currently testing */
    long match = -1;         /* the matching index, or -1 if none found */

    while (count < batch && index < config->keyspace) {
        if (search_check_index(config, index) == 1) {
            match = index;
            /* Advance past the match so a resumed search would not retest it, then
               stop: this batch found the answer. */
            index = index + stride;
            count = count + 1;
            break;
        }
        index = index + stride;
        count = count + 1;
    }

    *evaluated  = count;
    *next_index = index;
    return match;
}
