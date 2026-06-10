/* Dataset support for valkey-benchmark
 *
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 */

#include "fmacros.h"

#include "valkey-benchmark-dataset.h"
#include "zmalloc.h"
#include <valkey/valkey.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Internal constants */
#define PLACEHOLDER_COUNT 10
#define PLACEHOLDER_LEN 12

static const char *PLACEHOLDERS[PLACEHOLDER_COUNT] = {
    "__rand_int__", "__rand_1st__", "__rand_2nd__", "__rand_3rd__", "__rand_4th__",
    "__rand_5th__", "__rand_6th__", "__rand_7th__", "__rand_8th__", "__rand_9th__"};

/* Forward declarations */
static bool datasetBuildFieldMap(dataset *ds, sds *template_argv, int template_argc);
static sds getFieldValue(const char *row, int column_index, char delimiter);
static bool csvDiscoverFields(dataset *ds);
static bool csvLoadDocuments(dataset *ds, int verbose);
static bool shouldStopLoading(dataset *ds);
static int findFieldIndex(dataset *ds, const char *field_name, size_t field_name_len);
static const char *extractDatasetFieldValue(dataset *ds, int field_idx, int record_index);
static sds replaceOccurrence(sds processed_arg, const char *pos, const char *replacement);
static sds processFieldsInArg(dataset *ds, sds arg, int record_index);
static sds processRandPlaceholdersForDataSet(sds cmd, _Atomic uint64_t *seq_key, int replace_placeholders, int keyspacelen, int sequential_replacement);

dataset *datasetInit(const char *filename, int max_documents, int has_field_placeholders, sds *template_argv, int template_argc, int verbose) {
    if (!filename) return NULL;

    dataset *ds = zcalloc(sizeof(dataset));
    if (!ds) return NULL;

    ds->filename = filename;
    ds->max_documents = max_documents;

    /* Detect format */
    if (strstr(filename, ".csv")) {
        ds->format = DATASET_FORMAT_CSV;
        ds->delimiter = ',';
    } else if (strstr(filename, ".tsv")) {
        ds->format = DATASET_FORMAT_TSV;
        ds->delimiter = '\t';
    } else {
        ds->format = DATASET_FORMAT_CSV;
        ds->delimiter = ',';
    }

    if (!csvDiscoverFields(ds)) goto error;

    /* Build field map if needed (BEFORE loading) */
    if (has_field_placeholders && template_argv && template_argc > 0) {
        if (!datasetBuildFieldMap(ds, template_argv, template_argc)) goto error;
    } else {
        ds->used_field_count = ds->field_count;
    }

    if (!csvLoadDocuments(ds, verbose)) goto error;

    return ds;

error:
    datasetFree(ds);
    return NULL;
}

void datasetFree(dataset *ds) {
    if (!ds) return;

    if (ds->field_names) {
        /* Unified memory management: all formats use zmalloc + individual sdsnew */
        for (int i = 0; i < ds->field_count; i++) {
            sdsfree(ds->field_names[i]);
        }
        zfree(ds->field_names);
    }

    if (ds->field_map) {
        zfree(ds->field_map);
    }

    if (ds->records) {
        for (size_t i = 0; i < ds->record_count; i++) {
            if (ds->records[i].fields) {
                for (int j = 0; j < ds->used_field_count; j++) {
                    sdsfree(ds->records[i].fields[j]);
                }
                zfree(ds->records[i].fields);
            }
        }
        zfree(ds->records);
    }

    zfree(ds);
}

bool datasetBuildFieldMap(dataset *ds, sds *template_argv, int template_argc) {
    if (!ds) return false;

    ds->field_map = zmalloc(ds->field_count * sizeof(int));
    ds->used_field_count = 0;

    for (int i = 0; i < ds->field_count; i++) {
        ds->field_map[i] = -1;
    }

    for (int arg_idx = 0; arg_idx < template_argc; arg_idx++) {
        const char *arg = template_argv[arg_idx];
        const char *field_pos = strstr(arg, FIELD_PREFIX);

        while (field_pos) {
            const char *field_start = field_pos + FIELD_PREFIX_LEN;
            const char *field_end = strstr(field_start, FIELD_SUFFIX);
            if (!field_end) break;

            size_t field_name_len = field_end - field_start;
            sds field_name = sdsnewlen(field_start, field_name_len);

            int field_idx = -1;
            for (int k = 0; k < ds->field_count; k++) {
                if (!strcmp(field_name, ds->field_names[k])) {
                    field_idx = k;
                    break;
                }
            }

            if (field_idx == -1) {
                fprintf(stderr, "Error: Field placeholder '__field:%s__' not found in dataset fields\n", field_name);
                fprintf(stderr, "Available fields: ");
                for (int j = 0; j < ds->field_count; j++) {
                    fprintf(stderr, "%s%s", ds->field_names[j], (j < ds->field_count - 1) ? ", " : "\n");
                }
                sdsfree(field_name);
                return false;
            }

            if (ds->field_map[field_idx] == -1) {
                ds->field_map[field_idx] = ds->used_field_count++;
            }

            sdsfree(field_name);
            field_pos = strstr(field_end + FIELD_SUFFIX_LEN, FIELD_PREFIX);
        }
    }

    return true;
}

size_t datasetGetRecordCount(dataset *ds) {
    return ds ? ds->record_count : 0;
}

sds datasetGenerateCommand(dataset *ds, int record_index, sds *template_argv, int template_argc, _Atomic uint64_t *seq_key, int replace_placeholders, int keyspacelen, int sequential_replacement) {
    if (!ds || !template_argv) return NULL;

    sds *processed_argv = zmalloc(template_argc * sizeof(sds));
    for (int i = 0; i < template_argc; i++) {
        processed_argv[i] = processFieldsInArg(ds, sdsdup(template_argv[i]), record_index);
    }

    char *cmd = NULL;
    int len = valkeyFormatCommandArgv(&cmd, template_argc, (const char **)processed_argv, NULL);
    if (len == -1) {
        for (int i = 0; i < template_argc; i++) {
            sdsfree(processed_argv[i]);
        }
        zfree(processed_argv);
        if (cmd) {
            free(cmd);
        }
        return NULL;
    }
    sds result = sdsnewlen(cmd, len);
    free(cmd);

    result = processRandPlaceholdersForDataSet(result, seq_key, replace_placeholders,
                                               keyspacelen, sequential_replacement);

    for (int i = 0; i < template_argc; i++) {
        sdsfree(processed_argv[i]);
    }
    zfree(processed_argv);

    return result;
}

static bool shouldStopLoading(dataset *ds) {
    if (ds->max_documents > 0 && (int)ds->record_count >= ds->max_documents) {
        return true;
    }
    return false;
}

static sds getFieldValue(const char *row, int column_index, char delimiter) {
    int current_col = 0;
    const char *start = row;
    const char *p = row;
    int in_quotes = 0;

    while (*p) {
        if (*p == '"') {
            in_quotes = !in_quotes;
        } else if (*p == delimiter && !in_quotes) {
            if (current_col == column_index) {
                size_t len = p - start;
                if (len > 0 && start[0] == '"' && p[-1] == '"') {
                    start++;
                    len -= 2;
                }
                return sdsnewlen(start, len);
            }
            current_col++;
            start = p + 1;
        }
        p++;
    }

    if (current_col == column_index) {
        size_t len = p - start;
        if (len > 0 && start[0] == '"' && p[-1] == '"') {
            start++;
            len -= 2;
        }
        return sdsnewlen(start, len);
    }

    return sdsempty();
}

static bool csvDiscoverFields(dataset *ds) {
    FILE *fp = fopen(ds->filename, "r");
    if (!fp) {
        fprintf(stderr, "Cannot open dataset file: %s\n", ds->filename);
        return false;
    }

    char *line = NULL;
    size_t len = 0;
    if (getline(&line, &len, fp) == -1) {
        fprintf(stderr, "Cannot read header from dataset file\n");
        free(line);
        fclose(fp);
        return false;
    }

    len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
    if (len > 1 && line[len - 2] == '\r') line[len - 2] = '\0';

    int count;
    char delim_str[2] = {ds->delimiter, '\0'};
    sds *temp = sdssplitlen(line, strlen(line), delim_str, 1, &count);

    ds->field_names = zmalloc(count * sizeof(sds));
    for (int i = 0; i < count; i++) {
        ds->field_names[i] = sdsdup(temp[i]);
    }
    sdsfreesplitres(temp, count);
    ds->field_count = count;

    free(line);
    fclose(fp);
    return true;
}

static bool csvLoadDocuments(dataset *ds, int verbose) {
    FILE *fp = fopen(ds->filename, "r");
    if (!fp) return false;

    char *line = NULL;
    size_t len = 0;
    if (getline(&line, &len, fp) == -1) {
        fprintf(stderr, "Cannot read header from dataset file\n");
        free(line);
        fclose(fp);
        return false;
    }

    size_t capacity = 1000;
    ds->records = zmalloc(sizeof(datasetRecord) * capacity);

    if (verbose) {
        fprintf(stderr, "Loading dataset from %s...\n", ds->filename);
    }

    int *load_indices = NULL;
    int load_count = 0;
    if (ds->field_map) {
        load_indices = zmalloc(ds->used_field_count * sizeof(int));
        for (int i = 0; i < ds->field_count; i++) {
            if (ds->field_map[i] >= 0) {
                load_indices[load_count++] = i;
            }
        }
    }

    while (getline(&line, &len, fp) != -1 && !shouldStopLoading(ds)) {
        if (line[0] == '\0' || line[0] == '\n') continue;

        size_t line_len = strlen(line);
        if (line_len > 0 && line[line_len - 1] == '\n') line[line_len - 1] = '\0';
        if (line_len > 1 && line[line_len - 2] == '\r') line[line_len - 2] = '\0';

        if (ds->record_count >= capacity) {
            capacity *= 2;
            ds->records = zrealloc(ds->records, sizeof(datasetRecord) * capacity);
        }

        datasetRecord *record = &ds->records[ds->record_count];
        record->fields = zmalloc(sizeof(sds) * ds->used_field_count);

        if (ds->field_map) {
            for (int j = 0; j < load_count; j++) {
                int orig_idx = load_indices[j];
                int mapped_idx = ds->field_map[orig_idx];
                record->fields[mapped_idx] = getFieldValue(line, orig_idx, ds->delimiter);
            }
        } else {
            for (int i = 0; i < ds->field_count; i++) {
                record->fields[i] = getFieldValue(line, i, ds->delimiter);
            }
        }

        ds->record_count++;

        if (verbose && ds->record_count % 1000 == 0) {
            fprintf(stderr, "\rLoaded %zu documents...", ds->record_count);
            fflush(stderr);
        }
    }

    if (verbose) {
        fprintf(stderr, "\rLoaded %zu documents%*s\n", ds->record_count, 20, "");
    }

    if (load_indices) zfree(load_indices);
    free(line);
    fclose(fp);
    return true;
}

static int findFieldIndex(dataset *ds, const char *field_name, size_t field_name_len) {
    for (int k = 0; k < ds->field_count; k++) {
        if (strlen(ds->field_names[k]) == field_name_len &&
            !memcmp(ds->field_names[k], field_name, field_name_len)) {
            return ds->field_map ? ds->field_map[k] : k;
        }
    }
    return -1;
}

static const char *extractDatasetFieldValue(dataset *ds, int field_idx, int record_index) {
    return ds->records[record_index].fields[field_idx];
}

static sds replaceOccurrence(sds processed_arg, const char *pos, const char *replacement) {
    size_t offset = pos - processed_arg;
    size_t replacement_len = strlen(replacement);
    size_t total_len = offset + replacement_len + (sdslen(processed_arg) - offset - PLACEHOLDER_LEN);

    sds result = sdsnewlen(NULL, total_len);
    char *p = result;

    memcpy(p, processed_arg, offset);
    p += offset;

    memcpy(p, replacement, replacement_len);
    p += replacement_len;

    const char *after_start = pos + PLACEHOLDER_LEN;
    size_t after_len = sdslen(processed_arg) - offset - PLACEHOLDER_LEN;
    memcpy(p, after_start, after_len);

    sdsfree(processed_arg);
    return result;
}

static sds processFieldsInArg(dataset *ds, sds arg, int record_index) {
    if (!strstr(arg, FIELD_PREFIX)) return arg;

    /* Loop through all field placeholders in the argument */
    while (strstr(arg, FIELD_PREFIX)) {
        const char *field_pos = strstr(arg, FIELD_PREFIX);
        const char *field_start = field_pos + FIELD_PREFIX_LEN;
        const char *field_end = strstr(field_start, FIELD_SUFFIX);
        if (!field_end) break;

        size_t field_name_len = field_end - field_start;
        int field_idx = findFieldIndex(ds, field_start, field_name_len);
        if (field_idx == -1) break;

        const char *field_value = extractDatasetFieldValue(ds, field_idx, record_index);
        size_t before_len = field_pos - arg;
        const char *after_start = field_end + FIELD_SUFFIX_LEN;

        sds result = sdsnewlen(arg, before_len);
        result = sdscat(result, field_value);
        result = sdscat(result, after_start);

        sdsfree(arg);
        arg = result;
    }

    return arg;
}

static sds processRandPlaceholdersForDataSet(sds cmd, _Atomic uint64_t *seq_key, int replace_placeholders, int keyspacelen, int sequential_replacement) {
    if (!replace_placeholders || keyspacelen == 0) return cmd;

    for (int ph = 0; ph < PLACEHOLDER_COUNT; ph++) {
        if (!strstr(cmd, PLACEHOLDERS[ph])) continue;

        uint64_t shared_key = 0;
        int generate_shared_key = (ph != 0);

        if (generate_shared_key) {
            if (sequential_replacement) {
                shared_key = atomic_fetch_add_explicit(&seq_key[ph], 1, memory_order_relaxed);
            } else {
                shared_key = (uint64_t)random();
            }
            shared_key %= keyspacelen;
        }

        size_t search_offset = 0;
        char *pos;
        while ((pos = strstr(cmd + search_offset, PLACEHOLDERS[ph])) != NULL) {
            uint64_t key = generate_shared_key ? shared_key : 0;

            if (!generate_shared_key) {
                if (sequential_replacement) {
                    key = atomic_fetch_add_explicit(&seq_key[ph], 1, memory_order_relaxed);
                } else {
                    key = (uint64_t)random();
                }
                key %= keyspacelen;
            }

            char key_str[24];
            snprintf(key_str, sizeof(key_str), "%012llu", (unsigned long long)key);

            size_t offset = pos - cmd;
            cmd = replaceOccurrence(cmd, pos, key_str);
            search_offset = offset + PLACEHOLDER_LEN;
        }
    }

    return cmd;
}
