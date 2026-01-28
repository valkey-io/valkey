/* Unit tests for valkey-benchmark dataset module
 *
 * Copyright (c) 2024, Redis Ltd.
 * All rights reserved.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "../valkey-benchmark-dataset.h"
#include "../zmalloc.h"
#include "../sds.h"
#include "test_help.h"

#define UNUSED(x) (void)(x)

static char *create_test_file(const char *suffix, const char *content) {
    static char filename[512];
    snprintf(filename, sizeof(filename), "/tmp/test_dataset_%d_%s", getpid(), suffix);
    FILE *f = fopen(filename, "w");
    if (!f) return NULL;
    if (content) fputs(content, f);
    fclose(f);
    return filename;
}

static void cleanup_test_file(const char *filename) {
    if (filename) unlink(filename);
}

int test_field_mapping_optimization(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    const char *csv = "a,b,c,d,e,f,g,h\n1,2,3,4,5,6,7,8\n";
    char *file = create_test_file("mapping.csv", csv);
    TEST_ASSERT(file != NULL);

    sds arg1 = sdsnew("HSET");
    sds arg2 = sdsnew("key");
    sds arg3 = sdsnew("b");
    sds arg4 = sdsnew("__field:b__");
    sds arg5 = sdsnew("f");
    sds arg6 = sdsnew("__field:f__");
    sds args[] = {arg1, arg2, arg3, arg4, arg5, arg6};
    dataset *ds = datasetInit(file, NULL, 0, 1, args, 6);

    TEST_ASSERT_MESSAGE("Dataset initialized", ds != NULL);
    TEST_ASSERT_MESSAGE("Total fields discovered", ds->field_count == 8);
    TEST_ASSERT_MESSAGE("Only used fields loaded", ds->used_field_count == 2);
    TEST_ASSERT_MESSAGE("Correct values loaded", !strcmp(ds->records[0].fields[0], "2"));
    TEST_ASSERT_MESSAGE("Correct values loaded", !strcmp(ds->records[0].fields[1], "6"));

    datasetFree(ds);
    sdsfree(arg1);
    sdsfree(arg2);
    sdsfree(arg3);
    sdsfree(arg4);
    sdsfree(arg5);
    sdsfree(arg6);
    cleanup_test_file(file);
    return 0;
}

int test_csv_quoted_fields(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    const char *csv = "id,title,desc\n"
                      "1,\"Book, Part 1\",\"Quote: \"\"Hello\"\"\"\n";
    char *file = create_test_file("quoted.csv", csv);
    TEST_ASSERT(file != NULL);

    sds arg1 = sdsnew("SET");
    sds arg2 = sdsnew("__field:title__");
    sds args[] = {arg1, arg2};
    dataset *ds = datasetInit(file, NULL, 0, 1, args, 2);

    TEST_ASSERT_MESSAGE("Dataset initialized", ds != NULL);
    TEST_ASSERT_MESSAGE("Quoted comma preserved", !strcmp(ds->records[0].fields[0], "Book, Part 1"));

    datasetFree(ds);
    sdsfree(arg1);
    sdsfree(arg2);
    cleanup_test_file(file);
    return 0;
}

int test_field_count_correctness(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    const char *csv = "id,title,author\n1,Book,Alice\n";
    char *file = create_test_file("fields.csv", csv);
    TEST_ASSERT(file != NULL);

    sds arg1 = sdsnew("GET");
    sds arg2 = sdsnew("__field:title__");
    sds args[] = {arg1, arg2};
    dataset *ds = datasetInit(file, NULL, 0, 1, args, 2);

    TEST_ASSERT_MESSAGE("Dataset initialized", ds != NULL);
    TEST_ASSERT_MESSAGE("Correct field count", ds->field_count == 3);
    TEST_ASSERT_MESSAGE("Correct record count", datasetGetRecordCount(ds) == 1);

    datasetFree(ds);
    sdsfree(arg1);
    sdsfree(arg2);
    cleanup_test_file(file);
    return 0;
}

int test_excessive_field_name_length(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Create a 513-character field name (over MAX_FIELD_NAME_LEN limit) */
    char excessive_field[514];
    memset(excessive_field, 'b', 513);
    excessive_field[513] = '\0';

    /* Build XML with the excessive field name */
    sds xml = sdsempty();
    xml = sdscat(xml, "<doc>\n");
    xml = sdscat(xml, "  <title>ValidValue</title>\n");
    xml = sdscat(xml, "  <");
    xml = sdscat(xml, excessive_field);
    xml = sdscat(xml, ">ExcessiveValue</");
    xml = sdscat(xml, excessive_field);
    xml = sdscat(xml, ">\n");
    xml = sdscat(xml, "</doc>\n");

    char *file = create_test_file("excessive_field.xml", xml);
    sdsfree(xml);
    TEST_ASSERT(file != NULL);

    /* Initialize dataset - only the valid field should be loaded */
    dataset *ds = datasetInit(file, "doc", 1, 0, NULL, 0);

    TEST_ASSERT_MESSAGE("Dataset initialized", ds != NULL);
    TEST_ASSERT_MESSAGE("Only valid field loaded (513-char field rejected)", ds->field_count == 1);
    TEST_ASSERT_MESSAGE("Valid field is 'title'", !strcmp(ds->field_names[0], "title"));

    datasetFree(ds);
    cleanup_test_file(file);
    return 0;
}

int test_max_dataset_fields(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Build XML with exactly 1000 fields (MAX_DATASET_FIELDS limit) */
    sds xml = sdsempty();
    xml = sdscat(xml, "<doc>\n");
    for (int i = 0; i < 1000; i++) {
        xml = sdscatprintf(xml, "  <field%d>value%d</field%d>\n", i, i, i);
    }
    xml = sdscat(xml, "</doc>\n");

    char *file = create_test_file("max_fields.xml", xml);
    sdsfree(xml);
    TEST_ASSERT(file != NULL);

    dataset *ds = datasetInit(file, "doc", 1, 0, NULL, 0);

    TEST_ASSERT_MESSAGE("Dataset initialized", ds != NULL);
    TEST_ASSERT_MESSAGE("1000 fields accepted (at MAX_DATASET_FIELDS limit)", ds->field_count == 1000);
    TEST_ASSERT_MESSAGE("First field correct", !strcmp(ds->field_names[0], "field0"));
    TEST_ASSERT_MESSAGE("Last field correct", !strcmp(ds->field_names[999], "field999"));

    datasetFree(ds);
    cleanup_test_file(file);
    return 0;
}

int test_excessive_dataset_fields(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Build XML with 1001 fields (over MAX_DATASET_FIELDS limit) */
    sds xml = sdsempty();
    xml = sdscat(xml, "<doc>\n");
    for (int i = 0; i < 1001; i++) {
        xml = sdscatprintf(xml, "  <field%d>value%d</field%d>\n", i, i, i);
    }
    xml = sdscat(xml, "</doc>\n");

    char *file = create_test_file("excessive_fields.xml", xml);
    sdsfree(xml);
    TEST_ASSERT(file != NULL);

    /* Initialize dataset - should fail when exceeding MAX_DATASET_FIELDS */
    dataset *ds = datasetInit(file, "doc", 1, 0, NULL, 0);

    TEST_ASSERT_MESSAGE("Dataset initialization fails (1001 fields exceeds limit)", ds == NULL);

    cleanup_test_file(file);
    return 0;
}
