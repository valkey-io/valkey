/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "server.h"
#include "throttle_repl.h"
#include "throttle.h"
#include "stat_calc.h"

/* Configuration instance. */
struct throttle_repl_config throttle_repl_config;

#define RATE_INCREASE_MULTIPLIER 1.05
#define RATE_DECREASE_MULTIPLIER 0.95
#define COB_TREND_WINDOW_SECS 2              /* A 2-second window gives 20 data points at     \
                                              * 100ms serverCron. Sufficient for a good       \
                                              * measurement, while remaining short enough for \
                                              * throttling adjustments every 100ms. */
#define STEADY_STATE_CONVERGENCE_SECS 30     /* projection horizon for COB extrapolation */
#define MAX_COB_TARGET (1024L * 1024 * 1024) /* 1GB */
#define METRICS_NAME "ReplThrottle"          /* shared metrics group name */

/* Metrics for INFO output and operational visibility. */
typedef struct {
    bool is_throttler_active;
    double current_throttle_rate;             /* only valid if throttler is active */
    unsigned long throttle_activation_events; /* cumulative times throttler has been activated */
    unsigned long throttle_more_events;       /* cumulative times we throttled more */
    unsigned long throttle_less_events;       /* cumulative times we throttled less */
} throttleReplMetrics;

static throttleReplMetrics metrics = {0};
static int throttle_id = 0;

/* --- Internal helpers --- */

static bool isThrottlerActive(void) {
    return (throttle_id != 0);
}

/* Criteria: throttle commands that generate replication traffic. */
static bool criteriaProc(client *c, void *priv_data) {
    UNUSED(priv_data);
    if (c->cmd->flags & (CMD_WRITE | CMD_MAY_REPLICATE)) return true;
    return false;
}

static void installThrottler(void) {
    serverAssert(!isThrottlerActive());
    throttle_id = throttle_register(criteriaProc, NULL, METRICS_NAME);
    metrics.is_throttler_active = true;
    metrics.current_throttle_rate = THROTTLE_UNLIMITED_RATE;
    metrics.throttle_activation_events++;
}

static void uninstallThrottler(void) {
    serverAssert(isThrottlerActive());
    throttle_deregister(throttle_id);
    throttle_id = 0;
    metrics.is_throttler_active = false;
    metrics.current_throttle_rate = THROTTLE_UNLIMITED_RATE;
}

/* Apply a rate change based on the evaluator's decision. Installs the throttler on first
 * reduce request and removes it when rate reaches UNLIMITED. */
static void adjustThrottleRate(bool reduceTrafficRate) {
    if (isThrottlerActive()) {
        double rate;
        if (reduceTrafficRate) {
            rate = throttle_adjustRate(throttle_id, RATE_DECREASE_MULTIPLIER);
            metrics.throttle_more_events++;
        } else {
            rate = throttle_adjustRate(throttle_id, RATE_INCREASE_MULTIPLIER);
            metrics.throttle_less_events++;
            if (rate >= THROTTLE_UNLIMITED_RATE) uninstallThrottler();
        }
        metrics.current_throttle_rate = rate;
    } else {
        /* Installing the throttler starts measurement of current traffic rate.
         * Once the measurement is stable, rate adjustments will be meaningful. */
        if (reduceTrafficRate) installThrottler();
    }
}

static int64_t getReplicaSteadyStateCobTargetSize(void) {
    int64_t limit = server.client_obuf_limits[CLIENT_TYPE_REPLICA].soft_limit_bytes;
    if (limit == 0) limit = server.client_obuf_limits[CLIENT_TYPE_REPLICA].hard_limit_bytes;

    int64_t cob_target = limit / 2; /* Target is half the limit. */

    if (cob_target == 0 || cob_target > MAX_COB_TARGET) cob_target = MAX_COB_TARGET;

    return cob_target;
}

/* Steady-state throttling targets the replica with the largest COB to ensure all replicas
 * maintain sync. Throttling begins at 25% of the configured soft limit (half the target COB size).
 * The short-term COB trend is used to project when COB will intersect the target within the
 * convergence window. This will result in a convergence to the desired target, rather than
 * overshooting the target. */
static bool evaluateSteadyStateThrottle(client *c, int64_t cob_size) {
    int64_t cob_target = getReplicaSteadyStateCobTargetSize();
    int64_t throttle_threshold = cob_target / 2;

    if (cob_size < throttle_threshold) return false;

    /* Using the full window for COB trend shows greater hysteresis than using only the final
     * datapoints. The short trend results in more jittery rate adjustments, but this is good
     * as the up/down/up/down... type adjustments result in a smoother traffic rate than
     * up/up/up/down/down/down... */
    double short_trend = trendCalc_changePerSecShortTerm(c->cob_trend);
    int64_t extrapolated = cob_size + (int64_t)(short_trend * STEADY_STATE_CONVERGENCE_SECS);

    return (extrapolated > cob_target);
}

/* --- Public API --- */

/* In some cases, we want to protect replicas from being killed by the COB limits. When
 * throttling hasn't had time to adjust and there is no severe memory condition, it makes
 * sense to allow the replica to live until throttling can stabilize the situation. */
bool throttleRepl_isClientExemptFromCobLimits(client *c) {
    if (!throttle_repl_config.steady_state_repl_throttle_enabled || !isThrottlerActive()) return false;
    if (!iAmPrimary()) return false;
    if (!c->flag.replica) return false;

    /* Throttle is actively working, protect this replica from COB
     * disconnect if its COB is above target. */
    int64_t client_cob_size = (int64_t)getClientOutputBufferMemoryUsage(c);
    if (client_cob_size < getReplicaSteadyStateCobTargetSize()) return false;

    /* Don't exempt if server is over maxmemory.
     * When eviction is already running, we can't afford to let
     * replica output buffers grow further. */
    if (server.maxmemory && getMaxmemoryState(NULL, NULL, NULL, NULL) == C_ERR) return false;

    /* Don't protect if throttle has been working too long without success. */
    time_t elapsed = server.unixtime - c->obuf_soft_limit_reached_time;
    if (elapsed > 4 * STEADY_STATE_CONVERGENCE_SECS) return false;
    return true;
}

/* Called from serverCron every 100ms. Evaluates the replica with the largest COB and
 * adjusts throttling as needed. */
void throttleRepl_adjustThrottling(void) {
    if (!iAmPrimary()) {
        /* Failover could happen before. */
        if (isThrottlerActive()) uninstallThrottler();
        return;
    }
    if (!throttle_repl_config.steady_state_repl_throttle_enabled && !isThrottlerActive()) return;

    bool reduceTrafficRate = false;
    client *measured_steady_state_replica = NULL;
    uint64_t largest_steady_state_cob = 0;

    /* Scan replicas, find steady-state replica with largest COB. */
    listIter li;
    listNode *ln;
    listRewind(server.replicas, &li);
    while ((ln = listNext(&li)) != NULL) {
        client *c = ln->value;
        if (!c->repl_data || c->repl_data->repl_state != REPLICA_STATE_ONLINE) continue;

        unsigned long cob_size = getClientOutputBufferMemoryUsage(c);

        if (c->cob_trend == NULL) c->cob_trend = newTrendCalc(COB_TREND_WINDOW_SECS);
        trendCalc_recordMetric(c->cob_trend, cob_size);

        /* The COB size contains some overhead. Treat it as zero until we reach a minimum. */
        if (cob_size <= PROTO_REPLY_CHUNK_BYTES) cob_size = 0;

        if (measured_steady_state_replica == NULL || cob_size > largest_steady_state_cob) {
            measured_steady_state_replica = c;
            largest_steady_state_cob = cob_size;
        }
    }

    if (measured_steady_state_replica != NULL) {
        reduceTrafficRate = evaluateSteadyStateThrottle(measured_steady_state_replica, largest_steady_state_cob);
    }

    adjustThrottleRate(reduceTrafficRate);
}

sds throttleRepl_sdscatInfoMetrics(sds info) {
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
                        isThrottlerActive() ? throttle_getGuardrailSecs(throttle_id) : 0L,
                        throttle_metrics->num_clients_throttled,
                        throttle_metrics->num_throttled_commands);

    return info;
}
