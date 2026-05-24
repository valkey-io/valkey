/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstring>

extern "C" {
#include "server.h"
}

class ZsetRangeTest : public ::testing::Test {
};

static void assertScoreRangeParseFails(const char *min, const char *max) {
    robj *minobj = createStringObject(min, strlen(min));
    robj *maxobj = createStringObject(max, strlen(max));
    zrangespec range;

    EXPECT_EQ(zslParseRange(minobj, maxobj, &range), C_ERR) << "min=" << min << " max=" << max;

    decrRefCount(minobj);
    decrRefCount(maxobj);
}

static void assertScoreRangeParseSucceeds(const char *min, const char *max, double expected_min, double expected_max, int expected_minex, int expected_maxex) {
    robj *minobj = createStringObject(min, strlen(min));
    robj *maxobj = createStringObject(max, strlen(max));
    zrangespec range;

    ASSERT_EQ(zslParseRange(minobj, maxobj, &range), C_OK) << "min=" << min << " max=" << max;
    EXPECT_DOUBLE_EQ(range.min, expected_min);
    EXPECT_DOUBLE_EQ(range.max, expected_max);
    EXPECT_EQ(range.minex, expected_minex);
    EXPECT_EQ(range.maxex, expected_maxex);

    decrRefCount(minobj);
    decrRefCount(maxobj);
}

TEST_F(ZsetRangeTest, ParseRangeRejectsEmptyScoreBounds) {
    assertScoreRangeParseFails("", "1");
    assertScoreRangeParseFails("1", "");
}

TEST_F(ZsetRangeTest, ParseRangeRejectsBareExclusiveScoreBounds) {
    assertScoreRangeParseFails("(", "1");
    assertScoreRangeParseFails("1", "(");
}

TEST_F(ZsetRangeTest, ParseRangeAcceptsValidInclusiveAndExclusiveBounds) {
    assertScoreRangeParseSucceeds("0", "1", 0.0, 1.0, 0, 0);
    assertScoreRangeParseSucceeds("(0", "(1", 0.0, 1.0, 1, 1);
}
