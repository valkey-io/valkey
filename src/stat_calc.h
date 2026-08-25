/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Calculators for statistical values.
 */

#ifndef STAT_CALC_H
#define STAT_CALC_H

/* =========================== TPS Calculator =============================== */

/* A TPS calculator computes a rolling average TPS over a specified time window.
 * This smooths jitter in the measurement, with the average slightly lagging
 * instantaneous changes. This provides a stable measurement that is resilient
 * to short-lived traffic spikes.
 */

typedef struct tpsCalculator tpsCalculator;

tpsCalculator *tpsCalculator_create(int window_secs);

void tpsCalculator_free(tpsCalculator *calc);

/* Add a datapoint of new transactions to the calculator. This should be called at minimum 10 times
 * over the window for smooth results. */
void tpsCalculator_record(tpsCalculator *calc, unsigned long transactions);

/* Retrieve the average TPS over the calculator's window */
double tpsCalculator_averageTps(tpsCalculator *calc);


/* ========================== Trend Calculator ============================== */

/* A trend calculator computes the rate of change of a metric over a specified
 * time window, reported as average increase/decrease per second.
 * Examples:
 *     - Data 1,2,1,2,1,2 — trend ≈ 0 (oscillating, no net change)
 *     - Data 0,0,0,10,10,10 — trend ≈ 3/sec (step increase)
 * Conceptually similar to the slope of a linear regression, but uses a
 * lightweight approximation suitable for high-frequency sampling.
 */
typedef struct trendCalculator trendCalculator;

trendCalculator *trendCalculator_create(int window_secs);

void trendCalculator_free(trendCalculator *calc);

/* Add a datapoint to the calculator. Should be called at minimum 10 times
 * over the window for smooth results. If the metric is highly volatile,
 * calling more often reduces the impact of individual outliers. */
void trendCalculator_recordMetric(trendCalculator *calc, long metric_value);

/* Get the rate of change using only the final 10% of the window.
 * More responsive to sudden changes but noisier than the full-window trend. */
double trendCalculator_changePerSecShortTerm(trendCalculator *calc);

#endif
