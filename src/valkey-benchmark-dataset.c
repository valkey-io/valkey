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

#include "fmacros.h"

#include "valkey-benchmark-dataset.h"
#include "zmalloc.h"
#include <valkey/valkey.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Internal constants for rand placeholders */
#define PLACEHOLDER_COUNT 10
#define PLACEHOLDER_LEN 12

static const char *PLACEHOLDERS[PLACEHOLDER_COUNT] = {
    "__rand_int__", "__rand_1st__", "__rand_2nd__", "__rand_3rd__", "__rand_4th__",
    "__rand_5th__", "__rand_6th__", "__rand_7th__", "__rand_8th__", "__rand_9th__"};

/* Forward declarations */
static int datasetBuildFieldMap(dataset *ds, sds *template_argv, int template_argc);
static sds getFieldValue(const char *row, int column_index, char delimiter);
static sds getXmlFieldValue(const char *xml_doc, const char *field_name);
static sds formatBytes(size_t bytes);
static int csvDiscoverFields(dataset *ds);
static int scanXmlFieldsFromFile(dataset *ds, const char *xml_root_element);
static int scanXmlFields(const char *doc_start, const char *doc_end, dataset *ds, const char *start_root_tag, const char *end_root_tag);
static int loadXmlDataset(dataset *ds, const char *xml_root_element);
static int csvLoadDocuments(dataset *ds);
static int shouldStopLoading(dataset *ds);
static int findFieldIndex(dataset *ds, const char *field_name, size_t field_name_len);
static const char *extractDatasetFieldValue(dataset *ds, int field_idx, int record_index);
static sds replaceOccurrence(sds processed_arg, const char *pos, const char *replacement);
static sds processFieldsInArg(dataset *ds, sds arg, int record_index);
static sds processRandPlaceholdersForDataSet(sds cmd, _Atomic uint64_t *seq_key, int replace_placeholders, int keyspacelen, int sequential_replacement);

dataset *datasetInit(const char *filename, const char *xml_root_element, int max_documents, int has_field_placeholders, sds *template_argv, int template_argc) {
    if (!filename) return NULL;

    dataset *ds = zcalloc(sizeof(dataset));
    if (!ds) return NULL;

    ds->filename = filename;
    ds->xml_root_element = xml_root_element;
    ds->max_documents = max_documents;

    /* Validate XML parameters */
    if (strstr(filename, ".xml") && !xml_root_element) {
        fprintf(stderr, "Error: XML dataset requires --xml-root-element parameter\n");
        zfree(ds);
        return NULL;
    }

    /* Detect format */
    if (strstr(filename, ".csv")) {
        ds->format = DATASET_FORMAT_CSV;
        ds->delimiter = ',';
    } else if (strstr(filename, ".tsv")) {
        ds->format = DATASET_FORMAT_TSV;
        ds->delimiter = '\t';
    } else if (strstr(filename, ".xml")) {
        ds->format = DATASET_FORMAT_XML;
        ds->delimiter = 0;
    } else {
        ds->format = DATASET_FORMAT_CSV;
        ds->delimiter = ',';
    }

    /* Discover fields */
    if (ds->format == DATASET_FORMAT_XML) {
        if (!scanXmlFieldsFromFile(ds, xml_root_element)) goto error;
    } else {
        if (!csvDiscoverFields(ds)) goto error;
    }

    /* Build field map if needed (BEFORE loading) */
    if (has_field_placeholders && template_argv && template_argc > 0) {
        if (!datasetBuildFieldMap(ds, template_argv, template_argc)) goto error;
    } else {
        ds->used_field_count = ds->field_count;
    }

    /* Load data with correct field count */
    if (ds->format == DATASET_FORMAT_XML) {
        if (!loadXmlDataset(ds, xml_root_element)) goto error;
    } else {
        if (!csvLoadDocuments(ds)) goto error;
    }

    return ds;

error:
    datasetFree(ds);
    return NULL;
}

void datasetFree(dataset *ds) {
    if (!ds) return;

    if (ds->field_names) {
        sdsfreesplitres(ds->field_names, ds->field_count);
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

int datasetBuildFieldMap(dataset *ds, sds *template_argv, int template_argc) {
    if (!ds) return 0;

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
                return 0;
            }

            if (ds->field_map[field_idx] == -1) {
                ds->field_map[field_idx] = ds->used_field_count++;
            }

            sdsfree(field_name);
            field_pos = strstr(field_end + FIELD_SUFFIX_LEN, FIELD_PREFIX);
        }
    }

    return 1;
}

int datasetLoad(dataset *ds, const char *xml_root_element) {
    if (!ds) return 0;
    (void)xml_root_element; /* Use stored value in ds */

    if (ds->format == DATASET_FORMAT_XML) {
        return loadXmlDataset(ds, ds->xml_root_element);
    } else {
        return csvLoadDocuments(ds);
    }
}

size_t datasetGetRecordCount(dataset *ds) {
    return ds ? ds->record_count : 0;
}

void datasetReportMemory(dataset *ds) {
    if (!ds) return;

    size_t total_memory = 0;
    for (size_t i = 0; i < ds->record_count; i++) {
        for (int j = 0; j < ds->used_field_count; j++) {
            total_memory += sdslen(ds->records[i].fields[j]);
        }
    }
    sds size_str = formatBytes(total_memory);
    printf("Dataset: %zu documents (%s)\n", ds->record_count, size_str);
    sdsfree(size_str);
}

sds datasetGenerateCommand(dataset *ds, int record_index, sds *template_argv, int template_argc, _Atomic uint64_t *seq_key, int replace_placeholders, int keyspacelen, int sequential_replacement) {
    if (!ds || !template_argv) return NULL;

    sds *processed_argv = zmalloc(template_argc * sizeof(sds));
    for (int i = 0; i < template_argc; i++) {
        processed_argv[i] = processFieldsInArg(ds, sdsdup(template_argv[i]), record_index);
    }

    char *cmd;
    int len = valkeyFormatCommandArgv(&cmd, template_argc, (const char **)processed_argv, NULL);
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

static sds formatBytes(size_t bytes) {
    if (bytes < 1024) {
        return sdscatprintf(sdsempty(), "%zu bytes", bytes);
    } else if (bytes < 1024 * 1024) {
        return sdscatprintf(sdsempty(), "%.2f KB", bytes / 1024.0);
    } else if (bytes < 1024 * 1024 * 1024) {
        return sdscatprintf(sdsempty(), "%.2f MB", bytes / (1024.0 * 1024.0));
    } else {
        return sdscatprintf(sdsempty(), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    }
}

static int shouldStopLoading(dataset *ds) {
    if (ds->max_documents > 0 && (int)ds->record_count >= ds->max_documents) {
        return 1;
    }
    return 0;
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

static sds getXmlFieldValue(const char *xml_doc, const char *field_name) {
    char start_tag_prefix[128], end_tag[128];
    snprintf(start_tag_prefix, sizeof(start_tag_prefix), "<%s", field_name);
    snprintf(end_tag, sizeof(end_tag), "</%s>", field_name);

    const char *tag_start = strstr(xml_doc, start_tag_prefix);
    if (!tag_start) return sdsempty();

    const char *tag_end = strchr(tag_start, '>');
    if (!tag_end) return sdsempty();

    if (tag_end > tag_start && tag_end[-1] == '/') {
        return sdsempty();
    }

    const char *content_start = tag_end + 1;
    const char *closing_tag = strstr(content_start, end_tag);
    if (!closing_tag) return sdsempty();

    size_t content_len = closing_tag - content_start;
    return sdsnewlen(content_start, content_len);
}

static int csvDiscoverFields(dataset *ds) {
    FILE *fp = fopen(ds->filename, "r");
    if (!fp) {
        fprintf(stderr, "Cannot open dataset file: %s\n", ds->filename);
        return 0;
    }

    char *line = NULL;
    size_t len = 0;
    if (getline(&line, &len, fp) == -1) {
        fprintf(stderr, "Cannot read header from dataset file\n");
        free(line);
        fclose(fp);
        return 0;
    }

    len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
    if (len > 1 && line[len - 2] == '\r') line[len - 2] = '\0';

    int count;
    char delim_str[2] = {ds->delimiter, '\0'};
    ds->field_names = sdssplitlen(line, strlen(line), delim_str, 1, &count);
    ds->field_count = count;

    free(line);
    fclose(fp);
    return 1;
}

static int scanXmlFields(const char *doc_start, const char *doc_end, dataset *ds, const char *start_root_tag, const char *end_root_tag) {
    char field_names[MAX_DATASET_FIELDS][64];
    int field_count = 0;
    int root_start_tag_len = strlen(start_root_tag);
    int root_end_tag_len = strlen(end_root_tag);

    const char *current_pos = doc_start;
    while ((current_pos = strchr(current_pos, '<')) != NULL && current_pos < doc_end) {
        if (current_pos[1] == '/' || current_pos[1] == '!' ||
            !strncmp(current_pos, start_root_tag, root_start_tag_len) ||
            !strncmp(current_pos, end_root_tag, root_end_tag_len)) {
            current_pos++;
            continue;
        }

        const char *tag_end = strchr(current_pos, '>');
        if (!tag_end || tag_end >= doc_end) break;

        const char *field_start = current_pos + 1;
        const char *field_name_end = field_start;

        while (field_name_end < tag_end && *field_name_end != ' ' && *field_name_end != '\t') {
            field_name_end++;
        }

        size_t field_name_len = field_name_end - field_start;

        if (field_name_len == 0 || field_name_len >= 64) {
            current_pos = tag_end + 1;
            continue;
        }

        int is_duplicate = 0;
        for (int i = 0; i < field_count; i++) {
            if (strlen(field_names[i]) == field_name_len &&
                !memcmp(field_names[i], field_start, field_name_len)) {
                is_duplicate = 1;
                break;
            }
        }

        if (!is_duplicate && field_count < MAX_DATASET_FIELDS) {
            memcpy(field_names[field_count], field_start, field_name_len);
            field_names[field_count][field_name_len] = '\0';
            field_count++;
        }

        current_pos = tag_end + 1;
    }

    if (field_count == 0) return 0;

    ds->field_names = zmalloc(field_count * sizeof(sds));
    for (int i = 0; i < field_count; i++) {
        ds->field_names[i] = sdsnew(field_names[i]);
    }
    ds->field_count = field_count;

    return 1;
}

static int scanXmlFieldsFromFile(dataset *ds, const char *xml_root_element) {
    FILE *fp = fopen(ds->filename, "r");
    if (!fp) return 0;

    char start_tag_prefix[64], start_tag[64], end_tag[64];
    snprintf(start_tag_prefix, sizeof(start_tag_prefix), "<%s", xml_root_element);
    snprintf(start_tag, sizeof(start_tag), "<%s>", xml_root_element);
    snprintf(end_tag, sizeof(end_tag), "</%s>", xml_root_element);

    char buffer[1024];
    sds current_doc = sdsempty();

    while (fgets(buffer, sizeof(buffer), fp)) {
        current_doc = sdscat(current_doc, buffer);

        const char *tag_prefix_pos = strstr(current_doc, start_tag_prefix);
        if (!tag_prefix_pos) continue;

        const char *tag_end_pos = strchr(tag_prefix_pos, '>');
        if (!tag_end_pos) continue;

        const char *doc_start = tag_prefix_pos;
        const char *doc_end = strstr(tag_end_pos, end_tag);
        if (!doc_end) continue;

        doc_end += strlen(end_tag);

        int result = scanXmlFields(doc_start, doc_end, ds, start_tag, end_tag);
        sdsfree(current_doc);
        fclose(fp);
        return result;
    }

    sdsfree(current_doc);
    fclose(fp);
    return 0;
}

static int loadXmlDataset(dataset *ds, const char *xml_root_element) {
    FILE *fp = fopen(ds->filename, "r");
    if (!fp) return 0;

    char start_tag_prefix[64], start_tag[64], end_tag[64];
    size_t end_tag_len;
    snprintf(start_tag_prefix, sizeof(start_tag_prefix), "<%s", xml_root_element);
    snprintf(start_tag, sizeof(start_tag), "<%s>", xml_root_element);
    snprintf(end_tag, sizeof(end_tag), "</%s>", xml_root_element);
    end_tag_len = strlen(end_tag);

    size_t buffer_capacity = 4 * 1024 * 1024;
    char *buffer = zmalloc(buffer_capacity);
    size_t buffer_used = 0;
    int fields_discovered = 0;
    size_t capacity = 1000;

    ds->records = zmalloc(sizeof(datasetRecord) * capacity);

    printf("Loading XML dataset from %s...\n", ds->filename);

    if (ds->field_names && ds->field_count > 0) {
        fields_discovered = 1;
        printf("Using %d fields: ", ds->field_count);
        for (int i = 0; i < ds->field_count; i++) {
            printf("%s%s", ds->field_names[i], (i < ds->field_count - 1) ? ", " : "\n");
        }
    }

    while (!shouldStopLoading(ds)) {
        size_t space_available = buffer_capacity - buffer_used;

        if (space_available == 0) {
            size_t new_capacity = buffer_capacity * 2;
            buffer = zrealloc(buffer, new_capacity);
            buffer_capacity = new_capacity;
            space_available = buffer_capacity - buffer_used;
        }

        size_t bytes_read = fread(buffer + buffer_used, 1, space_available, fp);
        buffer_used += bytes_read;

        if (buffer_used == 0) break;

        size_t scan_pos = 0;
        while (scan_pos < buffer_used) {
            const char *doc_start = NULL;
            const char *tag_open_end = NULL;

            for (size_t i = scan_pos; i < buffer_used; i++) {
                if (buffer[i] == '<' && i + 1 < buffer_used &&
                    strncmp(buffer + i, start_tag_prefix, strlen(start_tag_prefix)) == 0) {
                    doc_start = buffer + i;

                    for (size_t j = i + 1; j < buffer_used; j++) {
                        if (buffer[j] == '>') {
                            tag_open_end = buffer + j;
                            break;
                        }
                    }
                    break;
                }
            }

            if (!doc_start || !tag_open_end) break;

            const char *doc_end = NULL;
            for (size_t i = (tag_open_end - buffer); i + end_tag_len <= buffer_used; i++) {
                if (strncmp(buffer + i, end_tag, end_tag_len) == 0) {
                    doc_end = buffer + i + end_tag_len;
                    break;
                }
            }

            if (!doc_end) break;

            size_t doc_len = doc_end - doc_start;

            if (!fields_discovered) {
                if (!scanXmlFields(doc_start, doc_end, ds, start_tag, end_tag)) {
                    fprintf(stderr, "No XML fields discovered\n");
                    zfree(buffer);
                    fclose(fp);
                    return 0;
                }
                fields_discovered = 1;

                printf("Discovered %d fields: ", ds->field_count);
                for (int i = 0; i < ds->field_count; i++) {
                    printf("%s%s", ds->field_names[i], (i < ds->field_count - 1) ? ", " : "\n");
                }
            }

            if (ds->record_count >= capacity) {
                capacity *= 2;
                ds->records = zrealloc(ds->records, sizeof(datasetRecord) * capacity);
            }

            datasetRecord *record = &ds->records[ds->record_count];
            record->fields = zmalloc(sizeof(sds) * ds->used_field_count);

            sds doc_str = sdsnewlen(doc_start, doc_len);
            for (int i = 0; i < ds->field_count; i++) {
                if (ds->field_map && ds->field_map[i] >= 0) {
                    record->fields[ds->field_map[i]] = getXmlFieldValue(doc_str, ds->field_names[i]);
                }
            }
            sdsfree(doc_str);

            ds->record_count++;

            if (ds->record_count % 1000 == 0) {
                printf("\rLoaded %zu documents...", ds->record_count);
                fflush(stdout);
            }

            scan_pos = doc_end - buffer;
        }

        if (scan_pos > 0 && scan_pos < buffer_used) {
            size_t remaining = buffer_used - scan_pos;
            memmove(buffer, buffer + scan_pos, remaining);
            buffer_used = remaining;
        } else if (scan_pos == buffer_used) {
            buffer_used = 0;
        }

        if (bytes_read == 0 && (buffer_used == 0 || scan_pos == 0)) {
            break;
        }
    }

    printf("\rLoaded %zu documents%*s\n", ds->record_count, 20, "");

    zfree(buffer);
    fclose(fp);
    return 1;
}

static int csvLoadDocuments(dataset *ds) {
    FILE *fp = fopen(ds->filename, "r");
    if (!fp) return 0;

    char *line = NULL;
    size_t len = 0;
    if (getline(&line, &len, fp) == -1) {
        fprintf(stderr, "Cannot read header from dataset file\n");
        free(line);
        fclose(fp);
        return 0;
    }

    size_t capacity = 1000;
    ds->records = zmalloc(sizeof(datasetRecord) * capacity);

    const char *format_name = (ds->format == DATASET_FORMAT_CSV) ? "csv" : (ds->format == DATASET_FORMAT_TSV) ? "tsv"
                                                                                                              : "xml";
    (void)format_name; /* Suppress output in unit tests */

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
    }

    if (load_indices) zfree(load_indices);
    free(line);
    fclose(fp);
    return 1;
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

    const char *field_pos = strstr(arg, FIELD_PREFIX);
    const char *field_start = field_pos + FIELD_PREFIX_LEN;
    const char *field_end = strstr(field_start, FIELD_SUFFIX);
    if (!field_end) return arg;

    size_t field_name_len = field_end - field_start;
    int field_idx = findFieldIndex(ds, field_start, field_name_len);
    if (field_idx == -1) return arg;

    const char *field_value = extractDatasetFieldValue(ds, field_idx, record_index);
    size_t before_len = field_pos - arg;
    const char *after_start = field_end + FIELD_SUFFIX_LEN;

    sds result = sdsnewlen(arg, before_len);
    result = sdscat(result, field_value);
    result = sdscat(result, after_start);

    sdsfree(arg);
    return result;
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
