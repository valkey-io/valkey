/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "stat_calc.h"
#include "monotonic.h"
#include "zmalloc.h"
#include <stdbool.h>

static const long ONE_SECOND_IN_MICROS = 1000000;

/* ------------- TPS Calculator ------------- */
struct tpsCalculator {
    double window_secs;
    double window_us;
    double trans_per_window;
    monotime last_update;
    long update_freq_us;
    long uncounted_trans;
    bool is_new;
};

tpsCalculator *tpsCalculator_create(int window_secs) {
    tpsCalculator *calc = zmalloc(sizeof(tpsCalculator));
    calc->window_secs = (double)window_secs;
    calc->window_us = (double)window_secs * 1000000.0;
    calc->trans_per_window = 0.0;
    calc->last_update = getMonotonicUs();
    /* Update at most 20 times per window for smooth results. */
    calc->update_freq_us = window_secs * (ONE_SECOND_IN_MICROS / 20);
    calc->uncounted_trans = 0;
    calc->is_new = true;
    return calc;
}

void tpsCalculator_free(tpsCalculator *calc) {
    zfree(calc);
}

void tpsCalculator_record(tpsCalculator *calc, unsigned long transactions) {
    monotime now = getMonotonicUs();
    long elapsed_us = now - calc->last_update;

    calc->uncounted_trans += transactions;
    if (elapsed_us < calc->update_freq_us) return; /* accumulate until update frequency is hit */

    double total = (double)calc->uncounted_trans;
    calc->uncounted_trans = 0;
    calc->last_update = now;

    if (elapsed_us >= calc->window_us || calc->is_new) {
        calc->trans_per_window = total * calc->window_us / elapsed_us;
        calc->is_new = false;
    } else {
        /* Decay existing by fraction of window elapsed, add new. */
        calc->trans_per_window =
            (calc->trans_per_window * (calc->window_us - elapsed_us) / calc->window_us) + total;
    }
}

double tpsCalculator_averageTps(tpsCalculator *calc) {
    /* Flush any pending samples so the value reflects "now". */
    tpsCalculator_record(calc, 0);
    return calc->trans_per_window / calc->window_secs;
}

/* ------------- Trend Calculator ------------- */

#define DATA_POINTS 10
struct trendCalculator {
    int window_sec;
    monotime last_update;
    long update_freq_us;
    bool is_new;
    long metrics[DATA_POINTS];
    long uncounted_total;
    int uncounted_samples;
    double trend;
    double trend_short;
};

trendCalculator *trendCalculator_create(int window_secs) {
    trendCalculator *calc = zcalloc(sizeof(trendCalculator));
    calc->window_sec = window_secs;
    calc->last_update = getMonotonicUs();
    calc->update_freq_us = window_secs * ONE_SECOND_IN_MICROS / DATA_POINTS;
    calc->is_new = true;
    return calc;
}

void trendCalculator_free(trendCalculator *calc) {
    zfree(calc);
}

void trendCalculator_recordMetric(trendCalculator *calc, long metric_value) {
    monotime now = getMonotonicUs();
    long elapsed_us = now - calc->last_update;

    calc->uncounted_total += metric_value;
    calc->uncounted_samples++;

    if (elapsed_us < calc->update_freq_us) return;

    long new_value = calc->uncounted_total / calc->uncounted_samples;
    calc->uncounted_total = 0;
    calc->uncounted_samples = 0;
    calc->last_update = now;

    if (calc->is_new) {
        for (int i = 0; i < DATA_POINTS; i++) calc->metrics[i] = new_value;
        calc->is_new = false;
    }

    long older_total = 0;
    for (int i = 0; i < DATA_POINTS / 2; i++) {
        calc->metrics[i] = calc->metrics[i + 1];
        older_total += calc->metrics[i];
    }
    long newer_total = 0;
    for (int i = DATA_POINTS / 2; i < DATA_POINTS - 1; i++) {
        calc->metrics[i] = calc->metrics[i + 1];
        newer_total += calc->metrics[i];
    }
    calc->metrics[DATA_POINTS - 1] = new_value;
    newer_total += new_value;

    /* Formula is the average of the newer data points, less the average of the older data
     * points. The time is from the center of each half,
     * resulting in half the window size (secs). So the formula is:
     *     (AveNewer - AveOlder) / (WindowSec/2)
     * Where:
     *     AveNewer = newerTotal / (DATA_POINTS/2)
     *     AveOlder = olderTotal / (DATA_POINTS/2) */
    double older_avg = (double)older_total / (DATA_POINTS / 2);
    double newer_avg = (double)newer_total / (DATA_POINTS / 2);
    double time_between_centers = (double)calc->window_sec / 2.0;
    calc->trend = (newer_avg - older_avg) / time_between_centers;

    /* Short-term: rate of change between last 2 datapoints. */
    long delta_short = calc->metrics[DATA_POINTS - 1] - calc->metrics[DATA_POINTS - 2];
    double time_between_slots = (double)calc->window_sec / DATA_POINTS;
    calc->trend_short = delta_short / time_between_slots;
}

double trendCalculator_changePerSecShortTerm(trendCalculator *calc) {
    return calc->trend_short;
}
