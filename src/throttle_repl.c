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
struct throttleReplConfig throttleRepl_config;

/* A 2-second window gives 20 data points at 100ms serverCron. Sufficient for a good
 * measurement, while remaining short enough for throttling adjustments every 100ms. */
static const int COB_TREND_WINDOW_SECS = 2;
static const double RATE_INCREASE_MULTIPLIER = 1.05;
static const double RATE_DECREASE_MULTIPLIER = 0.95;
static const int STEADY_STATE_CONVERGENCE_SECS = 30;     /* projection horizon for COB extrapolation */
static const char *const METRICS_NAME = "repl_throttle"; /* shared metrics group name */

/* Metrics for INFO output and operational visibility. */
typedef struct {
    bool is_throttler_active;
    double current_throttle_rate;             /* only valid if throttler is active */
    unsigned long throttle_activation_events; /* cumulative times throttler has been activated */
    unsigned long throttle_more_events;       /* cumulative times we throttled more */
    unsigned long throttle_less_events;       /* cumulative times we throttled less */
} throttleReplMetrics;

static throttleReplMetrics metrics = {0};
static throttler *repl_throttler = NULL;

/* --- Internal helpers --- */

static bool isThrottlerActive(void) {
    return (repl_throttler != NULL);
}

/* Criteria: throttle commands that generate replication traffic. */
static bool criteriaProc(client *c, void *priv_data) {
    UNUSED(priv_data);
    if (c->cmd->flags & (CMD_WRITE | CMD_MAY_REPLICATE)) return true;
    return false;
}

static void installThrottler(void) {
    serverAssert(!isThrottlerActive());
    repl_throttler = throttle_register(criteriaProc, NULL, METRICS_NAME);
    serverAssert(repl_throttler != NULL);
    metrics.is_throttler_active = true;
    metrics.current_throttle_rate = THROTTLE_UNLIMITED_RATE;
    metrics.throttle_activation_events++;
}

static void uninstallThrottler(void) {
    serverAssert(isThrottlerActive());
    throttle_deregister(repl_throttler);
    repl_throttler = NULL;
    metrics.is_throttler_active = false;
    metrics.current_throttle_rate = THROTTLE_UNLIMITED_RATE;
}

/* Apply a rate change based on the evaluator's decision. Installs the throttler on first
 * reduce request and removes it when rate reaches UNLIMITED. */
static void adjustThrottleRate(bool reduce_traffic_rate) {
    if (isThrottlerActive()) {
        double rate;
        if (reduce_traffic_rate) {
            rate = throttle_adjustRate(repl_throttler, RATE_DECREASE_MULTIPLIER);
            metrics.throttle_more_events++;
        } else {
            rate = throttle_adjustRate(repl_throttler, RATE_INCREASE_MULTIPLIER);
            metrics.throttle_less_events++;
            if (rate >= THROTTLE_UNLIMITED_RATE) uninstallThrottler();
        }
        metrics.current_throttle_rate = rate;
    } else {
        /* Installing the throttler starts measurement of current traffic rate.
         * Once the measurement is stable, rate adjustments will be meaningful. */
        if (reduce_traffic_rate) installThrottler();
    }
}

static int64_t getReplicaSteadyStateCobTargetSize(void) {
    int64_t limit = server.client_obuf_limits[CLIENT_TYPE_REPLICA].soft_limit_bytes;
    if (limit == 0) limit = server.client_obuf_limits[CLIENT_TYPE_REPLICA].hard_limit_bytes;

    int64_t cob_target = limit / 2; /* Target is half the limit. */

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
    double short_trend = trendCalculator_changePerSecShortTerm(c->cob_trend);
    int64_t extrapolated = cob_size + (int64_t)(short_trend * STEADY_STATE_CONVERGENCE_SECS);

    return (extrapolated > cob_target);
}

/* --- Public API --- */

/* Determines whether a replica should be temporarily exempted from the soft client output
 * buffer limit. While the steady-state throttle is converging, exempting the soft limit
 * prevents a premature disconnect and allows the throttler to reduce the replica's buffer
 * back below target. */
bool throttleRepl_isClientExemptFromCobLimits(client *c) {
    if (!throttleRepl_config.repl_throttling_enabled || !isThrottlerActive()) return false;
    if (!iAmPrimary()) return false;
    if (getClientType(c) != CLIENT_TYPE_REPLICA) return false;

    /* Throttle is actively working, protect this replica from COB
     * disconnect if its COB is above target. */
    int64_t client_cob_size = (int64_t)getClientOutputBufferMemoryUsage(c);
    /* There's no need to protect the replica if it's already using less than the target size. */
    if (client_cob_size < getReplicaSteadyStateCobTargetSize()) return false;

    /* Don't exempt if the server is over maxmemory.
     * When eviction is already running, we can't afford to let replica output buffers grow further. */
    if (server.maxmemory && getMaxmemoryState(NULL, NULL, NULL, NULL) == C_ERR) return false;

    /* Don't protect if throttle has been working too long without success. */
    time_t elapsed = server.unixtime - c->obuf_soft_limit_reached_time;
    if (elapsed > 4 * STEADY_STATE_CONVERGENCE_SECS) return false;
    /* Otherwise, allow the replica to exceed the soft limit, giving the throttler time to correct. */
    return true;
}

/* Called from serverCron every 100ms. Evaluates the replica with the largest COB and
 * adjusts throttling as needed. */
void throttleRepl_adjustThrottling(void) {
    /* Tear down and stop if we're no longer the primary (e.g. after failover), replication
     * throttling was disabled, no COB limit is configured, or the last replica disconnected. */
    if (!iAmPrimary() || !throttleRepl_config.repl_throttling_enabled ||
        getReplicaSteadyStateCobTargetSize() <= 0 || listLength(server.replicas) == 0) {
        if (isThrottlerActive()) uninstallThrottler();
        return;
    }

    bool reduce_traffic_rate = false;
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

        if (c->cob_trend == NULL) c->cob_trend = trendCalculator_create(COB_TREND_WINDOW_SECS);
        trendCalculator_recordMetric(c->cob_trend, cob_size);

        /* The COB size contains some overhead. Treat it as zero until we reach a minimum. */
        if (cob_size <= PROTO_REPLY_CHUNK_BYTES) cob_size = 0;

        if (measured_steady_state_replica == NULL || cob_size > largest_steady_state_cob) {
            measured_steady_state_replica = c;
            largest_steady_state_cob = cob_size;
        }
    }

    if (measured_steady_state_replica != NULL) {
        reduce_traffic_rate = evaluateSteadyStateThrottle(measured_steady_state_replica, largest_steady_state_cob);
    }

    adjustThrottleRate(reduce_traffic_rate);
}

sds throttleRepl_sdscatInfoMetrics(sds info) {
    throttleMetrics throttle_metrics;
    throttle_getMetrics(METRICS_NAME, &throttle_metrics);
    info = sdscatprintf(info,
                        "repl_throttle_rate:%.2f\r\n"
                        "repl_throttle_activation_events:%lu\r\n"
                        "repl_throttle_below_guardrail_secs:%ld\r\n"
                        "repl_throttle_total_commands:%lld\r\n",
                        metrics.is_throttler_active ? metrics.current_throttle_rate : -1.0,
                        metrics.throttle_activation_events,
                        isThrottlerActive() ? throttle_getGuardrailSecs(repl_throttler) : 0L,
                        throttle_metrics.num_commands_throttled);

    return info;
}

/* Verbose debug metrics. */
sds throttleRepl_sdscatInfoDebugMetrics(sds info) {
    throttleMetrics throttle_metrics;
    throttle_getMetrics(METRICS_NAME, &throttle_metrics);
    info = sdscatprintf(info,
                        "repl_throttle_more_events:%lu\r\n"
                        "repl_throttle_less_events:%lu\r\n"
                        "repl_throttle_current_clients:%d\r\n",
                        metrics.throttle_more_events,
                        metrics.throttle_less_events,
                        throttle_metrics.num_clients_throttled);

    return info;
}
