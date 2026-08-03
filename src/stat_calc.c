/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "stat_calc.h"
#include "server.h"
#include "monotonic.h"

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

tpsCalculator *newTpsCalc(int window_secs) {
    serverAssert(window_secs > 0);
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
    int windowSec;
    monotime lastUpdate;
    long updateFreqUs;
    bool newCalculator;
    long metrics[DATA_POINTS];
    long uncountedTotal;
    int uncountedSamples;
    double trend;
    double trendShort;
};

trendCalculator *newTrendCalc(int windowSecs) {
    trendCalculator *calc = zcalloc(sizeof(trendCalculator));
    calc->windowSec = windowSecs;
    calc->lastUpdate = getMonotonicUs();
    calc->updateFreqUs = windowSecs * ONE_SECOND_IN_MICROS / DATA_POINTS;
    calc->newCalculator = true;
    return calc;
}

void trendCalc_free(trendCalculator *calc) {
    zfree(calc);
}

void trendCalc_recordMetric(trendCalculator *calc, long metricValue) {
    monotime now = getMonotonicUs();
    long elapsedUs = now - calc->lastUpdate;

    calc->uncountedTotal += metricValue;
    calc->uncountedSamples++;

    if (elapsedUs < calc->updateFreqUs) return;

    long newValue = calc->uncountedTotal / calc->uncountedSamples;
    calc->uncountedTotal = 0;
    calc->uncountedSamples = 0;
    calc->lastUpdate = now;

    if (calc->newCalculator) {
        for (int i = 0; i < DATA_POINTS; i++) calc->metrics[i] = newValue;
        calc->newCalculator = false;
    }

    long olderTotal = 0;
    for (int i = 0; i < DATA_POINTS / 2; i++) {
        calc->metrics[i] = calc->metrics[i + 1];
        olderTotal += calc->metrics[i];
    }
    long newerTotal = 0;
    for (int i = DATA_POINTS / 2; i < DATA_POINTS - 1; i++) {
        calc->metrics[i] = calc->metrics[i + 1];
        newerTotal += calc->metrics[i];
    }
    calc->metrics[DATA_POINTS - 1] = newValue;
    newerTotal += newValue;

    /* Formula is the average of the newer data points, less the average of the older data
     * points. The time is from the center of each half,
     * resulting in half the window size (secs). So the formula is:
     *     (AveNewer - AveOlder) / (WindowSec/2)
     * Where:
     *     AveNewer = newerTotal / (DATA_POINTS/2)
     *     AveOlder = olderTotal / (DATA_POINTS/2) */
    double olderAvg = (double)olderTotal / (DATA_POINTS / 2);
    double newerAvg = (double)newerTotal / (DATA_POINTS / 2);
    double timeBetweenCenters = (double)calc->windowSec / 2.0;
    calc->trend = (newerAvg - olderAvg) / timeBetweenCenters;

    /* Short-term: rate of change between last 2 datapoints. */
    long deltaShort = calc->metrics[DATA_POINTS - 1] - calc->metrics[DATA_POINTS - 2];
    double timeBetweenSlots = (double)calc->windowSec / DATA_POINTS;
    calc->trendShort = deltaShort / timeBetweenSlots;
}

double trendCalc_changePerSec(trendCalculator *calc) {
    return calc->trend;
}

double trendCalc_changePerSecShortTerm(trendCalculator *calc) {
    return calc->trendShort;
}
