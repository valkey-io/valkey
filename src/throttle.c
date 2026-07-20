/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "throttle.h"
#include "throttle_token_bucket.h"
#include "stat_calc.h"
#include "hashtable.h"
#include "monotonic.h"

#include <math.h>

#define MAX_WAIT_TIME_MS 100                 /* max ms before rescheduling timer */
#define MAX_UNTHROTTLE_PROCESSING_TIME_MS 10 /* max ms spent unthrottling per timer fire */
#define THROTTLE_CLEANUP_ID (-1)             /* sentinel: throttler deregistered, draining queue */
#define THROTTLE_OPS_PER_MIN_GUARDRAIL 6     /* 0.1 TPS - report when rate stays below this */
#define TPS_WINDOW_SEC 5                     /* rolling window for incoming TPS measurement */
#define EPSILON 0.0001                       /* values below this are treated as zero */
#define TOKENS_BURST_RATE_SEC 0.1            /* burst capacity in seconds of sustained rate */
#define MIN_ADJUST_AFTER_DISABLE 100.0       /* initial rate when recovering from halted state */

static int nextThrottlerId = 1;
static hashtable *metricsTable = NULL;
static list *throttlerList = NULL;

typedef struct metricsEntry {
    sds throttler_type;
    int num_clients_throttled;
    int num_throttled_commands;
    tpsCalculator *incoming_tps;
} metricsEntry;

typedef struct throttler {
    int id;
    throttleCriteriaProc *criteria_proc; /* callback defining throttling criteria */
    long long time_event_id;             /* timer event id for throttlerTimeProc */
    void *priv_data;                     /* private data for use by the criteria_proc */
    tokenBucket *bucket;                 /* token bucket: 1 token = 1 operation */
    list *client_queue;                  /* clients currently queued for throttling */
    listNode *ln;                        /* my node in throttlerList */
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

static metricsEntry *findMetrics(const char *name) {
    sds key = sdsnew(name);
    void *found = NULL;
    if (hashtableFind(metricsTable, key, &found)) {
        sdsfree(key);
        return (metricsEntry *)found;
    }
    metricsEntry *m = zmalloc(sizeof(metricsEntry));
    m->throttler_type = key;
    m->num_clients_throttled = 0;
    m->num_throttled_commands = 0;
    m->incoming_tps = newTpsCalc(TPS_WINDOW_SEC);
    hashtableAdd(metricsTable, m);
    return m;
}

/* Framework-level metrics */
static long long total_throttled_commands;

static int listMatchThrottler(void *throttler_ptr, void *id) {
    return ((throttler *)throttler_ptr)->id == (long)id;
}

static throttler *findThrottler(int id) {
    listNode *ln = listSearchKey(throttlerList, (void *)(long)id);
    serverAssert(ln != NULL);
    throttler *t = ln->value;
    serverAssert(t->ln == ln);
    return t;
}

/* Compute how long to wait before the next token becomes available. */
static int waitTimeMs(throttler *t) {
    serverAssert(listLength(t->client_queue) > 0);
    double ms = tokenBucket_msUntilAvailable(t->bucket, 1.0);
    if (ms < 0 || ms >= MAX_WAIT_TIME_MS) return MAX_WAIT_TIME_MS;
    return (int)ceil(ms);
}

/* Release throttler resources. Only called when client queue is fully drained. */
static void freeThrottler(throttler *t) {
    serverAssert(listLength(t->client_queue) == 0);
    serverAssert(t->time_event_id == AE_DELETED_EVENT_ID);
    serverAssert(t->ln != NULL);
    listDelNode(throttlerList, t->ln);
    listRelease(t->client_queue);
    tokenBucket_free(t->bucket);
    /* metrics is shared and do not free here */
    zfree(t);
}


static void consumeOtherThrottlers(client *c, throttler *except) {
    listNode *ln;
    listIter li;
    listRewind(throttlerList, &li);
    while ((ln = listNext(&li))) {
        throttler *t = ln->value;
        if (t->id == THROTTLE_CLEANUP_ID || t == except) continue;
        if (t->criteria_proc(c, t->priv_data)) tokenBucket_tryConsume(t->bucket, 1.0, true);
    }
}

/* Re-execute a client's deferred command after throttle release.
 * Restores the read handler and processes the pending command and input buffer. */
static void processUnthrottledClient(client *c) {
    serverAssert(c->argc > 0 && c->flag.pending_command && !c->flag.throttled);
    if (c->conn && !connHasReadHandler(c->conn)) {
        if (connSetReadHandler(c->conn, readQueryFromClient) == C_ERR) {
            freeClient(c);
            return;
        }
    }
    if (processPendingCommandAndInputBuffer(c) == C_OK) beforeNextClient(c);
}

/* Timer event handler: releases queued clients at the token bucket rate.
 * Processes clients until tokens are exhausted or time budget is spent. */
static long long throttlerTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    UNUSED(eventLoop);
    UNUSED(id);
    // if the clients are paused, then return 1 ms so we wake up every ms
    if (isPausedActionsWithUpdate(PAUSE_ACTIONS_CLIENT_ALL_SET)) return 1;

    throttler *t = (throttler *)clientData;

    monotime work_start;
    elapsedStart(&work_start);

    while (listLength(t->client_queue) > 0 &&
           elapsedMs(work_start) < MAX_UNTHROTTLE_PROCESSING_TIME_MS &&
           tokenBucket_tryConsume(t->bucket, 1.0, false)) {
        client *c = listNodeValue(listFirst(t->client_queue));
        throttle_removeClient(c);
        if (c->flag.throttle_multi) {
            c->flag.throttle_multi = 0;
            consumeOtherThrottlers(c, t);
        }
        processUnthrottledClient(c);
    }

    if (listLength(t->client_queue) == 0) {
        serverAssert(t->time_event_id == AE_DELETED_EVENT_ID); // Already set in throttle_removeClient
        if (t->id == THROTTLE_CLEANUP_ID) freeThrottler(t);
        return AE_NOMORE;
    }
    return waitTimeMs(t);
}

static void throttlerAddClient(throttler *t, client *c) {
    serverAssert(c->throttler == NULL);
    serverAssert(!c->flag.throttled);
    elapsedStart(&c->throttle_start_us);
    c->flag.throttled = 1;
    listAddNodeTail(t->client_queue, c);

    if (c->conn) connSetReadHandler(c->conn, NULL);

    t->metrics->num_clients_throttled++;
    t->metrics->num_throttled_commands++;
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
    if (throttlerList == NULL) {
        throttlerList = listCreate();
        listSetMatchMethod(throttlerList, listMatchThrottler);
    }
    if (metricsTable == NULL) {
        metricsTable = hashtableCreate(&metricsHashtableType);
    }
}

/* In most cases, each throttler should have its own independent metrics_name. When the same
 * throttler is instantiated multiple times (with different priv_data), they may share a single
 * metrics object by using the same name. This allows statistics to be aggregated across related
 * throttler instances. */
int throttle_register(throttleCriteriaProc *criteria_proc,
                      void *priv_data,
                      const char *metrics_name) {
    serverAssert(criteria_proc != NULL);
    serverAssert(metrics_name != NULL);
    serverAssert(nextThrottlerId > 0);

    throttler *t = zmalloc(sizeof(throttler));
    t->id = nextThrottlerId++;
    t->criteria_proc = criteria_proc;
    t->time_event_id = AE_DELETED_EVENT_ID;
    t->priv_data = priv_data;
    t->bucket = tokenBucket_create(THROTTLE_UNLIMITED_RATE, TOKENS_BURST_RATE_SEC);
    t->metrics = findMetrics(metrics_name);
    t->client_queue = listCreate();
    t->rate_below_guardrail_since = 0;
    listAddNodeTail(throttlerList, t);
    t->ln = listLast(throttlerList);
    throttle_setRate(t->id, THROTTLE_UNLIMITED_RATE);
    return t->id;
}

void throttle_deregister(int id) {
    serverAssert(throttlerList != NULL && listLength(throttlerList) > 0);
    throttler *t = findThrottler(id);

    if (listLength(t->client_queue) == 0) {
        freeThrottler(t);
    } else {
        t->id = THROTTLE_CLEANUP_ID;
        tokenBucket_setRate(t->bucket, THROTTLE_UNLIMITED_RATE);
    }
}

void throttle_setRate(int id, double ops_per_sec) {
    serverAssert(ops_per_sec >= 0);
    throttler *t = findThrottler(id);

    if (ops_per_sec < EPSILON) {
        ops_per_sec = 0;
    } else if (ops_per_sec > THROTTLE_UNLIMITED_RATE) {
        ops_per_sec = THROTTLE_UNLIMITED_RATE;
    }
    tokenBucket_setRate(t->bucket, ops_per_sec);

    double rate_per_min = ops_per_sec * 60.0;
    if (rate_per_min <= THROTTLE_OPS_PER_MIN_GUARDRAIL) {
        if (t->rate_below_guardrail_since == 0) {
            elapsedStart(&t->rate_below_guardrail_since);
        }
    } else {
        t->rate_below_guardrail_since = 0;
    }
}

double throttle_adjustRate(int id, double multiplier) {
    serverAssert(multiplier >= 0.0 && multiplier <= 3.0);
    throttler *t = findThrottler(id);
    double current = tokenBucket_getRate(t->bucket);

    /* No change needed if already unlimited and trying to increase. */
    if (multiplier > 1.0 && current == THROTTLE_UNLIMITED_RATE) {
        return current;
    }

    double new_rate;

    if (multiplier <= 1.0) {
        /* Decrease: plain multiply, but never drop below incoming TPS. */
        new_rate = current * multiplier;
        double incoming = tpsCalculator_averageTps(t->metrics->incoming_tps);
        if (incoming > EPSILON && new_rate < incoming) new_rate = incoming;
    } else if (current < EPSILON) {
        /* Coming back from halted: jump to a sensible starting rate. */
        new_rate = MIN_ADJUST_AFTER_DISABLE;
    } else {
        /* Increase: proportional with minimum step of 1 ops/sec. */
        double delta = current * (multiplier - 1.0);
        if (delta < 1.0) delta = 1.0;
        new_rate = current + delta;
    }

    if (new_rate != current) throttle_setRate(t->id, new_rate);
    return tokenBucket_getRate(t->bucket);
}

void throttle_removeClient(client *c) {
    if (!c->flag.throttled) return;

    c->flag.throttled = 0;
    throttler *t = c->throttler;
    serverAssert(t != NULL);

    listDelNode(t->client_queue, c->throttle_node);

    t->metrics->num_clients_throttled--;

    if (listLength(t->client_queue) == 0) {
        serverAssert(t->time_event_id != AE_DELETED_EVENT_ID);
        aeDeleteTimeEvent(server.el, t->time_event_id);
        t->time_event_id = AE_DELETED_EVENT_ID;
        if (t->id == THROTTLE_CLEANUP_ID) freeThrottler(t);
    }
    c->throttler = NULL;
    c->throttle_node = NULL;
    c->throttle_start_us = 0;
}

bool throttleClientIfNeeded(client *c) {
    if (throttlerList == NULL || listLength(throttlerList) == 0) return false;

    /* Skip internal clients and clients already checked for this command.
     * Prevents re-throttling after unblocking. */
    if (!c->conn || c->flag.throttle_checked) return false;
    c->flag.throttle_checked = 1;

    bool need_throttle = false;
    int match_count = 0;
    /* Strictest throttler is the applicable throttler with the lowest rate.
     * It is the most restrictive throttler the client needs to throttle at.
     */
    throttler *strictest = NULL;
    listNode *ln;
    listIter li;
    listRewind(throttlerList, &li);
    while ((ln = listNext(&li))) {
        throttler *t = ln->value;
        if (t->id == THROTTLE_CLEANUP_ID) continue;

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
long long throttle_getTotalThrottledCommands(void) {
    return total_throttled_commands;
}

const throttleMetrics *throttle_getMetrics(const char *metrics_name) {
    static throttleMetrics result;
    metricsEntry *m = findMetrics(metrics_name);

    result.num_clients_throttled = m->num_clients_throttled;
    result.num_throttled_commands = m->num_throttled_commands;
    result.incoming_tps = tpsCalculator_averageTps(m->incoming_tps);
    result.ops_per_sec = 0.0;
    result.oldest_client_delay_us = 0;

    /* Aggregate ops_per_sec and oldest_client from all throttlers sharing this metrics. */
    listNode *ln;
    listIter li;
    listRewind(throttlerList, &li);
    while ((ln = listNext(&li))) {
        throttler *t = ln->value;
        if (t->metrics != m) continue;
        result.ops_per_sec += tokenBucket_getRate(t->bucket);
        if (listLength(t->client_queue) > 0) {
            client *oldest = listNodeValue(listFirst(t->client_queue));
            long delay_us = elapsedUs(oldest->throttle_start_us);
            result.oldest_client_delay_us = MAX(result.oldest_client_delay_us, delay_us);
        }
    }
    return &result;
}

sds throttle_sdscatInfoMetrics(sds info) {
    // Check for any throttlers which are below guardrail.  Report only offending throttlers.
    listNode *ln;
    listIter li;
    listRewind(throttlerList, &li);
    while ((ln = listNext(&li))) {
        throttler *t = ln->value;
        if (t->rate_below_guardrail_since != 0) {
            int secs = elapsedSec(t->rate_below_guardrail_since);
            if (secs > 0) {
                info = sdscatprintf(info,
                                    "throttle_%s_guardrail_secs:%d\r\n",
                                    t->metrics->throttler_type, secs);
            }
        }
    }
    return info;
}

long throttle_getGuardrailSecs(int id) {
    throttler *t = findThrottler(id);
    if (t == NULL || t->rate_below_guardrail_since == 0) return 0;
    return (long)elapsedSec(t->rate_below_guardrail_since);
}
