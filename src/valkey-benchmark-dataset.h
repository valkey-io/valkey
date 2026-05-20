/* Dataset support for valkey-benchmark
 *
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 */

#ifndef VALKEY_BENCHMARK_DATASET_H
#define VALKEY_BENCHMARK_DATASET_H

#include "sds.h"
#include <stdbool.h>
#include <stddef.h>
#ifndef __cplusplus
#include <stdatomic.h>
#endif

/* Dataset constants */
#define MAX_DATASET_FIELDS 1000
#define MAX_FIELD_NAME_LEN 512
#define FIELD_PREFIX "__field:"
#define FIELD_PREFIX_LEN 8
#define FIELD_SUFFIX "__"
#define FIELD_SUFFIX_LEN 2

/* Dataset format types */
typedef enum datasetFormat {
    DATASET_FORMAT_CSV = 0,
    DATASET_FORMAT_TSV
} datasetFormat;

/* Dataset structures */
typedef struct datasetRecord {
    sds *fields;
} datasetRecord;

typedef struct dataset {
    datasetFormat format;
    char delimiter;
    sds *field_names;
    int field_count;
    int *field_map;
    int used_field_count;
    datasetRecord *records;
    size_t record_count;
    const char *filename;
    int max_documents;
} dataset;

/* Initialize dataset from file - returns NULL on error */
dataset *datasetInit(const char *filename, int max_documents, int has_field_placeholders, sds *template_argv, int template_argc, int verbose);

/* Free dataset and all memory */
void datasetFree(dataset *ds);

/* Get number of records */
size_t datasetGetRecordCount(dataset *ds);

#ifndef __cplusplus
/* Generate complete command for given record index (caller must sdsfree) */
sds datasetGenerateCommand(dataset *ds, int record_index, sds *template_argv, int template_argc, _Atomic uint64_t *seq_key, int replace_placeholders, int keyspacelen, int sequential_replacement);
#endif

#endif /* VALKEY_BENCHMARK_DATASET_H */
