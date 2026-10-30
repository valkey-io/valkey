/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Unit tests for the INFO latencystats percentile output.
 */

#include "generated_wrappers.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "hdr_histogram.h"
#include "server.h"
}

static double test_percentiles[] = {0.0, 0.1, 1.0, 25.0, 50.0, 75.0, 90.0, 99.0, 99.9, 99.99, 100.0};
static const int num_percentiles = sizeof(test_percentiles) / sizeof(test_percentiles[0]);

class LatencyPercentilesTest : public ::testing::Test {
  protected:
    struct hdr_histogram *histogram;
    double *saved_percentiles;
    int saved_percentiles_len;

    void SetUp() override {
        histogram = NULL;
        saved_percentiles = server.latency_tracking_info_percentiles;
        saved_percentiles_len = server.latency_tracking_info_percentiles_len;
        setPercentiles(test_percentiles, num_percentiles);
    }

    void TearDown() override {
        if (histogram) hdr_close(histogram);
        setPercentiles(saved_percentiles, saved_percentiles_len);
    }

    /* Labels must be freed while the old length is still set. */
    void setPercentiles(double *percentiles, int len) {
        freeLatencyTrackingInfoPercentilesLabels();
        server.latency_tracking_info_percentiles = percentiles;
        server.latency_tracking_info_percentiles_len = len;
        createLatencyTrackingInfoPercentilesLabels();
    }

    void record(long long value, long long count) {
        for (long long i = 0; i < count; i++) updateCommandLatencyHistogram(&histogram, value);
    }

    /* Build the line the way it was built before: hdr_value_at_percentile()
     * for each percentile, printed with "%.3f". */
    sds expectedLine(void) {
        sds s = sdsnew("latency_percentiles_usec_cmd:");
        for (int j = 0; j < num_percentiles; j++) {
            char fbuf[128];
            size_t len = snprintf(fbuf, sizeof(fbuf), "%f", test_percentiles[j]);
            trimDoubleString(fbuf, len);
            double value = (double)hdr_value_at_percentile(histogram, test_percentiles[j]) / 1000.0;
            s = sdscatprintf(s, "p%s=%.3f", fbuf, value);
            if (j != num_percentiles - 1) s = sdscatlen(s, ",", 1);
        }
        return sdscatlen(s, "\r\n", 2);
    }

    void checkLine(void) {
        sds expected = expectedLine();
        sds actual = fillPercentileDistributionLatencies(sdsempty(), "cmd", histogram);
        EXPECT_STREQ(expected, actual);
        sdsfree(expected);
        sdsfree(actual);
    }
};

TEST_F(LatencyPercentilesTest, SingleValue) {
    record(1500, 1);
    checkLine();
}

TEST_F(LatencyPercentilesTest, SinglePercentile) {
    static double p50[] = {50.0};
    record(1000, 1);
    record(2000, 3);
    setPercentiles(p50, 1);
    double value = (double)hdr_value_at_percentile(histogram, 50.0) / 1000.0;
    sds expected = sdscatprintf(sdsempty(), "latency_percentiles_usec_cmd:p50=%.3f\r\n", value);
    sds actual = fillPercentileDistributionLatencies(sdsempty(), "cmd", histogram);
    EXPECT_STREQ(expected, actual);
    sdsfree(expected);
    sdsfree(actual);
}

TEST_F(LatencyPercentilesTest, EmptyPercentiles) {
    record(1500, 1);
    setPercentiles(NULL, 0);
    sds actual = fillPercentileDistributionLatencies(sdsempty(), "cmd", histogram);
    EXPECT_STREQ("latency_percentiles_usec_cmd:\r\n", actual);
    sdsfree(actual);
}

TEST_F(LatencyPercentilesTest, MinAndMaxRange) {
    record(LATENCY_HISTOGRAM_MIN_VALUE, 1);
    record(LATENCY_HISTOGRAM_MAX_VALUE, 1);
    checkLine();
}

TEST_F(LatencyPercentilesTest, ZeroValues) {
    /* updateCommandLatencyHistogram() never records 0, so record it directly.
     * hdr does not track 0 in min_value. */
    record(5000, 4);
    for (int i = 0; i < 6; i++) hdr_record_value(histogram, 0);
    checkLine();
}

TEST_F(LatencyPercentilesTest, BodyWithSlowOutlier) {
    /* The common shape: fast calls plus one slow call, so p99.9 is the max. */
    srand(1);
    for (int i = 0; i < 200; i++) record(800 + rand() % 3000, 1);
    record(5000000, 1);
    checkLine();
}

TEST_F(LatencyPercentilesTest, ManySamplesInTopBucket) {
    record(1000, 10);
    record(900000, 90);
    checkLine();
}

TEST_F(LatencyPercentilesTest, BucketBoundaries) {
    for (int shift = 0; shift < 30; shift++) {
        record((1LL << shift) - 1, 1);
        record(1LL << shift, 1);
        record((1LL << shift) + 1, 1);
    }
    checkLine();
}

TEST_F(LatencyPercentilesTest, RandomShapes) {
    srand(7);
    for (int iter = 0; iter < 200; iter++) {
        if (histogram) {
            hdr_close(histogram);
            histogram = NULL;
        }
        int samples = 1 + rand() % 500;
        int shape = iter % 3;
        for (int i = 0; i < samples; i++) {
            long long value;
            if (shape == 0) {
                value = 1 + rand() % LATENCY_HISTOGRAM_MAX_VALUE;
            } else if (shape == 1) {
                value = 1 + rand() % 5000;
            } else {
                value = (rand() % 20 == 0) ? 1 + rand() % LATENCY_HISTOGRAM_MAX_VALUE : 500 + rand() % 3000;
            }
            record(value, 1 + (rand() % 10 == 0 ? rand() % 50 : 0));
        }
        checkLine();
    }
}
