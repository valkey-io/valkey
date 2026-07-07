/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef STAT_CALC_H
#define STAT_CALC_H

/* Rolling-average TPS calculator over a configurable time window.
 *
 * Records transaction counts and reports a smoothed average TPS.
 */

typedef struct tpsCalculator tpsCalculator;

tpsCalculator *tpsCalculator_create(int window_secs);
void tpsCalculator_free(tpsCalculator *calc);

void tpsCalculator_record(tpsCalculator *calc, unsigned long transactions);
double tpsCalculator_averageTps(tpsCalculator *calc);

/* A trend calculator is used to compute the trend of data points over a specified time window.
 * Periodically, values are added to the calculator.  The calculator computes a "running trend" of
 * the data over the given time window.  The trend is reported as an average increase/decrease per
 * second.  Examples:
 *     - Data 1,2,1,2,1,2,1,2,1 - trend is essentially 0.  A trend line would have 0 slope.
 *     - Data 0,0,0,10,10,10 - trend is approximately 3/sec
 * This is similar to slope from a linear regression, but a simple speed-optimized algorithm.
 */
typedef struct trendCalculator trendCalculator;

trendCalculator *newTrendCalc(int windowSecs);
void trendCalc_recordMetric(trendCalculator *calc, long metricValue);
double trendCalc_changePerSec(trendCalculator *calc);
double trendCalc_changePerSecShortTerm(trendCalculator *calc);

#endif
