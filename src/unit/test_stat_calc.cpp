/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Unit tests for stat_calc.h.
 */

#include "generated_wrappers.hpp"

extern "C" {
#include "stat_calc.h"
static monotime fakeGetMonotonicUs(void);
static monotime (*origGetMonotonicUs)(void);
}

static const long ONE_SECOND_IN_MICROS = 1000000;

static monotime fakeMonotimeUs;

static monotime fakeGetMonotonicUs(void) {
    return fakeMonotimeUs;
}

class StatCalcTest : public ::testing::Test {
  protected:
    tpsCalculator *tps;
    trendCalculator *trend;

    static void SetUpTestSuite() {
        origGetMonotonicUs = getMonotonicUs;
        getMonotonicUs = fakeGetMonotonicUs;
    }

    static void TearDownTestSuite() {
        getMonotonicUs = origGetMonotonicUs;
    }

    void SetUp() override {
        fakeMonotimeUs = 100;
        tps = tpsCalculator_create(5);
        trend = trendCalculator_create(5);
    }

    void TearDown() override {
        tpsCalculator_free(tps);
        trendCalculator_free(trend);
    }
};

/* ========================== TPS Calculator Tests ========================== */

TEST_F(StatCalcTest, TpsInitZero) {
    EXPECT_DOUBLE_EQ(tpsCalculator_averageTps(tps), 0.0);
}

TEST_F(StatCalcTest, TpsExtrapolateFromOneSecond) {
    /* 1 second of data at 10 transactions, TPS should be 10 */
    fakeMonotimeUs += ONE_SECOND_IN_MICROS;
    tpsCalculator_record(tps, 10);
    EXPECT_DOUBLE_EQ(tpsCalculator_averageTps(tps), 10.0);
}

TEST_F(StatCalcTest, TpsInterpolateFromTenSeconds) {
    /* 10 seconds of data at 10 transactions, TPS should be 1 */
    fakeMonotimeUs += 10 * ONE_SECOND_IN_MICROS;
    tpsCalculator_record(tps, 10);
    EXPECT_DOUBLE_EQ(tpsCalculator_averageTps(tps), 1.0);
}

TEST_F(StatCalcTest, TpsSuddenIncrease) {
    /* Initialize at 10/sec */
    fakeMonotimeUs += ONE_SECOND_IN_MICROS;
    tpsCalculator_record(tps, 10);
    EXPECT_DOUBLE_EQ(tpsCalculator_averageTps(tps), 10.0);

    /* Add 1 more second at 100/sec */
    fakeMonotimeUs += ONE_SECOND_IN_MICROS;
    tpsCalculator_record(tps, 100);

    /* Window: 4s at 10 TPS + 1s at 100 TPS = 140/5 = 28 TPS */
    EXPECT_DOUBLE_EQ(tpsCalculator_averageTps(tps), 28.0);
}

TEST_F(StatCalcTest, TpsSuddenDecrease) {
    /* Fill window at 100/sec */
    for (int i = 0; i < 5; i++) {
        fakeMonotimeUs += ONE_SECOND_IN_MICROS;
        tpsCalculator_record(tps, 100);
    }
    EXPECT_DOUBLE_EQ(tpsCalculator_averageTps(tps), 100.0);

    /* One second at 0 */
    fakeMonotimeUs += ONE_SECOND_IN_MICROS;
    tpsCalculator_record(tps, 0);

    /* Window shifts: 4s at 100 TPS + 1s at 0 TPS = 400/5 = 80 */
    EXPECT_DOUBLE_EQ(tpsCalculator_averageTps(tps), 80.0);
}

TEST_F(StatCalcTest, TpsMultipleRecordsInOneInterval) {
    /* Two records before the update interval elapses accumulate (5 + 5); the
     * next flush folds them in together as 10 transactions. */
    tpsCalculator_record(tps, 5);
    tpsCalculator_record(tps, 5);
    fakeMonotimeUs += ONE_SECOND_IN_MICROS;
    EXPECT_DOUBLE_EQ(tpsCalculator_averageTps(tps), 10.0);
}

/* ======================== Trend Calculator Tests ========================== */

/* Trend calc updates once per window/DATA_POINTS. For a 5s window and 10 data
 * points, that is 500ms per datapoint. */
static const monotime TREND_INTERVAL = 5 * ONE_SECOND_IN_MICROS / 10;

TEST_F(StatCalcTest, TrendInitZero) {
    EXPECT_DOUBLE_EQ(trendCalculator_changePerSecShortTerm(trend), 0.0);
}

TEST_F(StatCalcTest, TrendSingleDatapointFlat) {
    /* A single datapoint cannot establish a slope, so the trend stays flat. */
    fakeMonotimeUs += TREND_INTERVAL;
    trendCalculator_recordMetric(trend, 100);
    EXPECT_DOUBLE_EQ(trendCalculator_changePerSecShortTerm(trend), 0.0);
}

TEST_F(StatCalcTest, TrendTwoPoint) {
    fakeMonotimeUs += TREND_INTERVAL;
    trendCalculator_recordMetric(trend, 100); /* First point fills all 10 slots */

    fakeMonotimeUs += TREND_INTERVAL;
    trendCalculator_recordMetric(trend, 0);

    /* Now we have 9 points at 100 and 1 point at 0.
     *  Left average is 100. Right average is 400/5 = 80.
     *  Trend has decreased 20 over 2.5 seconds, or 8/sec. */
    /* The short-term view shows a decrease from 100 to 0 over 1/2 sec. */
    EXPECT_DOUBLE_EQ(trendCalculator_changePerSecShortTerm(trend), -200.0);

    fakeMonotimeUs += TREND_INTERVAL;
    trendCalculator_recordMetric(trend, 0);

    /* Now we have 8 points at 100 and 2 points at 0.
     *  Left average is 100. Right average is 300/5 = 60.
     *  Trend has decreased 40 over 2.5 seconds, or 16/sec. */
    /* The short-term view shows no change (0 to 0). */
    EXPECT_DOUBLE_EQ(trendCalculator_changePerSecShortTerm(trend), 0.0);
}

TEST_F(StatCalcTest, TrendIntervalGating) {
    fakeMonotimeUs += TREND_INTERVAL;
    trendCalculator_recordMetric(trend, 100); /* First point fills all 10 slots */

    fakeMonotimeUs += TREND_INTERVAL - 1; /* Not at the collection interval yet */
    trendCalculator_recordMetric(trend, 0);

    /* The datapoint was not collected, so the trend should not have changed. */
    EXPECT_DOUBLE_EQ(trendCalculator_changePerSecShortTerm(trend), 0.0);
}

TEST_F(StatCalcTest, TrendRising) {
    /* Metric increases by 10 per 100ms.
     * Batch averages: 20, 70, 120, ..., 470.
     * olderAvg = (20+70+120+170+220)/5 = 120
     * newerAvg = (270+320+370+420+470)/5 = 370
     * trend = (370 - 120) / 2.5 = 100.0
     * Short-term: last two slots (420 -> 470) over 0.5s = 100.0 */
    for (int i = 0; i < 50; i++) {
        fakeMonotimeUs += ONE_SECOND_IN_MICROS / 10;
        trendCalculator_recordMetric(trend, (long)(i * 10));
    }
    EXPECT_DOUBLE_EQ(trendCalculator_changePerSecShortTerm(trend), 100.0);
}

TEST_F(StatCalcTest, TrendFalling) {
    /* Metric decreases by 10 per 100ms.
     * Batch averages: 480, 430, 380, ..., 30.
     * olderAvg = (480+430+380+330+280)/5 = 380
     * newerAvg = (230+180+130+80+30)/5 = 130
     * trend = (130 - 380) / 2.5 = -100.0
     * Short-term: last two slots (80 -> 30) over 0.5s = -100.0 */
    for (int i = 0; i < 50; i++) {
        fakeMonotimeUs += ONE_SECOND_IN_MICROS / 10;
        trendCalculator_recordMetric(trend, (long)(500 - i * 10));
    }
    EXPECT_DOUBLE_EQ(trendCalculator_changePerSecShortTerm(trend), -100.0);
}
