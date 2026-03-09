/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <climits>
#include <cstdio>
#include <cstring>

extern "C" {
#include "sds.h"
#include "sdsalloc.h"
#include "util.h"

extern bool large_memory;
}

static sds sdsTestTemplateCallback(const_sds varname, void *arg) {
    UNUSED(arg);
    static const char *_var1 = "variable1";
    static const char *_var2 = "variable2";

    if (!strcmp(varname, _var1))
        return sdsnew("value1");
    else if (!strcmp(varname, _var2))
        return sdsnew("value2");
    else
        return NULL;
}

class SdsTest : public ::testing::Test {}; // Empty fixture for test organization and filtering

TEST_F(SdsTest, TestSds) {
    sds x = sdsnew("foo"), y;

    /* Create a string and obtain the length */
    ASSERT_TRUE(sdslen(x) == 3 && memcmp(x, "foo\0", 4) == 0);

    sdsfree(x);
    x = sdsnewlen("foo", 2);
    /* Create a string with specified length */
    ASSERT_TRUE(sdslen(x) == 2 && memcmp(x, "fo\0", 3) == 0);

    x = sdscat(x, "bar");
    /* Strings concatenation */
    ASSERT_TRUE(sdslen(x) == 5 && memcmp(x, "fobar\0", 6) == 0);

    x = sdscpy(x, "a");
    /* sdscpy() against an originally longer string */
    ASSERT_TRUE(sdslen(x) == 1 && memcmp(x, "a\0", 2) == 0);

    x = sdscpy(x, "xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk");
    /* sdscpy() against an originally shorter string */
    ASSERT_TRUE(sdslen(x) == 33 && memcmp(x, "xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk\0", 33) == 0);

    sdsfree(x);
    x = sdscatprintf(sdsempty(), "%d", 123);
    /* sdscatprintf() seems working in the base case */
    ASSERT_TRUE(sdslen(x) == 3 && memcmp(x, "123\0", 4) == 0);

    sdsfree(x);
    x = sdscatprintf(sdsempty(), "a%cb", 0);
    /* sdscatprintf() seems working with \0 inside of result */
    ASSERT_TRUE(sdslen(x) == 3 && memcmp(x, "a\0"
                                            "b\0",
                                         4) == 0);

    sdsfree(x);
    size_t etalon_size = 1024 * 1024;
    char *etalon = (char *)malloc(etalon_size);
    for (size_t i = 0; i < etalon_size; i++) {
        etalon[i] = '0';
    }
    x = sdscatprintf(sdsempty(), "%0*d", (int)etalon_size, 0);
    /* sdscatprintf() can print 1MB */
    ASSERT_TRUE(sdslen(x) == etalon_size && memcmp(x, etalon, etalon_size) == 0);
    free(etalon);

    sdsfree(x);
    x = sdsnew("--");
    x = sdscatfmt(x, "Hello %s World %I,%I--", "Hi!", LLONG_MIN, LLONG_MAX);
    /* sdscatfmt() seems working in the base case */
    ASSERT_TRUE(sdslen(x) == 60 && memcmp(x, "--Hello Hi! World -9223372036854775808,"
                                             "9223372036854775807--",
                                          60) == 0);

    sdsfree(x);
    x = sdsnew("--");
    x = sdscatfmt(x, "%u,%U--", UINT_MAX, ULLONG_MAX);
    /* sdscatfmt() seems working with unsigned numbers */
    ASSERT_TRUE(sdslen(x) == 35 && memcmp(x, "--4294967295,18446744073709551615--", 35) == 0);

    sdsfree(x);
    x = sdsnew(" x ");
    sdstrim(x, " x");
    /* sdstrim() works when all chars match */
    ASSERT_EQ(sdslen(x), 0u);

    sdsfree(x);
    x = sdsnew(" x ");
    sdstrim(x, " ");
    /* sdstrim() works when a single char remains */
    ASSERT_TRUE(sdslen(x) == 1 && x[0] == 'x');

    sdsfree(x);
    x = sdsnew("xxciaoyyy");
    sdstrim(x, "xy");
    /* sdstrim() correctly trims characters */
    ASSERT_TRUE(sdslen(x) == 4 && memcmp(x, "ciao\0", 5) == 0);

    y = sdsdup(x);
    sdsrange(y, 1, 1);
    /* sdsrange(...,1,1) */
    ASSERT_TRUE(sdslen(y) == 1 && memcmp(y, "i\0", 2) == 0);

    sdsfree(y);
    y = sdsdup(x);
    sdsrange(y, 1, -1);
    /* sdsrange(...,1,-1) */
    ASSERT_TRUE(sdslen(y) == 3 && memcmp(y, "iao\0", 4) == 0);

    sdsfree(y);
    y = sdsdup(x);
    sdsrange(y, -2, -1);
    /* sdsrange(...,-2,-1) */
    ASSERT_TRUE(sdslen(y) == 2 && memcmp(y, "ao\0", 3) == 0);

    sdsfree(y);
    y = sdsdup(x);
    sdsrange(y, 2, 1);
    /* sdsrange(...,2,1) */
    ASSERT_TRUE(sdslen(y) == 0 && memcmp(y, "\0", 1) == 0);

    sdsfree(y);
    y = sdsdup(x);
    sdsrange(y, 1, 100);
    /* sdsrange(...,1,100) */
    ASSERT_TRUE(sdslen(y) == 3 && memcmp(y, "iao\0", 4) == 0);

    sdsfree(y);
    y = sdsdup(x);
    sdsrange(y, 100, 100);
    /* sdsrange(...,100,100) */
    ASSERT_TRUE(sdslen(y) == 0 && memcmp(y, "\0", 1) == 0);

    sdsfree(y);
    y = sdsdup(x);
    sdsrange(y, 4, 6);
    /* sdsrange(...,4,6) */
    ASSERT_TRUE(sdslen(y) == 0 && memcmp(y, "\0", 1) == 0);

    sdsfree(y);
    y = sdsdup(x);
    sdsrange(y, 3, 6);
    /* sdsrange(...,3,6) */
    ASSERT_TRUE(sdslen(y) == 1 && memcmp(y, "o\0", 2) == 0);

    sdsfree(y);
    sdsfree(x);
    x = sdsnew("foo");
    y = sdsnew("foa");
    /* sdscmp(foo,foa) */
    ASSERT_GT(sdscmp(x, y), 0);

    sdsfree(y);
    sdsfree(x);
    x = sdsnew("bar");
    y = sdsnew("bar");
    /* sdscmp(bar,bar) */
    ASSERT_EQ(sdscmp(x, y), 0);

    sdsfree(y);
    sdsfree(x);
    x = sdsnew("aar");
    y = sdsnew("bar");
    /* sdscmp(bar,bar) */
    ASSERT_LT(sdscmp(x, y), 0);

    sdsfree(y);
    sdsfree(x);
    x = sdsnewlen("\a\n\0foo\r", 7);
    y = sdscatrepr(sdsempty(), x, sdslen(x));
    /* sdscatrepr(...data...) */
    ASSERT_EQ(memcmp(y, "\"\\a\\n\\x00foo\\r\"", 15), 0);

    unsigned int oldfree;
    char *p;
    int i;
    size_t step = 10, j;

    sdsfree(x);
    sdsfree(y);
    x = sdsnew("0");
    /* sdsnew() free/len buffers */
    ASSERT_TRUE(sdslen(x) == 1 && sdsavail(x) == 0);

    /* Run the test a few times in order to hit the first two
     * SDS header types. */
    for (i = 0; i < 10; i++) {
        size_t oldlen = sdslen(x);
        x = sdsMakeRoomFor(x, step);
        int type = x[-1] & SDS_TYPE_MASK;

        /* sdsMakeRoomFor() len */
        ASSERT_EQ(sdslen(x), oldlen);
        if (type != SDS_TYPE_5) {
            /* sdsMakeRoomFor() free */
            ASSERT_GE(sdsavail(x), step);
            oldfree = sdsavail(x);
            UNUSED(oldfree);
        }
        p = x + oldlen;
        for (j = 0; j < step; j++) {
            p[j] = 'A' + j;
        }
        sdsIncrLen(x, step);
    }
    /* sdsMakeRoomFor() content */
    ASSERT_EQ(memcmp("0ABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGH"
                     "IJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJ",
                     x, 101),
              0);
    /* sdsMakeRoomFor() final length */
    ASSERT_EQ(sdslen(x), 101u);

    sdsfree(x);

    /* Simple template */
    x = sdstemplate("v1={variable1} v2={variable2}", sdsTestTemplateCallback, NULL);
    /* sdstemplate() normal flow */
    ASSERT_EQ(memcmp(x, "v1=value1 v2=value2", 19), 0);
    sdsfree(x);

    /* Template with callback error */
    x = sdstemplate("v1={variable1} v3={doesnotexist}", sdsTestTemplateCallback, NULL);
    /* sdstemplate() with callback error */
    ASSERT_EQ(x, nullptr);

    /* Template with empty var name */
    x = sdstemplate("v1={", sdsTestTemplateCallback, NULL);
    /* sdstemplate() with empty var name */
    ASSERT_EQ(x, nullptr);

    /* Template with truncated var name */
    x = sdstemplate("v1={start", sdsTestTemplateCallback, NULL);
    /* sdstemplate() with truncated var name */
    ASSERT_EQ(x, nullptr);

    /* Template with quoting */
    x = sdstemplate("v1={{{variable1}} {{} v2={variable2}", sdsTestTemplateCallback, NULL);
    /* sdstemplate() with quoting */
    ASSERT_EQ(memcmp(x, "v1={value1} {} v2=value2", 24), 0);
    sdsfree(x);

    /* Test sdsResize - extend */
    x = sdsnew("1234567890123456789012345678901234567890");
    x = sdsResize(x, 200, 1);
    /* sdsResize() expand type */
    ASSERT_EQ(x[-1], SDS_TYPE_8);
    /* sdsResize() expand len */
    ASSERT_EQ(sdslen(x), 40u);
    /* sdsResize() expand strlen */
    ASSERT_EQ(strlen(x), 40u);
    /* Different allocator allocates at least as large as requested size,
     * to confirm the allocator won't waste too much,
     * we add a largest size checker here. */
    /* sdsResize() expand alloc */
    ASSERT_TRUE(sdsalloc(x) >= 200 && sdsalloc(x) < 400);

    /* Test sdsResize - trim free space */
    x = sdsResize(x, 80, 1);
    /* sdsResize() shrink type */
    ASSERT_EQ(x[-1], SDS_TYPE_8);
    /* sdsResize() shrink len */
    ASSERT_EQ(sdslen(x), 40u);
    /* sdsResize() shrink strlen */
    ASSERT_EQ(strlen(x), 40u);
    /* sdsResize() shrink alloc */
    ASSERT_GE(sdsalloc(x), 80u);

    /* Test sdsResize - crop used space */
    x = sdsResize(x, 30, 1);
    /* sdsResize() crop type */
    ASSERT_EQ(x[-1], SDS_TYPE_8);
    /* sdsResize() crop len */
    ASSERT_EQ(sdslen(x), 30u);
    /* sdsResize() crop strlen */
    ASSERT_EQ(strlen(x), 30u);
    /* sdsResize() crop alloc */
    ASSERT_GE(sdsalloc(x), 30u);

    /* Test sdsResize - extend to different class */
    x = sdsResize(x, 400, 1);
    /* sdsResize() expand type */
    ASSERT_EQ(x[-1], SDS_TYPE_16);
    /* sdsResize() expand len */
    ASSERT_EQ(sdslen(x), 30u);
    /* sdsResize() expand strlen */
    ASSERT_EQ(strlen(x), 30u);
    /* sdsResize() expand alloc */
    ASSERT_GE(sdsalloc(x), 400u);

    /* Test sdsResize - shrink to different class */
    x = sdsResize(x, 4, 1);
    /* sdsResize() crop type */
    ASSERT_EQ(x[-1], SDS_TYPE_8);
    /* sdsResize() crop len */
    ASSERT_EQ(sdslen(x), 4u);
    /* sdsResize() crop strlen */
    ASSERT_EQ(strlen(x), 4u);
    /* sdsResize() crop alloc */
    ASSERT_GE(sdsalloc(x), 4u);
    sdsfree(x);
}

TEST_F(SdsTest, TestTypesAndAllocSize) {
    sds x = sdsnewlen(NULL, 31);
    /* len 31 type */
    ASSERT_EQ((x[-1] & SDS_TYPE_MASK), SDS_TYPE_5);
    /* len 31 sdsAllocSize */
    ASSERT_EQ(sdsAllocSize(x), s_malloc_usable_size(sdsAllocPtr(x)));
    sdsfree(x);

    x = sdsnewlen(NULL, 32);
    /* len 32 type */
    ASSERT_GE((x[-1] & SDS_TYPE_MASK), SDS_TYPE_8);
    /* len 32 sdsAllocSize */
    ASSERT_EQ(sdsAllocSize(x), s_malloc_usable_size(sdsAllocPtr(x)));
    sdsfree(x);

    x = sdsnewlen(NULL, 252);
    /* len 252 type */
    ASSERT_GE((x[-1] & SDS_TYPE_MASK), SDS_TYPE_8);
    /* len 252 sdsAllocSize */
    ASSERT_EQ(sdsAllocSize(x), s_malloc_usable_size(sdsAllocPtr(x)));
    sdsfree(x);

    x = sdsnewlen(NULL, 253);
    /* len 253 type */
    ASSERT_EQ((x[-1] & SDS_TYPE_MASK), SDS_TYPE_16);
    /* len 253 sdsAllocSize */
    ASSERT_EQ(sdsAllocSize(x), s_malloc_usable_size(sdsAllocPtr(x)));
    sdsfree(x);

    x = sdsnewlen(NULL, 65530);
    /* len 65530 type */
    ASSERT_GE((x[-1] & SDS_TYPE_MASK), SDS_TYPE_16);
    /* len 65530 sdsAllocSize */
    ASSERT_EQ(sdsAllocSize(x), s_malloc_usable_size(sdsAllocPtr(x)));
    sdsfree(x);

    x = sdsnewlen(NULL, 65531);
    /* len 65531 type */
    ASSERT_GE((x[-1] & SDS_TYPE_MASK), SDS_TYPE_32);
    /* len 65531 sdsAllocSize */
    ASSERT_EQ(sdsAllocSize(x), s_malloc_usable_size(sdsAllocPtr(x)));
    sdsfree(x);

#if (LONG_MAX == LLONG_MAX)
    if (large_memory) {
        x = sdsnewlen(NULL, 4294967286);
        /* len 4294967286 type */
        ASSERT_GE((x[-1] & SDS_TYPE_MASK), SDS_TYPE_32);
        /* len 4294967286 sdsAllocSize */
        ASSERT_EQ(sdsAllocSize(x), s_malloc_usable_size(sdsAllocPtr(x)));
        sdsfree(x);

        x = sdsnewlen(NULL, 4294967287);
        /* len 4294967287 type */
        ASSERT_EQ((x[-1] & SDS_TYPE_MASK), SDS_TYPE_64);
        /* len 4294967287 sdsAllocSize */
        ASSERT_EQ(sdsAllocSize(x), s_malloc_usable_size(sdsAllocPtr(x)));
        sdsfree(x);
    }
#endif
}

/* The test verifies that we can adjust SDS types if an allocator returned
 * larger buffer. The maximum length for type SDS_TYPE_X is
 * 2^X - header_size(SDS_TYPE_X) - 1. The maximum value to be stored in alloc
 * field is 2^X - 1. When allocated buffer is larger than
 * 2^X + header_size(SDS_TYPE_X), we "move" to a larger type SDS_TYPE_Y. To be
 * sure SDS_TYPE_Y header fits into 2^X + header_size(SDS_TYPE_X) + 1 bytes, the
 * difference between header sizes must be smaller than
 * header_size(SDS_TYPE_X) + 1.
 * We ignore SDS_TYPE_5 as it doesn't have alloc field. */
TEST_F(SdsTest, TestSdsHeaderSizes) {
    /* can't always adjust SDS_TYPE_8 with SDS_TYPE_16 */
    ASSERT_LE(sizeof(struct sdshdr16), 2 * sizeof(struct sdshdr8) + 1);
    /* can't always adjust SDS_TYPE_16 with SDS_TYPE_32 */
    ASSERT_LE(sizeof(struct sdshdr32), 2 * sizeof(struct sdshdr16) + 1);
#if (LONG_MAX == LLONG_MAX)
    /* can't always adjust SDS_TYPE_32 with SDS_TYPE_64 */
    ASSERT_LE(sizeof(struct sdshdr64), 2 * sizeof(struct sdshdr32) + 1);
#endif
}

TEST_F(SdsTest, TestSdssplitargs) {
    int len;
    sds *sargv;

    sargv = sdssplitargs("Testing one two three", &len);
    ASSERT_EQ(4, len);
    ASSERT_TRUE(!strcmp("Testing", sargv[0]));
    ASSERT_TRUE(!strcmp("one", sargv[1]));
    ASSERT_TRUE(!strcmp("two", sargv[2]));
    ASSERT_TRUE(!strcmp("three", sargv[3]));
    sdsfreesplitres(sargv, len);

    sargv = sdssplitargs("", &len);
    ASSERT_EQ(0, len);
    ASSERT_NE(sargv, nullptr);
    sdsfreesplitres(sargv, len);

    sargv = sdssplitargs("\"Testing split strings\" 'Another split string'", &len);
    ASSERT_EQ(2, len);
    ASSERT_TRUE(!strcmp("Testing split strings", sargv[0]));
    ASSERT_TRUE(!strcmp("Another split string", sargv[1]));
    sdsfreesplitres(sargv, len);

    sargv = sdssplitargs("\"Hello\" ", &len);
    ASSERT_EQ(1, len);
    ASSERT_TRUE(!strcmp("Hello", sargv[0]));
    sdsfreesplitres(sargv, len);

    const char *binary_string = "\"\\x73\\x75\\x70\\x65\\x72\\x20\\x00\\x73\\x65\\x63\\x72\\x65\\x74\\x20\\x70\\x61\\x73\\x73\\x77\\x6f\\x72\\x64\"";
    sargv = sdssplitargs(binary_string, &len);
    ASSERT_EQ(1, len);
    ASSERT_TRUE(!strcmp("super \x00secret password", sargv[0]));
    sdsfreesplitres(sargv, len);

    sargv = sdssplitargs("unquoted", &len);
    ASSERT_EQ(1, len);
    ASSERT_TRUE(!strcmp("unquoted", sargv[0]));
    sdsfreesplitres(sargv, len);

    sargv = sdssplitargs("empty string \"\"", &len);
    ASSERT_EQ(3, len);
    ASSERT_TRUE(!strcmp("empty", sargv[0]));
    ASSERT_TRUE(!strcmp("string", sargv[1]));
    ASSERT_TRUE(!strcmp("", sargv[2]));
    sdsfreesplitres(sargv, len);

    sargv = sdssplitargs("\"deeply\\\"quoted\" 's\\'t\\\"r'ing", &len);
    ASSERT_EQ(2, len);
    ASSERT_TRUE(!strcmp("deeply\"quoted", sargv[0]));
    ASSERT_TRUE(!strcmp("s't\\\"ring", sargv[1]));
    sdsfreesplitres(sargv, len);

    sargv = sdssplitargs("unquoted\" \"with' 'quotes string", &len);
    ASSERT_EQ(2, len);
    ASSERT_TRUE(!strcmp("unquoted with quotes", sargv[0]));
    ASSERT_TRUE(!strcmp("string", sargv[1]));
    sdsfreesplitres(sargv, len);

    sargv = sdssplitargs("\"quoted\"' another 'quoted string", &len);
    ASSERT_EQ(2, len);
    ASSERT_TRUE(!strcmp("quoted another quoted", sargv[0]));
    ASSERT_TRUE(!strcmp("string", sargv[1]));
    sdsfreesplitres(sargv, len);

    sargv = sdssplitargs("\"shell-like \"'\"'\"'\"' quote-escaping '", &len);
    ASSERT_EQ(1, len);
    ASSERT_TRUE(!strcmp("shell-like \"' quote-escaping ", sargv[0]));
    sdsfreesplitres(sargv, len);

    sargv = sdssplitargs("\"unterminated \"'single quotes", &len);
    ASSERT_EQ(0, len);
    ASSERT_EQ(sargv, nullptr);

    sargv = sdssplitargs("'unterminated '\"double quotes", &len);
    ASSERT_EQ(0, len);
    ASSERT_EQ(sargv, nullptr);
}


TEST_F(SdsTest, TestSdsnsplitargs) {
    int len;
    sds *sargv;
    const char *test_str;

    // Test basic parameter splitting
    test_str = "Testing one two three";
    sargv = sdsnsplitargs(test_str, strlen(test_str), &len);
    ASSERT_EQ(4, len);
    ASSERT_TRUE(!strcmp("Testing", sargv[0]));
    ASSERT_TRUE(!strcmp("one", sargv[1]));
    ASSERT_TRUE(!strcmp("two", sargv[2]));
    ASSERT_TRUE(!strcmp("three", sargv[3]));
    sdsfreesplitres(sargv, len);

    // Test empty string
    sargv = sdsnsplitargs("", 0, &len);
    ASSERT_EQ(0, len);
    ASSERT_NE(sargv, nullptr);
    sdsfreesplitres(sargv, len);

    // Test quoted strings
    test_str = "\"Testing split strings\" 'Another split string'";
    sargv = sdsnsplitargs(test_str, strlen(test_str), &len);
    ASSERT_EQ(2, len);
    ASSERT_TRUE(!strcmp("Testing split strings", sargv[0]));
    ASSERT_TRUE(!strcmp("Another split string", sargv[1]));
    sdsfreesplitres(sargv, len);

    // Test trailing space after quoted string
    test_str = "\"Hello\" ";
    sargv = sdsnsplitargs(test_str, strlen(test_str), &len);
    ASSERT_EQ(1, len);
    ASSERT_TRUE(!strcmp("Hello", sargv[0]));
    sdsfreesplitres(sargv, len);

    // Test binary string with null character using \x escape
    test_str = "\"\\x73\\x75\\x70\\x65\\x72\\x20\\x00\\x73\\x65\\x63\\x72\\x65\\x74\\x20\\x70\\x61\\x73\\x73\\x77\\x6f\\x72\\x64\"";
    sargv = sdsnsplitargs(test_str, strlen(test_str), &len);
    ASSERT_EQ(1, len);
    ASSERT_TRUE(!strcmp("super \x00secret password", sargv[0]));
    sdsfreesplitres(sargv, len);

    char str_with_null[] = "test\0null";
    sargv = sdsnsplitargs(str_with_null, sizeof(str_with_null) - 1, &len);
    ASSERT_EQ(1, len);
    ASSERT_TRUE(!memcmp("test", sargv[0], 4));
    sdsfreesplitres(sargv, len);

    // Test single unquoted string
    test_str = "unquoted";
    sargv = sdsnsplitargs(test_str, strlen(test_str), &len);
    ASSERT_EQ(1, len);
    ASSERT_TRUE(!strcmp("unquoted", sargv[0]));
    sdsfreesplitres(sargv, len);

    // Test empty quoted string
    test_str = "empty string \"\"";
    sargv = sdsnsplitargs(test_str, strlen(test_str), &len);
    ASSERT_EQ(3, len);
    ASSERT_TRUE(!strcmp("empty", sargv[0]));
    ASSERT_TRUE(!strcmp("string", sargv[1]));
    ASSERT_TRUE(!strcmp("", sargv[2]));
    sdsfreesplitres(sargv, len);

    // Test escaped quotes
    test_str = "\"deeply\\\"quoted\" 's\\'t\\\"r'ing";
    sargv = sdsnsplitargs(test_str, strlen(test_str), &len);
    ASSERT_EQ(2, len);
    ASSERT_TRUE(!strcmp("deeply\"quoted", sargv[0]));
    ASSERT_TRUE(!strcmp("s't\\\"ring", sargv[1]));
    sdsfreesplitres(sargv, len);

    // Test mixed quoted and unquoted parts
    test_str = "unquoted\" \"with' 'quotes string";
    sargv = sdsnsplitargs(test_str, strlen(test_str), &len);
    ASSERT_EQ(2, len);
    ASSERT_TRUE(!strcmp("unquoted with quotes", sargv[0]));
    ASSERT_TRUE(!strcmp("string", sargv[1]));
    sdsfreesplitres(sargv, len);

    // Test concatenated quoted strings
    test_str = "\"quoted\"' another 'quoted string";
    sargv = sdsnsplitargs(test_str, strlen(test_str), &len);
    ASSERT_EQ(2, len);
    ASSERT_TRUE(!strcmp("quoted another quoted", sargv[0]));
    ASSERT_TRUE(!strcmp("string", sargv[1]));
    sdsfreesplitres(sargv, len);

    // Test complex quote escaping
    test_str = "\"shell-like \"'\"'\"'\"' quote-escaping '";
    sargv = sdsnsplitargs(test_str, strlen(test_str), &len);
    ASSERT_EQ(1, len);
    ASSERT_TRUE(!strcmp("shell-like \"' quote-escaping ", sargv[0]));
    sdsfreesplitres(sargv, len);

    // Test unterminated double quote
    test_str = "\"unterminated \"'single quotes";
    sargv = sdsnsplitargs(test_str, strlen(test_str), &len);
    ASSERT_EQ(0, len);
    ASSERT_EQ(sargv, nullptr);

    // Test unterminated single quote
    test_str = "'unterminated '\"double quotes";
    sargv = sdsnsplitargs(test_str, strlen(test_str), &len);
    ASSERT_EQ(0, len);
    ASSERT_EQ(sargv, nullptr);

    // Test partial string length (truncated input)
    test_str = "Testing one two three";
    sargv = sdsnsplitargs(test_str, 8, &len);
    ASSERT_EQ(1, len);
    ASSERT_TRUE(!strcmp("Testing", sargv[0]));
    sdsfreesplitres(sargv, len);

    // Test string with exact length (no truncation)
    test_str = "Exact length";
    sargv = sdsnsplitargs(test_str, strlen(test_str), &len);
    ASSERT_EQ(2, len);
    ASSERT_TRUE(!strcmp("Exact", sargv[0]));
    ASSERT_TRUE(!strcmp("length", sargv[1]));
    sdsfreesplitres(sargv, len);

    // Test string with leading spaces
    test_str = "   leading spaces";
    sargv = sdsnsplitargs(test_str, strlen(test_str), &len);
    ASSERT_EQ(2, len);
    ASSERT_TRUE(!strcmp("leading", sargv[0]));
    ASSERT_TRUE(!strcmp("spaces", sargv[1]));
    sdsfreesplitres(sargv, len);

    // Test string with trailing spaces
    test_str = "trailing spaces   ";
    sargv = sdsnsplitargs(test_str, strlen(test_str), &len);
    ASSERT_EQ(2, len);
    ASSERT_TRUE(!strcmp("trailing", sargv[0]));
    ASSERT_TRUE(!strcmp("spaces", sargv[1]));
    sdsfreesplitres(sargv, len);

    // Test string with consecutive spaces
    test_str = "multiple   spaces";
    sargv = sdsnsplitargs(test_str, strlen(test_str), &len);
    ASSERT_EQ(2, len);
    ASSERT_TRUE(!strcmp("multiple", sargv[0]));
    ASSERT_TRUE(!strcmp("spaces", sargv[1]));
    sdsfreesplitres(sargv, len);

    // Test string with only spaces
    test_str = "   ";
    sargv = sdsnsplitargs(test_str, strlen(test_str), &len);
    ASSERT_EQ(0, len);
    ASSERT_NE(sargv, nullptr);
    sdsfreesplitres(sargv, len);

    // Test string containing null character in the middle of parsing
    char str_with_null_in_middle[] = "arg1\0arg2 arg3";
    sargv = sdsnsplitargs(str_with_null_in_middle, sizeof(str_with_null_in_middle) - 1, &len);
    ASSERT_EQ(1, len);
    ASSERT_TRUE(!memcmp("arg1", sargv[0], 4));
    sdsfreesplitres(sargv, len);

    // Test very long single argument
    char long_arg[1024];
    memset(long_arg, 'a', sizeof(long_arg) - 1);
    long_arg[sizeof(long_arg) - 1] = '\0';
    sargv = sdsnsplitargs(long_arg, sizeof(long_arg) - 1, &len);
    ASSERT_EQ(1, len);
    ASSERT_EQ(strlen(sargv[0]), sizeof(long_arg) - 1);
    ASSERT_TRUE(!memcmp(long_arg, sargv[0], sizeof(long_arg) - 1));
    sdsfreesplitres(sargv, len);

    // Test mixed quote types in one argument
    test_str = "\"double'quotes\" 'single\"quotes'";
    sargv = sdsnsplitargs(test_str, strlen(test_str), &len);
    ASSERT_EQ(2, len);
    ASSERT_TRUE(!strcmp("double'quotes", sargv[0]));
    ASSERT_TRUE(!strcmp("single\"quotes", sargv[1]));
    sdsfreesplitres(sargv, len);

    // Test mixed quote types with different lengths
    sds complex_str = sdsnew("\"double'quotes\" 'single\"quotes'");
    sargv = sdsnsplitargs(complex_str, sdslen(complex_str) - 1, &len);
    ASSERT_EQ(0, len);

    complex_str = sdscatlen(complex_str, "\0", 1);
    sargv = sdsnsplitargs(complex_str, sdslen(complex_str) - 1, &len);
    ASSERT_EQ(2, len);
    ASSERT_TRUE(!strcmp("double'quotes", sargv[0]));
    ASSERT_TRUE(!strcmp("single\"quotes", sargv[1]));
    sdsfreesplitres(sargv, len);

    sargv = sdsnsplitargs(complex_str, sdslen(complex_str), &len);
    ASSERT_EQ(2, len);
    ASSERT_TRUE(!strcmp("double'quotes", sargv[0]));
    ASSERT_TRUE(!strcmp("single\"quotes", sargv[1]));
    sdsfreesplitres(sargv, len);

    sargv = sdsnsplitargs(complex_str, sdslen(complex_str) - 2, &len);
    ASSERT_EQ(0, len);
    sdsfree(complex_str);
}

/* This test is disabled by default because it takes a long time to run (1M iterations).
 * It's used for performance comparison between sdsnsplitargs and sdssplitargs.
 * To run this test explicitly, use:
 *   ./src/unit/valkey-unit-gtests --gtest_filter=SdsTest.DISABLED_TestSdsnsplitargsBenchmark --gtest_also_run_disabled_tests */
TEST_F(SdsTest, DISABLED_TestSdsnsplitargsBenchmark) {
    char str_with_null_in_middle[] = "arg1\0arg2 arg3";
    size_t str_len = sizeof(str_with_null_in_middle) - 1;
    int len = 0;

    long long start = ustime();
    for (int i = 0; i < 1000000; i++) {
        sds *sargv = sdsnsplitargs(str_with_null_in_middle, str_len, &len);
        sdsfreesplitres(sargv, len);
    }
    printf("sdsnsplitargs 1000000 times: %f\n", (double)(ustime() - start) / 1000000);

    start = ustime();
    for (int i = 0; i < 1000000; i++) {
        sds str = sdsnewlen(str_with_null_in_middle, str_len);
        sds *sargv = sdssplitargs(str, &len);
        sdsfreesplitres(sargv, len);
        sdsfree(str);
    }
    printf("sdssplitargs 1000000 times: %f\n", (double)(ustime() - start) / 1000000);
}
