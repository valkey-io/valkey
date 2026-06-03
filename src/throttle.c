/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "throttle.h"
#include "throttle_token_bucket.h"
#include "throttle_stat_calc.h"
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

/* === Throttler instance === */

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

static int listMatchThrottler(void *ptr, void *id) {
    return ((throttler *)ptr)->id == (long long)id;
}

/* === Lookup === */
static throttler *findThrottler(int id) {
    listNode *ln = listSearchKey(throttlerList, (void *)(long long)id);
    serverAssert(ln != NULL);
    throttler *t = ln->value;
    serverAssert(t->ln == ln);
    return t;
}

/* === Bucket sizing === */
static double computeBucketSize(double tokens_per_sec, double burst_time_sec) {
    return (tokens_per_sec < EPSILON) ? 0.0
                                      : 2.0 + tokens_per_sec * burst_time_sec;
}

static void replenishTokens(throttler *t) {
    if (t->id == THROTTLE_CLEANUP_ID) {
        tokenBucket_setTokensPerSec(t->bucket, THROTTLE_UNLIMITED_RATE);
        tokenBucket_add(t->bucket, THROTTLE_UNLIMITED_RATE);
        return;
    }
    tokenBucket_replenish(t->bucket);
    tokenBucket_capDebt(t->bucket, tokenBucket_getBucketSize(t->bucket));
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
    /* metrics is shared — We do not free here */
    zfree(t);
}

/* === Rate setting (with guardrail tracking) === */

static void setRate(throttler *t, double new_rate) {
    if (new_rate < EPSILON) {
        tokenBucket_halt(t->bucket);
    } else {
        if (new_rate > THROTTLE_UNLIMITED_RATE) new_rate = THROTTLE_UNLIMITED_RATE;
        tokenBucket_setTokensPerSec(t->bucket, new_rate);
    }

    double rate_per_min = tokenBucket_getTokensPerSec(t->bucket) * 60.0;
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

/* === Metrics lookup/create === */

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
                      const char *metrics_name,
                      double ops_per_sec) {
    serverAssert(criteria_proc != NULL);
    serverAssert(metrics_name != NULL);
    serverAssert(ops_per_sec >= 0);
    validateAlphaNumeric(metrics_name);
    serverAssert(nextThrottlerId > 0);

    throttler *t = zmalloc(sizeof(throttler));
    t->id = nextThrottlerId++;
    t->criteria_proc = criteria_proc;
    t->time_event_id = AE_DELETED_EVENT_ID;
    t->priv_data = priv_data;
    t->bucket = tokenBucket_create(ops_per_sec, TOKENS_BURST_RATE_SEC, computeBucketSize);
    t->metrics = findMetrics(metrics_name);
    t->client_queue = listCreate();
    t->rate_below_guardrail_since = 0;
    setRate(t, ops_per_sec);

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

void *throttle_setPrivData(int id, void *new_priv_data) {
    throttler *t = findThrottler(id);
    void *old = t->priv_data;
    t->priv_data = new_priv_data;
    return old;
}

void throttle_setRate(int id, double ops_per_sec) {
    serverAssert(ops_per_sec >= 0);
    throttler *t = findThrottler(id);
    setRate(t, ops_per_sec);
}

double throttle_adjustRate(int id, double multiplier) {
    serverAssert(multiplier >= 0.0 && multiplier <= 3.0);
    throttler *t = findThrottler(id);

    double throttle_rate = tokenBucket_getTokensPerSec(t->bucket);
    double new_rate;

    if (multiplier <= 1.0) {
        new_rate = throttle_rate * multiplier;
        double incoming_rate = tpsCalculator_averageTps(t->metrics->incoming_tps);
        if (incoming_rate > EPSILON && new_rate < incoming_rate) {
            new_rate = incoming_rate;
        }
    } else {
        if (throttle_rate == THROTTLE_UNLIMITED_RATE) {
            new_rate = throttle_rate;
        } else if (throttle_rate < EPSILON) {
            new_rate = MIN_ADJUST_AFTER_DISABLE;
        } else {
            double delta = throttle_rate * (multiplier - 1.0);
            if (delta < 1.0) delta = 1.0;
            new_rate = throttle_rate + delta;
        }
    }

    if (new_rate != throttle_rate) setRate(t, new_rate);
    return tokenBucket_getTokensPerSec(t->bucket);
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
        result.ops_per_sec += tokenBucket_getTokensPerSec(t->bucket);
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

/* === Multi-throttler token accounting === */

static void consumeOtherThrottlers(client *c, throttler *except) {
    listNode *ln;
    listIter li;
    listRewind(throttlerList, &li);
    while ((ln = listNext(&li))) {
        throttler *t = ln->value;
        if (t->id == THROTTLE_CLEANUP_ID) continue;
        if (t == except) continue;
        if (t->criteria_proc(c, t->priv_data)) {
            tokenBucket_consume(t->bucket, 1.0);
        }
    }
}

/* === Timer: drain the queue when tokens become available === */

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

    while (tokenBucket_canConsume(t->bucket, 1.0) &&
           listLength(t->client_queue) > 0 &&
           elapsedMs(work_start) < MAX_UNTHROTTLE_PROCESSING_TIME_MS) {
        tokenBucket_consume(t->bucket, 1.0);
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

/* === Queue management === */

static void throttlerAddClient(throttler *t, client *c) {
    serverAssert(c->throttler == NULL);
    serverAssert(!c->flag.throttled);
    elapsedStart(&c->throttle_start_us);
    c->flag.throttled = 1;
    listAddNodeTail(t->client_queue, c);

    if (c->conn) connSetReadHandler(c->conn, NULL);

    t->metrics->num_clients++;
    t->metrics->total_throttled_commands++;
    server.total_throttled_commands++;
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

/* === Per-command entry point === */

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
                tokenBucket_getTokensPerSec(t->bucket) < tokenBucket_getTokensPerSec(strictest->bucket)) {
                strictest = t;
            }
        }
    }

    if (strictest == NULL) return false;

    /* Fast path: queue empty AND a token is available. */
    if (listLength(strictest->client_queue) == 0) {
        replenishTokens(strictest);
        if (tokenBucket_canConsume(strictest->bucket, 1.0)) {
            if (match_count > 1) consumeOtherThrottlers(c, strictest);
            tokenBucket_consume(strictest->bucket, 1.0);
            return false;
        }
    }

    if (match_count > 1) c->flag.throttle_multi = 1;
    throttlerAddClient(strictest, c);
    return true;
}

/* === INFO output === */
// Harry TODO: Should we do the info based on overall metrics or single throttler
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
