/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "server.h"
#include "throttle_repl.h"
#include "throttle.h"
#include "throttle_stat_calc.h"

#define RATE_INCREASE_MULTIPLIER 1.05
#define RATE_DECREASE_MULTIPLIER 0.95
#define COB_TREND_WINDOW_SECS 2
#define CONVERGENCE_SECS 30
#define MAX_COB_TARGET (1024L * 1024 * 1024) /* 1GB */
#define METRICS_NAME "ReplThrottle"

typedef struct {
    bool is_throttler_active;
    double current_throttle_rate;
    unsigned long throttle_activation_events;
    unsigned long throttle_more_events;
    unsigned long throttle_less_events;
} throttleReplMetrics;

static throttleReplMetrics metrics = {0};
static int throttle_id = 0;

/* --- Internal helpers --- */

static bool isThrottleActive(void) {
    return (throttle_id != 0);
}

/* Criteria: throttle commands that generate replication traffic. */
static bool criteriaProc(client *c, void *priv_data) {
    UNUSED(priv_data);
    if (c->cmd->flags & (CMD_WRITE | CMD_MAY_REPLICATE)) return true;
    return false;
}

static void installThrottler(void) {
    serverAssert(!isThrottleActive());
    throttle_id = throttle_register(criteriaProc, NULL, METRICS_NAME, THROTTLE_UNLIMITED_RATE);
    metrics.is_throttler_active = true;
    metrics.current_throttle_rate = THROTTLE_UNLIMITED_RATE;
    metrics.throttle_activation_events++;
}

static void removeThrottler(void) {
    serverAssert(isThrottleActive());
    throttle_deregister(throttle_id);
    throttle_id = 0;
    metrics.is_throttler_active = false;
    metrics.current_throttle_rate = THROTTLE_UNLIMITED_RATE;
}

static void adjustThrottleRate(bool reduceTrafficRate) {
    if (isThrottleActive()) {
        double rate;
        if (reduceTrafficRate) {
            rate = throttle_adjustRate(throttle_id, RATE_DECREASE_MULTIPLIER);
            metrics.throttle_more_events++;
        } else {
            rate = throttle_adjustRate(throttle_id, RATE_INCREASE_MULTIPLIER);
            metrics.throttle_less_events++;
            if (rate >= THROTTLE_UNLIMITED_RATE) removeThrottler();
        }
        metrics.current_throttle_rate = rate;
    } else {
        if (reduceTrafficRate) installThrottler();
    }
}

/* Evaluate whether steady-state throttling is needed.
 * Uses short-term COB trend to extrapolate future COB size. */
static bool evaluateSteadyState(client *c, uint64_t cob_size) {
    unsigned long cob_target = throttleRepl_getCobTargetSize();
    uint64_t min_throttle = cob_target / 2; // 25 % of the cob limit

    if (cob_size < min_throttle) return false;

    double short_trend = trendCalc_changePerSecShortTerm(c->cob_trend);
    int64_t extrapolated = (int64_t)cob_size + (int64_t)(short_trend * CONVERGENCE_SECS);

    return (extrapolated > (int64_t)cob_target);
}

/* --- Public API --- */

bool throttleRepl_isEnabled(void) {
    return server.repl_throttle;
}

bool throttleRepl_isClientExempt(client *c) {
    if (!iAmPrimary()) return false;
    if (!c->flag.replica) return false;
    if (!isThrottleActive()) return false;

    /* Throttle is actively working, protect this replica from COB
     * disconnect if its COB is above target. */
    unsigned long cob = getClientOutputBufferMemoryUsage(c);
    if (cob < throttleRepl_getCobTargetSize()) return false;

    /* Don't protect if throttle has been working too long without success. */
    time_t elapsed = server.unixtime - c->obuf_soft_limit_reached_time;
    if (elapsed > 4 * CONVERGENCE_SECS) return false;
    return true;
}

unsigned long throttleRepl_getCobTargetSize(void) {
    int64_t cob_target = server.client_obuf_limits[CLIENT_TYPE_REPLICA].soft_limit_bytes;
    if (cob_target == 0) cob_target = server.client_obuf_limits[CLIENT_TYPE_REPLICA].hard_limit_bytes;

    cob_target /= 2; /* Target is half the limit. */

    if (cob_target == 0 || cob_target > MAX_COB_TARGET) cob_target = MAX_COB_TARGET;

    return (unsigned long)cob_target;
}

void throttleRepl_adjustThrottling(void) {
    if (!iAmPrimary()) {
        /* Failover could happen before. */
        if (isThrottleActive()) removeThrottler();
        return;
    }
    if (!throttleRepl_isEnabled() && !isThrottleActive()) return;

    bool reduce = false;
    client *measured_replica = NULL;
    uint64_t largest_cob = 0;

    /* Scan replicas, find steady-state replica with smallest COB. */
    listIter li;
    listNode *ln;
    listRewind(server.replicas, &li);
    while ((ln = listNext(&li)) != NULL) {
        client *c = ln->value;
        if (!c->repl_data || c->repl_data->repl_state != REPLICA_STATE_ONLINE) continue;

        unsigned long cob_size = getClientOutputBufferMemoryUsage(c);

        /* Record trend per replica. */
        if (c->cob_trend == NULL) c->cob_trend = newTrendCalc(COB_TREND_WINDOW_SECS);
        trendCalc_recordMetric(c->cob_trend, cob_size);

        /* Ignore tiny COB (overhead only). */
        if (cob_size <= PROTO_REPLY_CHUNK_BYTES) cob_size = 0;

        // Find the largest cob size among replica clients
        if (measured_replica == NULL || cob_size > largest_cob) {
            measured_replica = c;
            largest_cob = cob_size;
        }
    }

    if (measured_replica != NULL) {
        reduce = evaluateSteadyState(measured_replica, largest_cob);
    }

    adjustThrottleRate(reduce);
}

sds throttleRepl_sdscatMetrics(sds info) {
    info = sdscatprintf(info,
                        "repl_throttle_active:%d\r\n",
                        metrics.is_throttler_active ? 1 : 0);

    if (metrics.is_throttler_active) {
        info = sdscatprintf(info,
                            "repl_throttle_rate:%.2f\r\n",
                            metrics.current_throttle_rate);
    }

    const throttleMetrics *throttle_metrics = throttle_getMetrics(METRICS_NAME);
    info = sdscatprintf(info,
                        "repl_throttle_activation_events:%lu\r\n"
                        "repl_throttle_more_events:%lu\r\n"
                        "repl_throttle_less_events:%lu\r\n"
                        "repl_throttle_below_guardrail_secs:%ld\r\n"
                        "repl_throttle_current_clients:%d\r\n"
                        "repl_throttle_total_commands:%d\r\n",
                        metrics.throttle_activation_events,
                        metrics.throttle_more_events,
                        metrics.throttle_less_events,
                        isThrottleActive() ? throttle_getGuardrailSecs(throttle_id) : 0L,
                        throttle_metrics->num_clients,
                        throttle_metrics->total_throttled_commands);

    return info;
}
