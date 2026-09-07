/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstring>

extern "C" {
#include "hdr_histogram.h"
#include "server.h"
}

class HdrHistogramTest : public ::testing::Test {};

/* Returns the sum of every recorded count, which is what total_count is
 * supposed to track. */
static int64_t sum_of_counts(const struct hdr_histogram *h) {
    int64_t sum = 0;
    for (int32_t i = 0; i < h->counts_len; i++) sum += hdr_count_at_index(h, i);
    return sum;
}

/* Reproduces the state a torn hdr_reset() leaves behind: the buckets have been
 * cleared but total_count still counts concurrently recorded values. */
static void tearHistogram(struct hdr_histogram *h) {
    hdr_record_value(h, 500);
    hdr_record_value(h, 1200);
    hdr_record_value(h, 25000);
    memset(h->counts, 0, sizeof(int64_t) * h->counts_len);
    h->total_count = 3;
}

/* valkey-benchmark records latencies into one shared histogram from every
 * thread while showThroughput() resets that same histogram at the end of the
 * warmup period. hdr_reset() is a plain store followed by a memset, so a
 * concurrent hdr_record_value_atomic() can leave total_count larger than the
 * sum of the recorded counts.
 *
 * has_next() compares the running cumulative count against a snapshot of
 * total_count, so on such a histogram it never becomes false. The percentile
 * iterator must still terminate rather than spin forever, otherwise
 * showReport() hangs the whole process after the run has already finished. */
TEST_F(HdrHistogramTest, PercentileIterTerminatesWhenTotalCountExceedsCounts) {
    struct hdr_histogram *h;
    ASSERT_EQ(hdr_init(10, 3000000, 3, &h), 0);

    tearHistogram(h);
    ASSERT_GT(h->total_count, sum_of_counts(h));

    struct hdr_iter iter;
    hdr_iter_percentile_init(&iter, h, 1);

    /* Bounded so that a regression fails the test instead of hanging the suite. */
    const int max_iterations = 1000;
    int iterations = 0;
    while (hdr_iter_next(&iter)) {
        if (++iterations >= max_iterations) break;
    }
    ASSERT_LT(iterations, max_iterations) << "percentile iterator failed to terminate";

    hdr_close(h);
}

/* showReport() walks the histogram a second time with the linear iterator
 * (src/valkey-benchmark.c:1248), and LATENCY HISTOGRAM walks it with the
 * logarithmic iterator (src/latency.c:515). Both are gated only by has_next(),
 * so on the same torn histogram they never terminate either: move_next() keeps
 * returning true past the end of the counts array, incrementing counts_index
 * without bound. Guarding only the percentile iterator would move the hang
 * rather than remove it. */
TEST_F(HdrHistogramTest, LinearIterTerminatesWhenTotalCountExceedsCounts) {
    struct hdr_histogram *h;
    ASSERT_EQ(hdr_init(10, 3000000, 3, &h), 0);
    tearHistogram(h);

    struct hdr_iter iter;
    hdr_iter_linear_init(&iter, h, 100);

    const int max_iterations = 100000;
    int iterations = 0;
    while (hdr_iter_next(&iter)) {
        if (++iterations >= max_iterations) break;
    }
    ASSERT_LT(iterations, max_iterations) << "linear iterator failed to terminate";
    /* counts_index must not be walked past the end of the array. */
    ASSERT_LE(iter.counts_index, h->counts_len);

    hdr_close(h);
}

TEST_F(HdrHistogramTest, LogIterTerminatesWhenTotalCountExceedsCounts) {
    struct hdr_histogram *h;
    ASSERT_EQ(hdr_init(10, 3000000, 3, &h), 0);
    tearHistogram(h);

    struct hdr_iter iter;
    hdr_iter_log_init(&iter, h, 1024, 2);

    const int max_iterations = 100000;
    int iterations = 0;
    while (hdr_iter_next(&iter)) {
        if (++iterations >= max_iterations) break;
    }
    ASSERT_LT(iterations, max_iterations) << "log iterator failed to terminate";
    ASSERT_LE(iter.counts_index, h->counts_len);

    hdr_close(h);
}

/* A torn histogram cannot be reported accurately, but it must not invent data.
 * The percentile iterator reports one row per recorded value, so every value it
 * publishes has to be one that was actually recorded -- not the largest
 * representable value left behind by a scan that ran off the end of the counts
 * array. (The linear and logarithmic iterators are excluded on purpose: they
 * report fixed reporting levels, so walking into the empty buckets above the
 * last recorded value is their normal behaviour on any histogram.) */
TEST_F(HdrHistogramTest, PercentileIterDoesNotReportUnrecordedValueWhenCorrupt) {
    struct hdr_histogram *h;
    ASSERT_EQ(hdr_init(10, 3000000, 3, &h), 0);

    const int64_t values[] = {150, 400, 900, 2500, 11000};
    for (size_t i = 0; i < numElements(values); i++) {
        ASSERT_TRUE(hdr_record_value(h, values[i]));
    }
    const int64_t recorded_max = hdr_max(h);
    /* Only total_count is inflated here, so every bucket still holds real data
     * and anything reported must fall at or below the largest recorded value. */
    h->total_count += 2;
    ASSERT_GT(h->total_count, sum_of_counts(h));

    struct hdr_iter iter;
    hdr_iter_percentile_init(&iter, h, 1);
    int iterations = 0;
    while (hdr_iter_next(&iter) && ++iterations < 100000) {
        ASSERT_LE(iter.highest_equivalent_value, recorded_max)
            << "percentile iterator reported a value that was never recorded";
    }
    ASSERT_LT(iterations, 100000);

    hdr_close(h);
}

/* The termination guards must not cut a well-formed histogram short for the
 * linear and logarithmic iterators either. */
TEST_F(HdrHistogramTest, LinearAndLogItersStillReportFullDistribution) {
    struct hdr_histogram *h;
    ASSERT_EQ(hdr_init(10, 3000000, 3, &h), 0);

    const int64_t values[] = {150, 400, 900, 2500, 11000};
    for (size_t i = 0; i < numElements(values); i++) {
        ASSERT_TRUE(hdr_record_value(h, values[i]));
    }

    struct hdr_iter iter;
    hdr_iter_linear_init(&iter, h, 100);
    int64_t last_cumulative_count = 0;
    int iterations = 0;
    while (hdr_iter_next(&iter) && ++iterations < 100000) {
        last_cumulative_count = iter.cumulative_count;
    }
    ASSERT_GT(iterations, 0);
    ASSERT_EQ(last_cumulative_count, h->total_count);

    hdr_iter_log_init(&iter, h, 1024, 2);
    last_cumulative_count = 0;
    iterations = 0;
    while (hdr_iter_next(&iter) && ++iterations < 100000) {
        last_cumulative_count = iter.cumulative_count;
    }
    ASSERT_GT(iterations, 0);
    ASSERT_EQ(last_cumulative_count, h->total_count);

    hdr_close(h);
}

/* The same guard must not cut a well-formed histogram short: every recorded
 * value still has to be reported, and the iteration still has to end on the
 * 100th percentile. */
TEST_F(HdrHistogramTest, PercentileIterStillReportsFullDistribution) {
    struct hdr_histogram *h;
    ASSERT_EQ(hdr_init(10, 3000000, 3, &h), 0);

    const int64_t values[] = {150, 400, 900, 2500, 11000};
    for (size_t i = 0; i < numElements(values); i++) {
        ASSERT_TRUE(hdr_record_value(h, values[i]));
    }
    ASSERT_EQ(h->total_count, sum_of_counts(h));

    struct hdr_iter iter;
    hdr_iter_percentile_init(&iter, h, 1);
    struct hdr_iter_percentiles *percentiles = &iter.specifics.percentiles;

    const int max_iterations = 10000;
    int iterations = 0;
    double last_percentile = 0;
    int64_t last_cumulative_count = 0;
    while (hdr_iter_next(&iter)) {
        last_percentile = percentiles->percentile;
        last_cumulative_count = iter.cumulative_count;
        if (++iterations >= max_iterations) break;
    }

    ASSERT_LT(iterations, max_iterations) << "percentile iterator failed to terminate";
    ASSERT_GT(iterations, 0);
    /* All recorded values accounted for, and the walk ends at 100%. */
    ASSERT_EQ(last_cumulative_count, h->total_count);
    ASSERT_DOUBLE_EQ(last_percentile, 100.0);

    hdr_close(h);
}
