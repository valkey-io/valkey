/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstring>

extern "C" {
#include "hashtable_helpers.h"
#include "sds.h"
}

class HashtableHelpersTest : public ::testing::Test {};

TEST_F(HashtableHelpersTest, SdsKeyCompareIsBinarySafe) {
    sds a = sdsnewlen("a\0b", 3);
    sds b = sdsnewlen("a\0b", 3);
    sds c = sdsnewlen("a\0c", 3);
    sds d = sdsnewlen("a", 1);
    ASSERT_TRUE(dictSdsKeyCompare(a, b));
    ASSERT_FALSE(dictSdsKeyCompare(a, c));
    ASSERT_FALSE(dictSdsKeyCompare(a, d));
    sdsfree(a);
    sdsfree(b);
    sdsfree(c);
    sdsfree(d);
}

TEST_F(HashtableHelpersTest, CaseCompareAndHash) {
    sds lower = sdsnew("hello");
    sds upper = sdsnew("HELLO");
    ASSERT_TRUE(dictSdsKeyCaseCompare(lower, upper));
    ASSERT_TRUE(dictSdsKeyCaseCompare(lower, "HeLLo"));
    ASSERT_FALSE(dictSdsKeyCaseCompare(lower, "world"));
    ASSERT_EQ(dictSdsCaseHash(lower), dictSdsCaseHash(upper));
    ASSERT_EQ(dictSdsCaseHash(lower), dictCStrCaseHash("HeLLo"));
    ASSERT_EQ(dictSdsHash(lower), dictCStrHash("hello"));
    ASSERT_NE(dictSdsHash(lower), dictSdsHash(upper));
    ASSERT_TRUE(dictCStrKeyCompare("hello", lower));
    ASSERT_FALSE(dictCStrKeyCompare("hello", upper));
    sdsfree(lower);
    sdsfree(upper);
}

TEST_F(HashtableHelpersTest, PointerHash) {
    long slots[2] = {0, 0};
    ASSERT_EQ(hashtablePointerHash(&slots[0]), hashtablePointerHash(&slots[0]));
    ASSERT_NE(hashtablePointerHash(&slots[0]), hashtablePointerHash(&slots[1]));
}

TEST_F(HashtableHelpersTest, ConfigurableSeed) {
    uint8_t saved[16], seed[16];
    memcpy(saved, getConfigurableHashSeed(), sizeof(saved));
    sds key = sdsnew("key");

    memset(seed, 1, sizeof(seed));
    setConfigurableHashSeed(seed);
    ASSERT_EQ(memcmp(getConfigurableHashSeed(), seed, sizeof(seed)), 0);
    uint64_t h1 = sdsHashConfigurableSeed(key);
    ASSERT_EQ(h1, genHashFunctionConfigurableSeed("key", 3));

    memset(seed, 2, sizeof(seed));
    setConfigurableHashSeed(seed);
    ASSERT_NE(h1, sdsHashConfigurableSeed(key));

    setConfigurableHashSeed(saved);
    sdsfree(key);
}
