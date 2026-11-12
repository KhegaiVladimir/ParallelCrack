#include "benchmark.h"

#include <stdio.h>

/* The CSV column order. Kept in one place so the header and the row always match. */
static const char *CSV_HEADER =
    "strategy,nprocs,length,alphabet_size,batch_size,found,finder_rank,"
    "candidates_evaluated,wall_seconds,hashes_per_second,comm_seconds,rounds\n";

/*
 * Decide whether the file still needs a header. It needs one if it cannot be
 * opened for reading (does not exist yet) or if it is empty.
 */
static int file_needs_header(const char *path)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return 1; /* file does not exist yet */
    }

    int first_char = fgetc(file);
    fclose(file);

    if (first_char == EOF) {
        return 1; /* file exists but is empty */
    } else {
        return 0;
    }
}

int benchmark_append_csv(const char *path, const benchmark_record *record)
{
    int needs_header = file_needs_header(path);

    FILE *file = fopen(path, "a");
    if (file == NULL) {
        return -1;
    }

    if (needs_header == 1) {
        fputs(CSV_HEADER, file);
    }

    fprintf(file,
            "%s,%d,%d,%d,%ld,%d,%d,%ld,%.6f,%.0f,%.6f,%ld\n",
            record->strategy,
            record->nprocs,
            record->length,
            record->alphabet_size,
            record->batch_size,
            record->found,
            record->finder_rank,
            record->candidates_evaluated,
            record->wall_seconds,
            record->hashes_per_second,
            record->comm_seconds,
            record->rounds);

    fclose(file);
    return 0;
}
