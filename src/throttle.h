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

#include "server.h"
#include <stdbool.h>

static const double THROTTLE_UNLIMITED_RATE = 10000000.0;

static const int THROTTLE_INVALID_ID = -2;

/* A throttleCriteriaProc checks a client's current command and decides if it meets the criteria
 * for throttling. Returns true if the client meets the throttling criteria.
 *
 * priv_data - a private data structure provided during throttle_register. It can provide
 *             anything needed by the criteria proc, or NULL if unneeded. */
typedef bool throttleCriteriaProc(client *c, void *priv_data);

/* Metrics for a group of related throttlers sharing the same metrics_name.
 *
 * Note: Multiple related throttlers can share the same metrics by using the same metrics_name.
 * A typical use case is multiple instantiations of the same throttler with different private
 * data. */
typedef struct {
    int num_clients_throttled;   /* the backlog of currently throttled (queued) clients */
    int num_throttled_commands;  /* total number of commands throttled through this metrics group */
    double ops_per_sec;          /* the current throttling rate (summed across related throttlers) */
    double incoming_tps;         /* average incoming TPS over a 5-second rolling window */
    long oldest_client_delay_us; /* delay in microseconds for the oldest throttled client */
} throttleMetrics;

void throttle_init(void);

/* Register a new throttler.
 *   criteria_proc - identifies clients whose commands meet the criteria for throttling
 *   priv_data     - private data for passing to the criteria_proc (may be NULL)
 *   metrics_name  - a string used to identify a shared metrics group
 *
 * Returns an integer ID of the new throttler. */
int throttle_register(throttleCriteriaProc *criteria_proc,
                      void *priv_data,
                      const char *metrics_name);

/* Deregisters the throttler such that:
 *   - No new clients will be throttled by this throttler.
 *   - Existing queued clients will be drained at unlimited rate until the queue is empty. */
void throttle_deregister(int id);

void throttle_setRate(int id, double ops_per_sec);

/* A smart adjustment to the throttling rate. The multiplier is applied to the current rate,
 * with consideration for the actual incoming traffic rate.
 *   multiplier - applied to current rate to determine new rate (range 0.0 .. 3.0)
 *
 * If multiplier > 1.0: increase rate (with minimum step of 1 ops/sec at low rates).
 * If multiplier < 1.0: decrease rate (clamped to incoming TPS floor).
 * If multiplier == 0.0: halt (rate set to 0).
 *
 * Returns the actual rate set after clamping and adjustment.
 *
 * Usage guidance:
 *    1.  Adjust throttling at a regular interval > 250ms.  Adjusting the throttle too fast will
 *        result in large throttling swings before an observed metric has a chance to change.
 *        This can easily create a hysteresis problem.  The current incoming rate is based on a
 *        5-second window and will not update faster than 250ms.
 *    2.  Set a target for the observed metric.  As the observed metric approaches the target, make
 *        progressively smaller changes to the rate. */
double throttle_adjustRate(int id, double multiplier);

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
bool throttleClientIfNeeded(client *c);

/* Get the total number of commands throttled across all throttlers. */
long long throttle_getTotalThrottledCommands(void);

/* Get the metrics associated with a given metrics name.
 * Memory is managed by the throttler. Do not free the returned pointer.
 * Call this each time metrics are needed. Do not cache the pointer. */
const throttleMetrics *throttle_getMetrics(const char *metrics_name);

/* Append framework-level throttle metrics to the INFO output string.
 * Plug-in specific metrics are reported by their own sdscatInfoMetrics functions. */
sds throttle_sdscatInfoMetrics(sds info);

/* Get the number of seconds the throttler's rate has been below the guardrail.
 * Returns 0 if the rate is above the guardrail or the throttler is not active. */
long throttle_getGuardrailSecs(int id);

#endif
