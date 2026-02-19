/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/time.h>

extern "C" {
#include "intset.h"

/* Wrapper function declarations for accessing static intset.c internals */
uint8_t testOnlyIntsetValueEncoding(int64_t v);
int64_t testOnlyIntsetGetEncoded(intset *is, int pos, uint8_t enc);
int64_t testOnlyIntsetGet(intset *is, int pos);
uint8_t testOnlyIntsetSearch(intset *is, int64_t value, uint32_t *pos);
}

/* Macros from intset.c needed for testing */
#define INTSET_ENC_INT16 (sizeof(int16_t))
#define INTSET_ENC_INT32 (sizeof(int32_t))
#define INTSET_ENC_INT64 (sizeof(int64_t))

static long long usec(void) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return ((long long)tv.tv_sec * 1000000) + tv.tv_usec;
}

static intset *createSet(int bits, int size) {
    uint64_t mask = (1 << bits) - 1;
    uint64_t value;
    intset *is = intsetNew();

    for (int i = 0; i < size; i++) {
        if (bits > 32) {
            value = (rand() * rand()) & mask;
        } else {
            value = rand() & mask;
        }
        is = intsetAdd(is, value, nullptr);
    }
    return is;
}

/* Use memcpy to avoid strict aliasing violations and potential alignment issues. */
static int checkConsistency(intset *is) {
    for (uint32_t i = 0; i < (intrev32ifbe(is->length) - 1); i++) {
        uint32_t encoding = intrev32ifbe(is->encoding);

        if (encoding == INTSET_ENC_INT16) {
            int16_t v1, v2;
            memcpy(&v1, is->contents + i * sizeof(int16_t), sizeof(int16_t));
            memcpy(&v2, is->contents + (i + 1) * sizeof(int16_t), sizeof(int16_t));
            if (v1 >= v2) return 0;
        } else if (encoding == INTSET_ENC_INT32) {
            int32_t v1, v2;
            memcpy(&v1, is->contents + i * sizeof(int32_t), sizeof(int32_t));
            memcpy(&v2, is->contents + (i + 1) * sizeof(int32_t), sizeof(int32_t));
            if (v1 >= v2) return 0;
        } else {
            int64_t v1, v2;
            memcpy(&v1, is->contents + i * sizeof(int64_t), sizeof(int64_t));
            memcpy(&v2, is->contents + (i + 1) * sizeof(int64_t), sizeof(int64_t));
            if (v1 >= v2) return 0;
        }
    }
    return 1;
}

class IntsetTest : public ::testing::Test {};

TEST_F(IntsetTest, TestIntsetValueEncodings) {
    ASSERT_EQ(testOnlyIntsetValueEncoding(-32768), INTSET_ENC_INT16);
    ASSERT_EQ(testOnlyIntsetValueEncoding(+32767), INTSET_ENC_INT16);
    ASSERT_EQ(testOnlyIntsetValueEncoding(-32769), INTSET_ENC_INT32);
    ASSERT_EQ(testOnlyIntsetValueEncoding(+32768), INTSET_ENC_INT32);
    ASSERT_EQ(testOnlyIntsetValueEncoding(-2147483648), INTSET_ENC_INT32);
    ASSERT_EQ(testOnlyIntsetValueEncoding(+2147483647), INTSET_ENC_INT32);
    ASSERT_EQ(testOnlyIntsetValueEncoding(-2147483649), INTSET_ENC_INT64);
    ASSERT_EQ(testOnlyIntsetValueEncoding(+2147483648), INTSET_ENC_INT64);
    ASSERT_EQ(testOnlyIntsetValueEncoding(-9223372036854775808ull), INTSET_ENC_INT64);
    ASSERT_EQ(testOnlyIntsetValueEncoding(+9223372036854775807ull), INTSET_ENC_INT64);
}

TEST_F(IntsetTest, TestIntsetBasicAdding) {
    intset *is = intsetNew();
    uint8_t success;
    is = intsetAdd(is, 5, &success);
    ASSERT_TRUE(success);
    is = intsetAdd(is, 6, &success);
    ASSERT_TRUE(success);
    is = intsetAdd(is, 4, &success);
    ASSERT_TRUE(success);
    is = intsetAdd(is, 4, &success);
    ASSERT_FALSE(success);
    ASSERT_EQ(6, intsetMax(is));
    ASSERT_EQ(4, intsetMin(is));
    zfree(is);
}

TEST_F(IntsetTest, TestIntsetLargeNumberRandomAdd) {
    uint32_t inserts = 0;
    uint8_t success;
    intset *is = intsetNew();
    for (int i = 0; i < 1024; i++) {
        is = intsetAdd(is, rand() % 0x800, &success);
        if (success) inserts++;
    }
    ASSERT_EQ(intrev32ifbe(is->length), inserts);
    ASSERT_EQ(checkConsistency(is), 1);
    zfree(is);
}

TEST_F(IntsetTest, TestIntsetUpgradeFromint16Toint32) {
    intset *is = intsetNew();
    is = intsetAdd(is, 32, nullptr);
    ASSERT_EQ(intrev32ifbe(is->encoding), INTSET_ENC_INT16);
    is = intsetAdd(is, 65535, nullptr);
    ASSERT_EQ(intrev32ifbe(is->encoding), INTSET_ENC_INT32);
    ASSERT_TRUE(intsetFind(is, 32));
    ASSERT_TRUE(intsetFind(is, 65535));
    ASSERT_EQ(checkConsistency(is), 1);
    zfree(is);

    is = intsetNew();
    is = intsetAdd(is, 32, nullptr);
    ASSERT_EQ(intrev32ifbe(is->encoding), INTSET_ENC_INT16);
    is = intsetAdd(is, -65535, nullptr);
    ASSERT_EQ(intrev32ifbe(is->encoding), INTSET_ENC_INT32);
    ASSERT_TRUE(intsetFind(is, 32));
    ASSERT_TRUE(intsetFind(is, -65535));
    ASSERT_EQ(checkConsistency(is), 1);
    zfree(is);
}

TEST_F(IntsetTest, TestIntsetUpgradeFromint16Toint64) {
    intset *is = intsetNew();
    is = intsetAdd(is, 32, nullptr);
    ASSERT_EQ(intrev32ifbe(is->encoding), INTSET_ENC_INT16);
    is = intsetAdd(is, 4294967295, nullptr);
    ASSERT_EQ(intrev32ifbe(is->encoding), INTSET_ENC_INT64);
    ASSERT_TRUE(intsetFind(is, 32));
    ASSERT_TRUE(intsetFind(is, 4294967295));
    ASSERT_EQ(checkConsistency(is), 1);
    zfree(is);

    is = intsetNew();
    is = intsetAdd(is, 32, nullptr);
    ASSERT_EQ(intrev32ifbe(is->encoding), INTSET_ENC_INT16);
    is = intsetAdd(is, -4294967295, nullptr);
    ASSERT_EQ(intrev32ifbe(is->encoding), INTSET_ENC_INT64);
    ASSERT_TRUE(intsetFind(is, 32));
    ASSERT_TRUE(intsetFind(is, -4294967295));
    ASSERT_EQ(checkConsistency(is), 1);
    zfree(is);
}

TEST_F(IntsetTest, TestIntsetUpgradeFromint32Toint64) {
    intset *is = intsetNew();
    is = intsetAdd(is, 65535, nullptr);
    ASSERT_EQ(intrev32ifbe(is->encoding), INTSET_ENC_INT32);
    is = intsetAdd(is, 4294967295, nullptr);
    ASSERT_EQ(intrev32ifbe(is->encoding), INTSET_ENC_INT64);
    ASSERT_TRUE(intsetFind(is, 65535));
    ASSERT_TRUE(intsetFind(is, 4294967295));
    ASSERT_EQ(checkConsistency(is), 1);
    zfree(is);

    is = intsetNew();
    is = intsetAdd(is, 65535, nullptr);
    ASSERT_EQ(intrev32ifbe(is->encoding), INTSET_ENC_INT32);
    is = intsetAdd(is, -4294967295, nullptr);
    ASSERT_EQ(intrev32ifbe(is->encoding), INTSET_ENC_INT64);
    ASSERT_TRUE(intsetFind(is, 65535));
    ASSERT_TRUE(intsetFind(is, -4294967295));
    ASSERT_EQ(checkConsistency(is), 1);
    zfree(is);
}

TEST_F(IntsetTest, TestIntsetStressLookups) {
    long num = 100000, size = 10000;
    int i, bits = 20;
    long long start;
    intset *is = createSet(bits, size);
    ASSERT_EQ(checkConsistency(is), 1);

    start = usec();
    for (i = 0; i < num; i++) testOnlyIntsetSearch(is, rand() % ((1 << bits) - 1), nullptr);
    printf("%ld lookups, %ld element set, %lldusec\n", num, size, usec() - start);
    zfree(is);
}

TEST_F(IntsetTest, TestIntsetStressAddDelete) {
    int i, v1, v2;
    intset *is = intsetNew();
    for (i = 0; i < 0xffff; i++) {
        v1 = rand() % 0xfff;
        is = intsetAdd(is, v1, nullptr);
        ASSERT_TRUE(intsetFind(is, v1));

        v2 = rand() % 0xfff;
        is = intsetRemove(is, v2, nullptr);
        ASSERT_FALSE(intsetFind(is, v2));
    }
    ASSERT_EQ(checkConsistency(is), 1);
    zfree(is);
}
