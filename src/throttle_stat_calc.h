#ifndef THROTTLE_STAT_CALC_H
#define THROTTLE_STAT_CALC_H

/* Rolling-average TPS calculator over a configurable time window.
 *
 * Records transaction counts and reports a smoothed average TPS.
 * The smoothing uses a blending approach: existing count decays
 * proportional to elapsed time, new transactions are added on top.
 * This produces a lagging average that converges over the window.
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

// Allocate a new trend calculator.  Caller is responsible to deallocate with zfree().
trendCalculator *newTrendCalc(int windowSecs);

// Add a metric value to the trend calculator.  This should be called at minimum 10
//  times over the window for best results.  Note, if the metric is highly volatile,
//  it is better to call more often - as a single outlier is less likely to skew results.
void trendCalc_recordMetric(trendCalculator *calc, long metricValue);

// Retrieve the average trend over the calculator's window.  Note: this value is updated
//  approximately 10 times over the size of the window.  So, with a 1 minute window, the
//  reported trend will be updated roughly every 6 seconds.
double trendCalc_changePerSec(trendCalculator *calc);

// Retrieve the trend using only the FINAL 10% of the calculator's window.  This represents a
//  short-term view of the trend.
double trendCalc_changePerSecShortTerm(trendCalculator *calc);

#endif /* THROTTLE_STAT_CALC_H */
