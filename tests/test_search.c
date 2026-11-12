/*
 * Unit tests for the shared search engine.
 *
 * These tests do NOT use MPI: they check the pure, deterministic parts of the
 * workload that every strategy relies on. The most important property verified
 * here is that the static cyclic distribution covers every candidate in the
 * keyspace exactly once -- no candidate is missed and none is searched twice --
 * which is what makes a parallel search correct no matter how the ranks coordinate.
 *
 * Build/run via `make test`, or directly (see tests/README.md).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "search.h"

static int failures = 0;

static void check(int condition, const char *message)
{
    if (condition != 0) {
        printf("  ok   : %s\n", message);
    } else {
        printf("  FAIL : %s\n", message);
        failures++;
    }
}

/*
 * Simulate the way every strategy walks its slice of the keyspace (rank r visits
 * r, r+P, r+2P, ... via repeated search_batch calls) for P ranks, and confirm the
 * union of all slices is exactly the whole keyspace with no overlaps.
 */
static void test_partition_coverage(search_config *config, int process_count)
{
    char *times_seen = calloc((size_t)config->keyspace, sizeof(char));
    if (times_seen == NULL) {
        printf("  FAIL : out of memory in coverage test\n");
        failures++;
        return;
    }

    long grand_total = 0;

    for (int rank = 0; rank < process_count; rank++) {
        long start = rank;
        while (start < config->keyspace) {
            long evaluated = 0;
            long next_index = start;
            /* Absent target means no early stop, so a batch evaluates a contiguous
               run of this rank's indices: start, start+P, ... (evaluated of them). */
            search_batch(config, start, process_count, config->batch_size,
                         &evaluated, &next_index);

            long index = start;
            for (long k = 0; k < evaluated; k++) {
                if (index < config->keyspace) {
                    times_seen[index]++;
                }
                index += process_count;
            }
            grand_total += evaluated;
            start = next_index;
        }
    }

    long missed = 0;
    long duplicated = 0;
    for (long i = 0; i < config->keyspace; i++) {
        if (times_seen[i] == 0) {
            missed++;
        } else if (times_seen[i] > 1) {
            duplicated++;
        }
    }

    char message[128];
    snprintf(message, sizeof(message),
             "P=%d covers all %ld candidates exactly once (missed=%ld dup=%ld total=%ld)",
             process_count, config->keyspace, missed, duplicated, grand_total);
    check(missed == 0 && duplicated == 0 && grand_total == config->keyspace, message);

    free(times_seen);
}

int main(void)
{
    search_config config;
    char candidate_a[SEARCH_MAX_LENGTH + 1];
    char candidate_b[SEARCH_MAX_LENGTH + 1];

    printf("search engine unit tests\n");

    /* --- configuration and keyspace --- */
    check(search_config_init(&config, ALPHABET_LOWER, 3, 500) == 0,
          "init: lower alphabet, length 3");
    check(config.keyspace == 17576, "keyspace: 26^3 == 17576");
    check(search_config_init(&config, ALPHABET_PRINTABLE, 12, 1) == -1,
          "init: rejects a keyspace that would overflow long");

    /* Re-init to a known small config for the rest of the tests. */
    search_config_init(&config, ALPHABET_LOWER, 3, 500);

    /* --- deterministic candidate generation --- */
    search_index_to_candidate(&config, 0, candidate_a);
    check(strcmp(candidate_a, "aaa") == 0, "index 0 maps to \"aaa\"");
    search_index_to_candidate(&config, 1, candidate_a);
    check(strcmp(candidate_a, "aab") == 0, "index 1 maps to \"aab\"");
    search_index_to_candidate(&config, config.keyspace - 1, candidate_a);
    check(strcmp(candidate_a, "zzz") == 0, "last index maps to \"zzz\"");

    search_index_to_candidate(&config, 8675, candidate_a);
    search_index_to_candidate(&config, 8675, candidate_b);
    check(strcmp(candidate_a, candidate_b) == 0,
          "same index always maps to the same candidate (deterministic)");

    /* --- partition coverage for several process counts --- */
    search_set_target_absent(&config);
    test_partition_coverage(&config, 1);
    test_partition_coverage(&config, 2);
    test_partition_coverage(&config, 3); /* does not evenly divide the keyspace */
    test_partition_coverage(&config, 4);
    test_partition_coverage(&config, 5);
    test_partition_coverage(&config, 8);

    /* --- target setting and verification --- */
    search_set_target_index(&config, 9999);
    check(search_check_index(&config, 9999) == 1,
          "check_index: the target index matches");
    check(search_check_index(&config, 0) == 0,
          "check_index: a different index does not match");

    check(search_set_target_password(&config, "cat") == 0,
          "set_target_password: accepts an in-alphabet password of the right length");
    check(search_set_target_password(&config, "CAT") == -1,
          "set_target_password: rejects out-of-alphabet characters");
    check(search_set_target_password(&config, "cats") == -1,
          "set_target_password: rejects a wrong-length password");

    /* --- after setting target "cat", a full scan must find exactly that index --- */
    search_set_target_password(&config, "cat");
    long found = -1;
    long start = 0;
    while (start < config.keyspace) {
        long evaluated = 0;
        long next_index = start;
        long match = search_batch(&config, start, 1, config.batch_size,
                                  &evaluated, &next_index);
        if (match >= 0) {
            found = match;
            break;
        }
        start = next_index;
    }
    search_index_to_candidate(&config, found, candidate_a);
    check(found >= 0 && strcmp(candidate_a, "cat") == 0,
          "serial scan finds the candidate \"cat\"");

    printf("\n");
    if (failures == 0) {
        printf("ALL UNIT TESTS PASSED\n");
        return 0;
    } else {
        printf("%d UNIT TEST FAILURE(S)\n", failures);
        return 1;
    }
}
