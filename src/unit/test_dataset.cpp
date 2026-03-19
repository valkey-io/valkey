/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>

extern "C" {
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>
#include "sds.h"
#include "zmalloc.h"
#include "valkey-benchmark-dataset.h"
}

static char *create_test_file(const char *suffix, const char *content) {
    static char filename[512];
    snprintf(filename, sizeof(filename), "/tmp/test_dataset_%d_%s", getpid(), suffix);
    FILE *f = fopen(filename, "w");
    if (!f) return nullptr;
    if (content) fputs(content, f);
    fclose(f);
    return filename;
}

static void cleanup_test_file(const char *filename) {
    if (filename) unlink(filename);
}

TEST(DatasetTest, field_mapping_optimization) {
    const char *csv = "a,b,c,d,e,f,g,h\n1,2,3,4,5,6,7,8\n";
    char *file = create_test_file("mapping.csv", csv);
    ASSERT_NE(file, nullptr);

    sds arg1 = sdsnew("HSET");
    sds arg2 = sdsnew("key");
    sds arg3 = sdsnew("b");
    sds arg4 = sdsnew("__field:b__");
    sds arg5 = sdsnew("f");
    sds arg6 = sdsnew("__field:f__");
    sds args[] = {arg1, arg2, arg3, arg4, arg5, arg6};
    dataset *ds = datasetInit(file, nullptr, 0, 1, args, 6, 0);

    ASSERT_NE(ds, nullptr) << "Dataset initialized";
    ASSERT_EQ(ds->field_count, 8u) << "Total fields discovered";
    ASSERT_EQ(ds->used_field_count, 2u) << "Only used fields loaded";
    ASSERT_STREQ(ds->records[0].fields[0], "2") << "Correct values loaded";
    ASSERT_STREQ(ds->records[0].fields[1], "6") << "Correct values loaded";

    datasetFree(ds);
    sdsfree(arg1);
    sdsfree(arg2);
    sdsfree(arg3);
    sdsfree(arg4);
    sdsfree(arg5);
    sdsfree(arg6);
    cleanup_test_file(file);
}

TEST(DatasetTest, csv_quoted_fields) {
    const char *csv = "id,title,desc\n"
                      "1,\"Book, Part 1\",\"Quote: \"\"Hello\"\"\"\n";
    char *file = create_test_file("quoted.csv", csv);
    ASSERT_NE(file, nullptr);

    sds arg1 = sdsnew("SET");
    sds arg2 = sdsnew("__field:title__");
    sds args[] = {arg1, arg2};
    dataset *ds = datasetInit(file, nullptr, 0, 1, args, 2, 0);

    ASSERT_NE(ds, nullptr) << "Dataset initialized";
    ASSERT_STREQ(ds->records[0].fields[0], "Book, Part 1") << "Quoted comma preserved";

    datasetFree(ds);
    sdsfree(arg1);
    sdsfree(arg2);
    cleanup_test_file(file);
}

TEST(DatasetTest, field_count_correctness) {
    const char *csv = "id,title,author\n1,Book,Alice\n";
    char *file = create_test_file("fields.csv", csv);
    ASSERT_NE(file, nullptr);

    sds arg1 = sdsnew("GET");
    sds arg2 = sdsnew("__field:title__");
    sds args[] = {arg1, arg2};
    dataset *ds = datasetInit(file, nullptr, 0, 1, args, 2, 0);

    ASSERT_NE(ds, nullptr) << "Dataset initialized";
    ASSERT_EQ(ds->field_count, 3u) << "Correct field count";
    ASSERT_EQ(datasetGetRecordCount(ds), 1u) << "Correct record count";

    datasetFree(ds);
    sdsfree(arg1);
    sdsfree(arg2);
    cleanup_test_file(file);
}

TEST(DatasetTest, excessive_field_name_length) {
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
    ASSERT_NE(file, nullptr);

    /* Initialize dataset - only the valid field should be loaded */
    dataset *ds = datasetInit(file, "doc", 1, 0, nullptr, 0, 0);

    ASSERT_NE(ds, nullptr) << "Dataset initialized";
    ASSERT_EQ(ds->field_count, 1u) << "Only valid field loaded (513-char field rejected)";
    ASSERT_STREQ(ds->field_names[0], "title") << "Valid field is 'title'";

    datasetFree(ds);
    cleanup_test_file(file);
}

TEST(DatasetTest, max_dataset_fields) {
    /* Build XML with exactly 1000 fields (MAX_DATASET_FIELDS limit) */
    sds xml = sdsempty();
    xml = sdscat(xml, "<doc>\n");
    for (int i = 0; i < 1000; i++) {
        xml = sdscatprintf(xml, "  <field%d>value%d</field%d>\n", i, i, i);
    }
    xml = sdscat(xml, "</doc>\n");

    char *file = create_test_file("max_fields.xml", xml);
    sdsfree(xml);
    ASSERT_NE(file, nullptr);

    dataset *ds = datasetInit(file, "doc", 1, 0, nullptr, 0, 0);

    ASSERT_NE(ds, nullptr) << "Dataset initialized";
    ASSERT_EQ(ds->field_count, 1000u) << "1000 fields accepted (at MAX_DATASET_FIELDS limit)";
    ASSERT_STREQ(ds->field_names[0], "field0") << "First field correct";
    ASSERT_STREQ(ds->field_names[999], "field999") << "Last field correct";

    datasetFree(ds);
    cleanup_test_file(file);
}

TEST(DatasetTest, excessive_dataset_fields) {
    /* Build XML with 1001 fields (over MAX_DATASET_FIELDS limit) */
    sds xml = sdsempty();
    xml = sdscat(xml, "<doc>\n");
    for (int i = 0; i < 1001; i++) {
        xml = sdscatprintf(xml, "  <field%d>value%d</field%d>\n", i, i, i);
    }
    xml = sdscat(xml, "</doc>\n");

    char *file = create_test_file("excessive_fields.xml", xml);
    sdsfree(xml);
    ASSERT_NE(file, nullptr);

    /* Initialize dataset - should fail when exceeding MAX_DATASET_FIELDS */
    dataset *ds = datasetInit(file, "doc", 1, 0, nullptr, 0, 0);

    ASSERT_EQ(ds, nullptr) << "Dataset initialization fails (1001 fields exceeds limit)";

    cleanup_test_file(file);
}
