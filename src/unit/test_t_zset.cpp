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

    char err_msg[256];
    snprintf(err_msg, sizeof(err_msg), "min=%s max=%s", min, max);
    EXPECT_EQ(zslParseRange(minobj, maxobj, &range), C_ERR) << err_msg;

    decrRefCount(minobj);
    decrRefCount(maxobj);
}

static void assertScoreRangeParseSucceeds(const char *min, const char *max, double expected_min, double expected_max, int expected_minex, int expected_maxex) {
    robj *minobj = createStringObject(min, strlen(min));
    robj *maxobj = createStringObject(max, strlen(max));
    zrangespec range;

    char err_msg[256];
    snprintf(err_msg, sizeof(err_msg), "min=%s max=%s", min, max);
    ASSERT_EQ(zslParseRange(minobj, maxobj, &range), C_OK) << err_msg;
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
    assertScoreRangeParseFails("1.0abc", "1");
    assertScoreRangeParseFails("1", "1.0abc");
    assertScoreRangeParseFails("(1.0abc", "1");
    assertScoreRangeParseFails("1", "(1.0abc");
}

TEST_F(ZsetRangeTest, ParseRangeRejectsBareExclusiveScoreBounds) {
    assertScoreRangeParseFails("(", "1");
    assertScoreRangeParseFails("1", "(");
    assertScoreRangeParseFails("(nan", "1");
    assertScoreRangeParseFails("1", "(nan");
}

TEST_F(ZsetRangeTest, ParseRangeAcceptsValidInclusiveAndExclusiveBounds) {
    assertScoreRangeParseSucceeds("0", "1", 0.0, 1.0, 0, 0);
    assertScoreRangeParseSucceeds("(0", "(1", 0.0, 1.0, 1, 1);
}

TEST_F(ZsetRangeTest, ParseRangeAcceptsInfinityBounds) {
    assertScoreRangeParseSucceeds("-inf", "+inf", -HUGE_VAL, HUGE_VAL, 0, 0);
    assertScoreRangeParseSucceeds("(-inf", "(+inf", -HUGE_VAL, HUGE_VAL, 1, 1);
}
