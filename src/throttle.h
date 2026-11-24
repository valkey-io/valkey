/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A generic client throttling framework using a token bucket algorithm.
 *
 * Plug-in evaluators register throttlers that control the rate at which client commands are
 * processed. When a client's command matches a throttler's criteria, the client is queued and
 * its commands are released at the configured rate.
 *
 * Design:
 * Multiple throttlers can be registered simultaneously. When a client matches more than one,
 * the most restrictive rate applies. Throttled clients have their read handler removed and
 * are released via timer events at the configured rate. Throttling occurs in processCommand()
 * before command execution. Once throttled, the client's command is deferred until tokens become
 * available.
 */

#ifndef THROTTLE_H
#define THROTTLE_H

#include "sds.h"
#include <stdbool.h>
typedef struct client client;
typedef struct throttler throttler;

static const double THROTTLE_UNLIMITED_RATE = 10000000.0;

/* A throttleCriteriaProc checks a client's current command and decides if it meets the criteria
 * for throttling. Returns true if the client meets the throttling criteria.
 *
 * The criteria proc should base decisions only on the state of the client, not considering
 * the question of the current requirements for throttling. If this returns true, the client
 * MIGHT be throttled.
 *
 * priv_data - a private data structure provided during throttle_register. It can provide
 *             anything needed by the criteria proc, or NULL if unneeded. */
typedef bool throttleCriteriaProc(client *c, void *priv_data);

/* Metrics for a throttler or group of related throttlers. The metrics name allows the metrics to
 * persist even after the throttler(s) is deregistered. Metrics collection will continue (under the
 * same name) if/when the throttler is registered again.
 *
 * Note: Multiple related throttlers can share the same metrics by using the same metrics_name.
 * A typical use case is multiple instantiations of the same throttler with different private data. */
typedef struct {
    int num_clients_throttled;        /* the backlog of currently throttled (queued) clients */
    long long num_commands_throttled; /* total number of commands throttled through this metrics group */
    double ops_per_sec;               /* the current throttling rate (summed across related throttlers) */
    double incoming_tps;              /* average incoming TPS over a 5-second rolling window */
    long oldest_client_delay_us;      /* delay in microseconds for the oldest throttled client */
} throttleMetrics;

/* Initialize the throttling framework. Must be called once at startup before any
 * throttler is registered. Idempotent: safe to call more than once. */
void throttle_init(void);

/* Register a new throttler.
 *   criteria_proc - identifies clients whose commands meet the criteria for throttling
 *   priv_data     - private data for passing to the criteria_proc (may be NULL)
 *   metrics_name  - a string used to identify a shared metrics group
 *
 * Returns the registered throttler. */
throttler *throttle_register(throttleCriteriaProc *criteria_proc,
                             void *priv_data,
                             const char *metrics_name);

/* Deregisters the throttler such that:
 *   - No new clients will be throttled by this throttler.
 *   - Existing queued clients will be drained at unlimited rate until the queue is empty. */
void throttle_deregister(throttler *t);

/* Set the absolute throttling rate for the given throttler.
 *   ops_per_sec - target rate in operations per second (must be >= 0)
 *
 * The rate is clamped: values below EPSILON are treated as 0,
 * and values above THROTTLE_UNLIMITED_RATE are capped at that ceiling. */
void throttle_setRate(throttler *t, double ops_per_sec);

/* A smart adjustment to the throttling rate. The multiplier is applied to the current rate,
 * with consideration for the actual incoming traffic rate.
 *   multiplier - applied to current rate to determine new rate (range 0.0 .. 3.0)
 *
 * If multiplier >= 1.0: increase the rate. If currently halted (rate ~0), jump to a
 *                       starting rate; otherwise, increase proportionally with a minimum
 *                       step of 1 ops/sec.
 * If multiplier < 1.0: decrease the rate proportionally. If the rate is far above the current
 *                      incoming rate, immediately adjusts down to the incoming rate.
 *
 * Returns the actual rate set after clamping and adjustment.
 *
 * Usage guidance:
 *    1.  Size each step to your call frequency: the more often you call this, the smaller
 *        each step should be. The driving metrics are smoothed and update slowly, so a large
 *        step applied at high frequency overshoots and causes hysteresis.
 *    2.  Prefer a small constant step, as a constant multiplicative step already tapers in
 *        absolute terms as the rate nears the target. */
double throttle_adjustRate(throttler *t, double multiplier);

/* Removes the client from the throttle queue. */
void throttle_removeClient(client *c);

/* Check if the client's current command should be throttled. Called at the beginning of
 * processCommand(). If any registered throttler's criteria matches, the client is queued
 * and the most restrictive throttle rate applies.
 *
 * Returns true if the client has been throttled.
 * Returns false if the client may proceed normally.
 *
 * Note: Even if a client matches throttling criteria, it might not be queued if tokens
 * are available. Throttling is checked before blocking, so a throttled
 * command cannot be blocked. Once a client is passed to this function, it will not be
 * throttled again for the same command after unblocking. */
bool throttle_throttleClientIfNeeded(client *c);

/* Get the metrics associated with a given metrics name.
 * The caller provides the metrics structure. */
void throttle_getMetrics(const char *metrics_name, throttleMetrics *metrics);

/* Append framework-level throttle metrics to the INFO output string.
 * Plug-in specific metrics are reported by their own sdscatInfoMetrics functions. */
sds throttle_sdscatInfoMetrics(sds info);

/* Get the number of seconds the throttler's rate has been below the guardrail.
 * Returns 0 if the rate is above the guardrail or the throttler is not active. */
long throttle_getGuardrailSecs(throttler *t);

#endif
