/* Dataset support for valkey-benchmark
 *
 * Copyright (c) 2009-2012, Redis Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef VALKEY_BENCHMARK_DATASET_H
#define VALKEY_BENCHMARK_DATASET_H

#include "sds.h"
#include <stddef.h>
#include <stdatomic.h>

/* Dataset constants */
#define MAX_DATASET_FIELDS 64
#define FIELD_PREFIX "__field:"
#define FIELD_PREFIX_LEN 8
#define FIELD_SUFFIX "__"
#define FIELD_SUFFIX_LEN 2

/* Dataset format types */
typedef enum datasetFormat {
    DATASET_FORMAT_CSV = 0,
    DATASET_FORMAT_TSV,
    DATASET_FORMAT_XML
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
    const char *xml_root_element;
    int max_documents;
} dataset;

/* Initialize dataset from file - returns NULL on error */
dataset *datasetInit(const char *filename, const char *xml_root_element, int max_documents, int has_field_placeholders, sds *template_argv, int template_argc);

/* Free dataset and all memory */
void datasetFree(dataset *ds);

/* Get number of records */
size_t datasetGetRecordCount(dataset *ds);

/* Report memory usage */
void datasetReportMemory(dataset *ds);

/* Generate complete command for given record index (caller must sdsfree) */
sds datasetGenerateCommand(dataset *ds, int record_index, sds *template_argv, int template_argc, _Atomic uint64_t *seq_key, int replace_placeholders, int keyspacelen, int sequential_replacement);

#endif /* VALKEY_BENCHMARK_DATASET_H */
