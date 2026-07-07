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
#include <ctype.h>

#define MAX_WAIT_TIME_MS 100
#define MAX_UNTHROTTLE_PROCESSING_TIME_MS 10
#define THROTTLE_CLEANUP_ID (-1)
#define THROTTLE_OPS_PER_MIN_GUARDRAIL 6
#define TPS_WINDOW_SEC 5
#define EPSILON 0.0001
#define TOKENS_BURST_RATE_SEC 0.1
#define MIN_ADJUST_AFTER_DISABLE 100.0

/* Framework-level metrics */
struct throttle_framework_metrics throttle_framework_metrics;

/* === Internal metrics (shared by name via hashtable) === */
typedef struct throttleInternalMetrics {
    sds name;
    int num_clients;
    int total_throttled_commands;
    tpsCalculator *incoming_tps;
} throttleInternalMetrics;

/* Metrics hashtable callbacks. */
static const void *metricsGetKey(const void *entry) {
    return ((throttleInternalMetrics *)entry)->name;
}

static void metricsDestructor(void *entry) {
    throttleInternalMetrics *m = entry;
    sdsfree(m->name);
    tpsCalculator_free(m->incoming_tps);
    zfree(m);
}

static hashtableType metricsHashtableType = {
    .entryGetKey = metricsGetKey,
    .hashFunction = dictSdsHash,
    .keyCompare = dictSdsKeyCompare,
    .entryDestructor = metricsDestructor,
};

static int nextThrottlerId = 1;
static list *throttlerList = NULL;
static hashtable *metricsTable = NULL;

typedef struct throttler {
    int id;
    throttleCriteriaProc *criteria_proc;
    long long time_event_id;
    void *priv_data;
    tokenBucket *bucket;
    list *client_queue;
    listNode *ln; /* my node in throttlerList */
    monotime rate_below_guardrail_since;
    throttleInternalMetrics *metrics;
} throttler;

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

static void replenishTokens(throttler *t) {
    if (t->id == THROTTLE_CLEANUP_ID) {
        tokenBucket_setRate(t->bucket, THROTTLE_UNLIMITED_RATE);
    }
}

static int waitTimeMs(throttler *t) {
    serverAssert(listLength(t->client_queue) > 0);
    double ms = tokenBucket_msUntilAvailable(t->bucket, 1.0);
    if (ms < 0) return MAX_WAIT_TIME_MS;
    return MIN(MAX_WAIT_TIME_MS, (int)ceil(ms));
}

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

static void setRate(throttler *t, double new_rate) {
    if (new_rate < EPSILON) {
        tokenBucket_setRate(t->bucket, 0);
    } else {
        if (new_rate > THROTTLE_UNLIMITED_RATE) new_rate = THROTTLE_UNLIMITED_RATE;
        tokenBucket_setRate(t->bucket, new_rate);
    }

    double rate_per_min = tokenBucket_getRate(t->bucket) * 60.0;
    if (rate_per_min <= THROTTLE_OPS_PER_MIN_GUARDRAIL) {
        if (t->rate_below_guardrail_since == 0) {
            elapsedStart(&t->rate_below_guardrail_since);
        }
    } else {
        t->rate_below_guardrail_since = 0;
    }
}

static void validateAlphaNumeric(const char *s) {
    for (; *s; s++) {
        serverAssert(isalnum(*s) || (*s == '_') || (*s == '-'));
    }
}

static throttleInternalMetrics *findMetrics(const char *name) {
    sds key = sdsnew(name);
    void *found = NULL;
    if (hashtableFind(metricsTable, key, &found)) {
        sdsfree(key);
        return (throttleInternalMetrics *)found;
    }
    throttleInternalMetrics *m = zmalloc(sizeof(throttleInternalMetrics));
    m->name = key;
    m->num_clients = 0;
    m->total_throttled_commands = 0;
    m->incoming_tps = tpsCalculator_create(TPS_WINDOW_SEC);
    hashtableAdd(metricsTable, m);
    return m;
}

static void consumeOtherThrottlers(client *c, throttler *except) {
    listNode *ln;
    listIter li;
    listRewind(throttlerList, &li);
    while ((ln = listNext(&li))) {
        throttler *t = ln->value;
        if (t->id == THROTTLE_CLEANUP_ID) continue;
        if (t == except) continue;
        if (t->criteria_proc(c, t->priv_data)) {
            tokenBucket_tryConsume(t->bucket, 1.0, true);
        }
    }
}

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

static long long throttlerTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    UNUSED(eventLoop);
    UNUSED(id);
    if (isPausedActionsWithUpdate(PAUSE_ACTIONS_CLIENT_ALL_SET)) return 1;

    throttler *t = (throttler *)clientData;
    replenishTokens(t);

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

    t->metrics->num_clients++;
    t->metrics->total_throttled_commands++;
    throttle_framework_metrics.total_throttled_commands++;
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

int throttle_register(throttleCriteriaProc *criteria_proc,
                      void *priv_data,
                      const char *metrics_name) {
    serverAssert(criteria_proc != NULL);
    serverAssert(metrics_name != NULL);
    validateAlphaNumeric(metrics_name);
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
    setRate(t, THROTTLE_UNLIMITED_RATE);

    listAddNodeTail(throttlerList, t);
    t->ln = listLast(throttlerList);
    return t->id;
}

void throttle_deregister(int id) {
    serverAssert(throttlerList != NULL && listLength(throttlerList) > 0);
    throttler *t = findThrottler(id);

    if (listLength(t->client_queue) == 0) {
        freeThrottler(t);
    } else {
        t->id = THROTTLE_CLEANUP_ID;
    }
}

void throttle_setRate(int id, double ops_per_sec) {
    serverAssert(ops_per_sec >= 0);
    throttler *t = findThrottler(id);
    setRate(t, ops_per_sec);
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
        if (incoming > EPSILON && new_rate < incoming) {
            new_rate = incoming;
        }
    } else if (current < EPSILON) {
        /* Coming back from halted: jump to a sensible starting rate. */
        new_rate = MIN_ADJUST_AFTER_DISABLE;
    } else {
        /* Increase: proportional with minimum step of 1 ops/sec. */
        double delta = current * (multiplier - 1.0);
        if (delta < 1.0) delta = 1.0;
        new_rate = current + delta;
    }

    if (new_rate != current) setRate(t, new_rate);
    return tokenBucket_getRate(t->bucket);
}

const throttleMetrics *throttle_getMetrics(const char *metrics_name) {
    static throttleMetrics result;
    throttleInternalMetrics *m = findMetrics(metrics_name);

    result.num_clients = m->num_clients;
    result.total_throttled_commands = m->total_throttled_commands;
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
            if (result.oldest_client_delay_us < delay_us) {
                result.oldest_client_delay_us = delay_us;
            }
        }
    }
    return &result;
}

void throttle_removeClient(client *c) {
    if (!c->flag.throttled) return;

    c->flag.throttled = 0;
    throttler *t = c->throttler;
    serverAssert(t != NULL);

    listDelNode(t->client_queue, c->throttle_node);

    if (listLength(t->client_queue) == 0) {
        serverAssert(t->time_event_id != AE_DELETED_EVENT_ID);
        aeDeleteTimeEvent(server.el, t->time_event_id);
        t->time_event_id = AE_DELETED_EVENT_ID;
    }
    t->metrics->num_clients--;
    c->throttler = NULL;
    c->throttle_node = NULL;
    c->throttle_start_us = 0;
}

bool throttle_deferCommand(client *c) {
    if (throttlerList == NULL || listLength(throttlerList) == 0) return false;
    // Exempt all internal commands that has no connection from throttling.
    if (!c->conn) return false;
    if (c->flag.throttle_checked) return false;
    c->flag.throttle_checked = 1;

    int match_count = 0;
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
            if (strictest == NULL ||
                tokenBucket_getRate(t->bucket) < tokenBucket_getRate(strictest->bucket)) {
                strictest = t;
            }
        }
    }

    if (strictest == NULL) return false;

    if (listLength(strictest->client_queue) == 0) {
        if (tokenBucket_tryConsume(strictest->bucket, 1.0, false)) {
            if (match_count > 1) consumeOtherThrottlers(c, strictest);
            return false;
        }
    }

    if (match_count > 1) c->flag.throttle_multi = 1;
    throttlerAddClient(strictest, c);
    return true;
}

/* === INFO output === */
sds throttle_sdscatMetrics(sds info) {
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
                                    t->metrics->name, secs);
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
