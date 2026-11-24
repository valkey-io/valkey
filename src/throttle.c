/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "server.h"
#include "throttle.h"
#include "throttle_token_bucket.h"
#include "stat_calc.h"
#include "hashtable.h"
#include "monotonic.h"

#include <math.h>

static const int MAX_WAIT_TIME_MS = 100;                      /* max ms before rescheduling timer */
static const uint64_t MAX_UNTHROTTLE_PROCESSING_TIME_MS = 10; /* max ms spent unthrottling per timer fire */
static const double THROTTLE_OPS_PER_SEC_GUARDRAIL = 0.1;     /* report when rate stays below this TPS */
static const int TPS_WINDOW_SEC = 5;                          /* rolling window for incoming TPS measurement */
static const double EPSILON = 0.0001;                         /* values below this are treated as zero */
static const double TOKENS_BURST_RATE_SEC = 0.1;              /* burst capacity in seconds of sustained rate */
static const double MIN_ADJUST_AFTER_DISABLE = 100.0;         /* initial rate when recovering from halted state */

static hashtable *metrics_table = NULL;    /* Maps throtter type name to metricsEntry*. */
static list *throttler_list = NULL;        /* all currently registered throttlers */
static long long total_throttled_commands; /* framework-level cumulative throttled-command counter */

typedef struct metricsEntry {
    sds throttler_type;
    int num_clients_throttled;
    long long num_commands_throttled;
    tpsCalculator *incoming_tps;
} metricsEntry;

typedef struct throttler {
    bool cleanup;                        /* true once deregistered; freed when its queue drains */
    throttleCriteriaProc *criteria_proc; /* callback defining throttling criteria */
    long long time_event_id;             /* timer event id for throttlerTimeProc */
    void *priv_data;                     /* private data for use by the criteria_proc */
    tokenBucket *bucket;                 /* token bucket: 1 token = 1 operation */
    list *client_queue;                  /* clients currently queued for throttling */
    listNode *ln;                        /* my node in throttler_list */
    monotime rate_below_guardrail_since; /* timestamp when rate dropped below guardrail, or 0 */
    metricsEntry *metrics;               /* reference to the named metrics object */
} throttler;

/* Metrics hashtable callbacks. */
static const void *metricsGetKey(const void *entry) {
    return ((metricsEntry *)entry)->throttler_type;
}

static void metricsDestructor(void *entry) {
    metricsEntry *m = entry;
    sdsfree(m->throttler_type);
    tpsCalculator_free(m->incoming_tps);
    zfree(m);
}

static hashtableType metricsHashtableType = {
    .entryGetKey = metricsGetKey,
    .hashFunction = dictSdsHash,
    .keyCompare = dictSdsKeyCompare,
    .entryDestructor = metricsDestructor,
};

/* dictType for metricsEntry* to earliest below-guardrail monotime, used for
 * producing INFO output to report earliest guardrail per metrics type. */
static dictType guardrailAggDictType = {
    .entryGetKey = dictEntryGetKey,
    .entryDestructor = zfree,
};

static metricsEntry *findMetrics(const char *name) {
    sds key = sdsnew(name);
    metricsEntry *found;
    if (hashtableFind(metrics_table, key, (void **)&found)) {
        sdsfree(key);
        return found;
    }
    metricsEntry *m = zcalloc(sizeof(metricsEntry));
    m->throttler_type = key;
    m->incoming_tps = tpsCalculator_create(TPS_WINDOW_SEC);
    hashtableAdd(metrics_table, m);
    return m;
}

/* Compute how long to wait before the next token becomes available. */
static int waitTimeMs(throttler *t) {
    double ms = tokenBucket_msUntilAvailable(t->bucket, 1.0);
    if (ms < 0 || ms >= MAX_WAIT_TIME_MS) return MAX_WAIT_TIME_MS;
    return (int)ceil(ms);
}

/* Release throttler resources. Only called when client queue is fully drained. */
static void freeThrottler(throttler *t) {
    serverAssert(listLength(t->client_queue) == 0);
    serverAssert(t->time_event_id == AE_DELETED_EVENT_ID);
    serverAssert(t->ln != NULL);
    listDelNode(throttler_list, t->ln);
    listRelease(t->client_queue);
    tokenBucket_free(t->bucket);
    /* metrics is shared and do not free here */
    zfree(t);
}

/* Remove a throttled client from its throttler's queue and clear its throttle state. */
static void dequeueThrottledClient(client *c) {
    serverAssert(c->flag.throttled);
    c->flag.throttled = 0;
    throttler *t = c->throttler;
    serverAssert(t != NULL);

    listDelNode(t->client_queue, c->throttle_node);
    t->metrics->num_clients_throttled--;

    c->throttler = NULL;
    c->throttle_node = NULL;
    c->throttle_start = 0;
}

static void consumeOtherThrottlers(client *c, throttler *except) {
    listNode *ln;
    listIter li;
    listRewind(throttler_list, &li);
    while ((ln = listNext(&li))) {
        throttler *t = ln->value;
        if (t->cleanup || t == except) continue;
        if (t->criteria_proc(c, t->priv_data)) tokenBucket_tryConsume(t->bucket, 1.0, true);
    }
}

/* Timer event handler: releases queued clients at the token bucket rate.
 * Processes clients until tokens are exhausted or time budget is spent. */
static long long throttlerTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    UNUSED(eventLoop);
    UNUSED(id);

    throttler *t = (throttler *)clientData;

    monotime work_start;
    elapsedStart(&work_start);

    while (listLength(t->client_queue) > 0 &&
           elapsedMs(work_start) < MAX_UNTHROTTLE_PROCESSING_TIME_MS &&
           tokenBucket_tryConsume(t->bucket, 1.0, false)) {
        client *c = listNodeValue(listFirst(t->client_queue));
        dequeueThrottledClient(c);
        if (c->flag.throttle_multi) {
            c->flag.throttle_multi = 0;
            consumeOtherThrottlers(c, t);
        }
        serverAssert(c->argc > 0 && c->flag.pending_command && !c->flag.throttled);
        queueClientForReprocessing(c); // Read handler will be installed during reprocessing.
    }

    if (listLength(t->client_queue) == 0) {
        t->time_event_id = AE_DELETED_EVENT_ID;
        /* This throttler is drained and ready to be freed. */
        if (t->cleanup) freeThrottler(t);
        return AE_NOMORE;
    }
    return waitTimeMs(t);
}

static void throttlerAddClient(throttler *t, client *c) {
    serverAssert(c->throttler == NULL);
    serverAssert(!c->flag.throttled);
    elapsedStart(&c->throttle_start);
    c->flag.throttled = 1;
    listAddNodeTail(t->client_queue, c);

    if (c->conn) connSetReadHandler(c->conn, NULL);

    t->metrics->num_clients_throttled++;
    t->metrics->num_commands_throttled++;
    total_throttled_commands++;
    c->throttler = t;
    c->throttle_node = listLast(t->client_queue);

    if (listLength(t->client_queue) == 1) {
        serverAssert(t->time_event_id == AE_DELETED_EVENT_ID);
        t->time_event_id = aeCreateTimeEvent(server.el,
                                             waitTimeMs(t),
                                             throttlerTimeProc,
                                             t, NULL);
    }
}

/* === Public API === */

void throttle_init(void) {
    if (throttler_list == NULL) {
        throttler_list = listCreate();
    }
    if (metrics_table == NULL) {
        metrics_table = hashtableCreate(&metricsHashtableType);
    }
}

/* In most cases, each throttler should have its own independent metrics_name. When the same
 * throttler is instantiated multiple times (with different priv_data), they may share a single
 * metrics object by using the same name. This allows statistics to be aggregated across related
 * throttler instances. */
throttler *throttle_register(throttleCriteriaProc *criteria_proc,
                             void *priv_data,
                             const char *metrics_name) {
    serverAssert(criteria_proc != NULL);
    serverAssert(metrics_name != NULL);

    throttler *t = zmalloc(sizeof(throttler));
    t->cleanup = false;
    t->criteria_proc = criteria_proc;
    t->time_event_id = AE_DELETED_EVENT_ID;
    t->priv_data = priv_data;
    t->bucket = tokenBucket_create(THROTTLE_UNLIMITED_RATE, TOKENS_BURST_RATE_SEC);
    t->metrics = findMetrics(metrics_name);
    t->client_queue = listCreate();
    t->rate_below_guardrail_since = 0;
    listAddNodeTail(throttler_list, t);
    t->ln = listLast(throttler_list);
    throttle_setRate(t, THROTTLE_UNLIMITED_RATE);
    serverLog(LL_DEBUG, "Throttler registered: type=%s", t->metrics->throttler_type);
    return t;
}

void throttle_deregister(throttler *t) {
    serverAssert(t != NULL);
    serverLog(LL_DEBUG, "Throttler deregistered: type=%s", t->metrics->throttler_type);

    if (listLength(t->client_queue) == 0) {
        freeThrottler(t);
    } else {
        t->cleanup = true;
        throttle_setRate(t, THROTTLE_UNLIMITED_RATE);
    }
}

void throttle_setRate(throttler *t, double ops_per_sec) {
    serverAssert(ops_per_sec >= 0);

    if (ops_per_sec < EPSILON) {
        ops_per_sec = 0;
    } else if (ops_per_sec > THROTTLE_UNLIMITED_RATE) {
        ops_per_sec = THROTTLE_UNLIMITED_RATE;
    }
    tokenBucket_setRate(t->bucket, ops_per_sec);

    if (ops_per_sec <= THROTTLE_OPS_PER_SEC_GUARDRAIL) {
        if (t->rate_below_guardrail_since == 0) {
            elapsedStart(&t->rate_below_guardrail_since);
        }
    } else {
        t->rate_below_guardrail_since = 0;
    }
}

double throttle_adjustRate(throttler *t, double multiplier) {
    serverAssert(multiplier >= 0.0 && multiplier <= 3.0);
    double current = tokenBucket_getRate(t->bucket);

    /* No change needed if already unlimited and trying to increase. */
    if (multiplier >= 1.0 && current == THROTTLE_UNLIMITED_RATE) return current;

    double new_rate;

    if (multiplier < 1.0) {
        /* Decrease: apply the multiplier to the current rate. If the result still exceeds the
         * measured incoming TPS, reduce it directly to that rate. */
        new_rate = current * multiplier;
        double incoming = tpsCalculator_averageTps(t->metrics->incoming_tps);
        /* If there is no incoming rate, it's possible that the tps calculator hasn't been populated with
         * data yet. Otherwise, if there's actually no incoming traffic, it doesn't matter if the
         * rate is adjusted. */
        if (incoming > EPSILON && new_rate > incoming) new_rate = incoming;
    } else if (current < EPSILON) {
        /* Coming back from halted: jump to a sensible starting rate. */
        new_rate = MIN_ADJUST_AFTER_DISABLE;
    } else {
        /* Increase: proportional with minimum step of 1 ops/sec. */
        double delta = current * (multiplier - 1.0);
        if (delta < 1.0) delta = 1.0;
        new_rate = current + delta;
    }

    if (new_rate != current) throttle_setRate(t, new_rate);
    return tokenBucket_getRate(t->bucket);
}

void throttle_removeClient(client *c) {
    if (!c->flag.throttled) return;

    throttler *t = c->throttler;
    dequeueThrottledClient(c);

    if (listLength(t->client_queue) == 0) {
        serverAssert(t->time_event_id != AE_DELETED_EVENT_ID);
        aeDeleteTimeEvent(server.el, t->time_event_id);
        t->time_event_id = AE_DELETED_EVENT_ID;
        if (t->cleanup) freeThrottler(t);
    }
}

bool throttle_throttleClientIfNeeded(client *c) {
    /* Skip internal clients and clients already checked for this command.
     * Prevents re-throttling after unblocking. */
    if (!c->conn || c->flag.throttle_checked) return false;
    c->flag.throttle_checked = 1;

    if (throttler_list == NULL || listLength(throttler_list) == 0) return false;

    bool need_throttle = false;
    int match_count = 0;
    /* Strictest throttler is the applicable throttler with the lowest rate.
     * It is the most restrictive throttler the client needs to throttle at.
     */
    throttler *strictest = NULL;
    listNode *ln;
    listIter li;
    listRewind(throttler_list, &li);
    while ((ln = listNext(&li))) {
        throttler *t = ln->value;
        if (t->cleanup) continue;

        if (t->criteria_proc(c, t->priv_data)) {
            match_count++;
            tpsCalculator_record(t->metrics->incoming_tps, 1);
            if (strictest == NULL || tokenBucket_getRate(t->bucket) < tokenBucket_getRate(strictest->bucket)) strictest = t;
        }
    }

    if (strictest != NULL) {
        if (listLength(strictest->client_queue) == 0 &&
            tokenBucket_tryConsume(strictest->bucket, 1.0, false)) {
            /* token available, consume and let command proceed. */
            if (match_count > 1) consumeOtherThrottlers(c, strictest);
        } else {
            /* no token available, defer the command. */
            if (match_count > 1) c->flag.throttle_multi = 1;
            throttlerAddClient(strictest, c);
            need_throttle = true;
        }
    }

    return need_throttle;
}

/* === INFO metrics output === */
void throttle_getMetrics(const char *metrics_name, throttleMetrics *metrics) {
    metricsEntry *m = findMetrics(metrics_name);

    metrics->num_clients_throttled = m->num_clients_throttled;
    metrics->num_commands_throttled = m->num_commands_throttled;
    metrics->incoming_tps = tpsCalculator_averageTps(m->incoming_tps);
    metrics->ops_per_sec = 0.0;
    metrics->oldest_client_delay_us = 0;

    /* Aggregate ops_per_sec and oldest_client from all throttlers sharing this metrics. */
    listNode *ln;
    listIter li;
    listRewind(throttler_list, &li);
    while ((ln = listNext(&li))) {
        throttler *t = ln->value;
        if (t->metrics != m || t->cleanup) continue;
        metrics->ops_per_sec += tokenBucket_getRate(t->bucket);
        if (listLength(t->client_queue) > 0) {
            client *oldest = listNodeValue(listFirst(t->client_queue));
            long delay_us = elapsedUs(oldest->throttle_start);
            metrics->oldest_client_delay_us = MAX(metrics->oldest_client_delay_us, delay_us);
        }
    }
}

sds throttle_sdscatInfoMetrics(sds info) {
    info = sdscatprintf(info, "total_throttled_commands:%lld\r\n", total_throttled_commands);

    /* Report the longest-below-guardrail throttler per metrics type. */
    dict *guardrail_agg = NULL;
    listNode *ln;
    listIter li;
    listRewind(throttler_list, &li);
    while ((ln = listNext(&li))) {
        throttler *t = ln->value;
        if (t->cleanup || t->rate_below_guardrail_since == 0) continue;

        if (guardrail_agg == NULL) guardrail_agg = dictCreate(&guardrailAggDictType);
        dictEntry *existing;
        dictEntry *de = dictAddRaw(guardrail_agg, t->metrics, &existing);
        if (de != NULL) {
            /* First seen for this type. */
            dictSetUnsignedIntegerVal(de, t->rate_below_guardrail_since);
        } else if (t->rate_below_guardrail_since < dictGetUnsignedIntegerVal(existing)) {
            /* Keep the earliest start. */
            dictSetUnsignedIntegerVal(existing, t->rate_below_guardrail_since);
        }
    }

    if (guardrail_agg != NULL) {
        dictIterator it;
        dictInitIterator(&it, guardrail_agg);
        dictEntry *de;
        while ((de = dictNext(&it)) != NULL) {
            metricsEntry *m = dictGetKey(de);
            int secs = elapsedSec((monotime)dictGetUnsignedIntegerVal(de));
            info = sdscatprintf(info, "throttle_%s_guardrail_secs:%d\r\n", m->throttler_type, secs);
        }
        dictRelease(guardrail_agg);
    }
    return info;
}

long throttle_getGuardrailSecs(throttler *t) {
    if (t == NULL || t->rate_below_guardrail_since == 0) return 0;
    return (long)elapsedSec(t->rate_below_guardrail_since);
}
