/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "server.h"
#include "traffic.h"
#include "cluster.h"
#include "monotonic.h"
#include "space_saving.h"

/* ---------------------------------------------------------------------------
 * Per-key traffic tracking
 *
 * A weighted Space-Saving manager (spaceSavingManager, see space_saving.h)
 * tracks the top-K (key, db) pairs by estimated bytes moved: every sampled
 * read or write charges the accessed value's estimated size as its weight, so
 * a moderately-read large value outranks a heavily-read tiny one — exactly the
 * keys that saturate bandwidth while never making the QPS leaderboard
 * (HOTKEYS) or a size scan (big keys). TRAFFIC GET reports the last completed
 * window as estimated bytes per second.
 *
 * Byte accounting is an approximation by design:
 *  - reads: the whole stored value is charged. Exact for whole-value reads
 *    (GET/HGETALL/LRANGE 0 -1); a deliberate overestimate for partial reads
 *    (HGET/LRANGE 0 9) — conservative in the direction of flagging the key.
 *  - writes: whole-value stores via setKey (SET/MSET/GETSET/...) charge the
 *    written value, so a write-heavy load on fresh keys is fully visible.
 *    In-place growth of an existing value (APPEND, HSET on an existing hash)
 *    is not charged — an accepted underestimate of a small path.
 *  - deletions move no bytes and are not charged.
 *  - module values are not charged (their size callback has unbounded cost).
 * Each entry splits its bytes by direction — read_bytes_per_second (egress:
 * values sent out) vs write_bytes_per_second (ingress: values stored) — via the
 * manager's secondary accumulator, because the two directions have different
 * causes and different fixes. Ranking stays by the combined total, so the
 * top-K guarantee is unchanged. The split is derived ONLY from observations
 * recorded under the entry's current identity: the slot's [count - error]
 * lower bound is exactly the weight observed here (insert, hit and eviction
 * all preserve that invariant), so eviction-inherited uncertainty is reported
 * separately as unattributed_bytes_per_second instead of being silently
 * booked as reads.
 * Sampling compensates globally: the reported rate scales the sampled weight
 * back up by 100/percentage, as hot-key detection does for counts.
 *
 * A second manager aggregates the same observations by SOURCE instead of by
 * key: TRAFFIC GET CLIENTS ranks clients (by name when CLIENT SETNAME was
 * used, else by ip:port) so bandwidth can be attributed to the connection
 * producing it. Behind a proxy every client looks like the proxy itself;
 * source attribution then belongs at the proxy.
 * --------------------------------------------------------------------------*/

/* Element probes per container size estimate — the same bound MEMORY USAGE
 * defaults to (OBJ_COMPUTE_SIZE_DEF_SAMPLES, private to object.c). */
#define TRAFFIC_SIZE_ESTIMATE_SAMPLES 5

/* Create a frozen-window manager sized and timed from the current config. */
static spaceSavingManager *trafficCreateManager(void) {
    uint64_t window_us = (uint64_t)server.traffic_window_seconds * 1000000ULL;
    spaceSavingManager *m = spaceSavingManagerCreate(server.traffic_top_k, window_us, getMonotonicUs());
    if (m) spaceSavingManagerSetLiveSamplingPercentage(m, server.traffic_sampling_percentage);
    return m;
}

/* The by-source (clients) manager aggregates the same observations as the
 * by-key one; identity is the client name when set, else its peer id. */

/* ===========================================================================
 * Invalidation helpers
 * ==========================================================================*/

void trafficPurgeAll(void) {
    if (!server.traffic_manager && !server.traffic_clients_manager) return;
    /* Reset preserves the live windows' sampling percentage, so there is
     * nothing to re-establish here. */
    if (server.traffic_manager) spaceSavingManagerReset(server.traffic_manager, getMonotonicUs());
    if (server.traffic_clients_manager) spaceSavingManagerReset(server.traffic_clients_manager, getMonotonicUs());
}

/* Periodic maintenance from databasesCron: close any window that has fully
 * elapsed so a completed window is frozen on schedule even when there is no
 * traffic. Cheap: a subtract and a compare unless a boundary was crossed.
 * No-op when tracking is disabled. */
void trafficCron(void) {
    if (server.traffic_manager) spaceSavingManagerRotate(server.traffic_manager, getMonotonicUs());
    if (server.traffic_clients_manager) spaceSavingManagerRotate(server.traffic_clients_manager, getMonotonicUs());
}

/* The cluster hash slot is not stored per entry — it is derived from the key
 * name on demand, only when a slot-scoped purge asks for it. */
static int trafficItemInSlot(sds key, int dbid, void *arg) {
    UNUSED(dbid);
    return (int)keyHashSlot(key, (int)sdslen(key)) == *(int *)arg;
}

static int trafficItemInDb(sds key, int dbid, void *arg) {
    UNUSED(key);
    return dbid == *(int *)arg;
}

/* Drop every entry on `slot` from both windows, so a removed slot's keys
 * disappear from reports immediately and do not resurface on rotation. */
void trafficPurgeSlot(int slot) {
    if (server.traffic_manager) spaceSavingManagerRemoveIf(server.traffic_manager, trafficItemInSlot, &slot);
}

/* Drop every entry in database `dbid` from both windows. */
void trafficPurgeDb(int dbid) {
    if (server.traffic_manager) spaceSavingManagerRemoveIf(server.traffic_manager, trafficItemInDb, &dbid);
}

/* ===========================================================================
 * Per-access detection hook
 * ==========================================================================*/

/* True when the current activity is a genuine client executing a command — a
 * real client that is actually processing a command, and is not the replication
 * link/AOF, and not RDB/AOF loading, and not an administrative bulk slot
 * deletion. Mirrors hotkeysShouldRecord() in hotkeys.c; kept as a copy so the
 * two dimensions stay independent modules that can be backported separately. */
static bool trafficShouldRecord(void) {
    client *c = server.current_client;
    return c != NULL && c->flag.executing_command && !mustObeyClient(c) && !server.loading &&
           !server.server_del_keys_in_slot;
}

/* Estimated bytes one access of `val` moves. See the module header for the
 * approximation; strings are exact and O(1), containers are sampled, module
 * values are skipped (their size callback can do arbitrary work). */
static uint64_t trafficEstimateValueBytes(robj *key, robj *val, int dbid) {
    if (!val) return 0;
    int type = objectGetType(val);
    if (type == OBJ_STRING) return (uint64_t)stringObjectLen(val);
    if (type == OBJ_MODULE) return 0;
    return (uint64_t)objectComputeSize(key, val, TRAFFIC_SIZE_ESTIMATE_SAMPLES, dbid);
}

/* Identity for the by-source view: the client's name when one was set, else
 * its cached peer id (ip:port). Both are borrowed from the client, which
 * outlives this call; the manager copies the string only when a slot is
 * committed to it. */
static sds trafficClientIdentity(void) {
    client *cl = server.current_client;
    if (cl->name) return objectGetVal(cl->name);
    return (sds)getClientPeerId(cl);
}

static void trafficRecordSample(robj *key, int dbid, uint64_t weight, uint64_t weight_write) {
    if (weight == 0) return;
    if (server.traffic_manager && key) {
        sds k = objectGetVal(key);
        if (k) recordSpaceSavingManagerSampleWeighted(server.traffic_manager, k, dbid, weight, weight_write);
    }
    if (server.traffic_clients_manager) {
        sds identity = trafficClientIdentity();
        if (identity) recordSpaceSavingManagerSampleWeighted(server.traffic_clients_manager, identity, 0, weight, weight_write);
    }
}

/* Charge a sampled read of `key` in `dbid` whose lookup returned `val`.
 *
 * Write lookups (LOOKUP_WRITE) are skipped: the bytes a write moves are the
 * value being stored, which the setKey hook charges from the written value —
 * charging the lookup too would double-count and miss fresh keys (whose
 * lookup is a miss). Lookups flagged LOOKUP_NOHOTKEYS are introspection
 * (OBJECT, DEBUG, the cluster redirect lookup) and are skipped, sharing the
 * flag hot-key detection uses for the same purpose. */
void trafficRecordLookup(robj *key, int dbid, int lookup_flags, robj *val) {
    if (!trafficEnabled() || (lookup_flags & (LOOKUP_NOHOTKEYS | LOOKUP_WRITE))) return;
    if (!trafficShouldRecord()) return;
    if (!bernoulliSampleHit(server.traffic_sampling_percentage)) return;
    uint64_t bytes = trafficEstimateValueBytes(key, val, dbid);
    trafficRecordSample(key, dbid, bytes, 0); /* a read moves egress bytes only */
}

/* Charge a sampled write of `key` in `dbid` storing value `val` — the setKey
 * path, so both a fresh key and an overwrite are charged the bytes written,
 * all of it in the write (ingress) direction. */
void trafficRecordSetKey(robj *key, int dbid, robj *val) {
    if (!trafficEnabled()) return;
    if (!trafficShouldRecord()) return;
    if (!bernoulliSampleHit(server.traffic_sampling_percentage)) return;
    uint64_t bytes = trafficEstimateValueBytes(key, val, dbid);
    trafficRecordSample(key, dbid, bytes, bytes);
}

/* ===========================================================================
 * TRAFFIC commands
 * ==========================================================================*/

/* Compute (a * b) / c rounded to nearest, without overflowing the intermediate
 * product. Uses a 128-bit intermediate where the compiler has one; the 64-bit
 * fallback is divide-first (see the comment on it below). Mirrors
 * hotkeysMulDivRound() in hotkeys.c; see the independence note on
 * trafficShouldRecord(). Exposed for unit tests. */
uint64_t trafficMulDivRound(uint64_t a, uint64_t b, uint64_t c) {
    if (c == 0) return 0;
#ifdef __SIZEOF_INT128__
    __uint128_t num = (__uint128_t)a * b;
    return (uint64_t)((num + c / 2) / c);
#else
    /* Overflow-safe fallback for compilers without a 128-bit type: divide
     * before multiplying. (a*b)/c == (a/c)*b + ((a%c)*b)/c, where (a/c)*b is
     * bounded by the (fitted) result and (a%c)*b < c*b. Unlike the hot-key
     * count path, byte weights reach a wrap-around with ordinary values: 172
     * sampled accesses to a 512 MiB value inside a one-second window already
     * put twice_midpoint * 1e8 near 2^64. c*b stays below 2^64 because every
     * caller uses b == 1e8 and c <= 2 * 100 * 300e6 (max sampling % x max
     * window), so c * b <= 6e18. */
    uint64_t q = a / c;
    uint64_t r = a % c;
    return q * b + (r * b + c / 2) / c;
#endif
}

/* Recover estimated bytes per second from a frozen (count, error) byte pair
 * whose bytes were Bernoulli-sampled at `sample_percentage` percent over a
 * window that really lasted `duration_us` microseconds: the [count-error,
 * count] midpoint scaled back up by 100/sample_percentage. The denominator is
 * the window's MEASURED duration, not the configured length — rotation runs on
 * serverCron, so a window is closed at or after its nominal boundary and holds
 * the traffic of that whole real interval. Mirrors hotkeysEstimateQps() in
 * hotkeys.c. Pass error == 0 to scale an exactly-observed weight. */
static uint64_t trafficEstimateBytesPerSecond(uint64_t count, uint64_t error, int sample_percentage, uint64_t duration_us) {
    if (sample_percentage <= 0 || duration_us == 0) return 0;
    uint64_t twice_midpoint = 2 * count - error;
    uint64_t den = 2ULL * (uint64_t)sample_percentage * duration_us;
    return trafficMulDivRound(twice_midpoint, 100ULL * 1000000ULL, den);
}

typedef struct {
    sds key;                   /* key name, or the client identity in the clients view */
    uint64_t bps;              /* combined bytes per second (the ranking key) */
    uint64_t read_bps;         /* egress share, from observations under this identity */
    uint64_t write_bps;        /* ingress share, from observations under this identity */
    uint64_t unattributed_bps; /* eviction-inherited uncertainty; no direction */
    int dbid;
} trafficCollected;

static int trafficCollectedCmpDesc(const void *a, const void *b) {
    const trafficCollected *ea = a;
    const trafficCollected *eb = b;
    if (eb->bps > ea->bps) return 1;
    if (eb->bps < ea->bps) return -1;
    return 0;
}

/* Read the frozen window of `m`, estimate per-second figures, sort descending
 * by the combined figure and reply up to top-k entries. `clients_view` labels
 * the identity field "client" and drops the db (a source has no database).
 *
 * Direction split: only observations recorded under the entry's current
 * identity are attributed. count - error is EXACTLY that observed weight
 * (insert, hit and eviction all preserve the invariant), count2 is its write
 * share, and the read share is the remainder — so a key that was evicted into
 * a slot reports zero reads even though its combined estimate inherited the
 * predecessor's mass. The inherited uncertainty (half the error band) is
 * reported as unattributed_bytes_per_second rather than booked as either
 * direction, and the three fields add up to bytes_per_second exactly. */
static void trafficReportManager(client *c, spaceSavingManager *m, int clients_view) {
    /* Close any window that has fully elapsed so we report the latest
     * completed window. */
    spaceSavingManagerRotate(m, getMonotonicUs());

    int cap = spaceSavingManagerCount(m);
    if (cap == 0) {
        addReplyArrayLen(c, 0);
        return;
    }

    trafficCollected *arr = zmalloc(cap * sizeof(trafficCollected));
    /* Estimate with the sampling percentage that produced the frozen window
     * (the current config may have changed since) and the interval it really
     * spanned. */
    int frozen_pct = spaceSavingManagerFrozenSamplingPercentage(m);
    uint64_t frozen_duration_us = spaceSavingManagerFrozenDurationUs(m);
    for (int i = 0; i < cap; i++) {
        uint64_t count, error, count2;
        spaceSavingManagerAt2(m, i, &arr[i].key, &arr[i].dbid, &count, &error, &count2);
        arr[i].bps = trafficEstimateBytesPerSecond(count, error, frozen_pct, frozen_duration_us);
        arr[i].write_bps = trafficEstimateBytesPerSecond(count2, 0, frozen_pct, frozen_duration_us);
        /* The observed (non-inherited) read weight: the lower bound minus the
         * observed write share. count2 <= count - error holds by construction,
         * so this never underflows. */
        uint64_t read_weight = (count - error) - count2;
        arr[i].read_bps = trafficEstimateBytesPerSecond(read_weight, 0, frozen_pct, frozen_duration_us);
        /* Rounding of the three estimates can let read+write exceed the
         * combined figure by one; shave the excess off read so the reply keeps
         * exact additivity and stays non-negative. */
        uint64_t excess = arr[i].read_bps + arr[i].write_bps > arr[i].bps
                              ? arr[i].read_bps + arr[i].write_bps - arr[i].bps
                              : 0;
        if (excess > arr[i].read_bps) excess = arr[i].read_bps;
        arr[i].read_bps -= excess;
        arr[i].unattributed_bps = arr[i].bps - arr[i].read_bps - arr[i].write_bps;
    }

    qsort(arr, cap, sizeof(trafficCollected), trafficCollectedCmpDesc);

    int limit = cap < server.traffic_top_k ? cap : server.traffic_top_k;
    addReplyArrayLen(c, limit);
    for (int j = 0; j < limit; j++) {
        addReplyMapLen(c, clients_view ? 5 : 6);
        addReplyBulkCString(c, clients_view ? "client" : "key");
        addReplyBulkCBuffer(c, arr[j].key, sdslen(arr[j].key));
        if (!clients_view) {
            addReplyBulkCString(c, "db");
            addReplyLongLong(c, arr[j].dbid);
        }
        addReplyBulkCString(c, "bytes_per_second");
        addReplyLongLong(c, arr[j].bps);
        addReplyBulkCString(c, "read_bytes_per_second");
        addReplyLongLong(c, arr[j].read_bps);
        addReplyBulkCString(c, "write_bytes_per_second");
        addReplyLongLong(c, arr[j].write_bps);
        addReplyBulkCString(c, "unattributed_bytes_per_second");
        addReplyLongLong(c, arr[j].unattributed_bps);
    }
    zfree(arr);
}

/* TRAFFIC GET [CLIENTS]: the per-key view by default, the per-source view with
 * CLIENTS. */
void trafficGetCommand(client *c) {
    int clients_view = 0;
    if (c->argc >= 3) {
        if (c->argc > 3 || strcasecmp(objectGetVal(c->argv[2]), "CLIENTS") != 0) {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
        clients_view = 1;
    }
    /* Report an empty result rather than an error when tracking is off, as
     * HOTKEYS GET does: a polling client has one shape to parse and does not
     * have to match on an error string to tell "disabled" from "no traffic". */
    if (!trafficEnabled()) {
        addReplyArrayLen(c, 0);
        return;
    }
    /* Tracking is enabled, so both managers must already exist (created by
     * trafficInit / the config callbacks whenever top-k is turned on). */
    spaceSavingManager *m = clients_view ? server.traffic_clients_manager : server.traffic_manager;
    serverAssert(m != NULL);
    trafficReportManager(c, m, clients_view);
}

void trafficResetCommand(client *c) {
    /* Nothing to clear when tracking is off; still report success, so callers
     * need not special-case the disabled state. */
    if (trafficEnabled()) trafficPurgeAll();
    addReply(c, shared.ok);
}

void trafficHelpCommand(client *c) {
    const char *help[] = {
        "GET [CLIENTS]",
        "    Return the keys that moved the most estimated bytes in the last",
        "    completed window, ordered by estimated bytes per second (descending).",
        "    Each entry reports the key name, the database it was accessed in,",
        "    the estimated bytes per second, and its split into read (egress),",
        "    write (ingress) and unattributed (eviction-inherited) bytes per",
        "    second. With CLIENTS, rank the sources of the traffic instead:",
        "    client names when CLIENT SETNAME was used, else ip:port.",
        "RESET",
        "    Clear all collected traffic statistics.",
        NULL,
    };
    addReplyHelp(c, help);
}

/* ===========================================================================
 * Generic traffic API
 * ==========================================================================*/

/* Is traffic tracking currently enabled? Tracking zero keys is the same thing
 * as not tracking, so `traffic-top-k` doubles as the on/off switch: 0 disables
 * tracking, any positive value enables it and sets the Space-Saving capacity.
 * The sampling percentage only sets how much traffic is sampled while enabled. */
bool trafficEnabled(void) {
    return server.traffic_top_k > 0;
}

/* Total sampled bytes in the last completed window (N). The Space-Saving
 * guarantee is stated relative to N: only keys whose byte share exceeds N/K
 * are guaranteed tracked, so operators use it to gauge the detection floor
 * and how much to trust a given entry. 0 when tracking is disabled. */
static uint64_t trafficLastWindowBytes(void) {
    return server.traffic_manager ? spaceSavingManagerFrozenTotal(server.traffic_manager) : 0;
}

/* Real duration of the last completed window, in microseconds. 0 means there
 * is no completed window: tracking was just enabled or reset, or the last
 * window was dropped for spanning more than twice the configured length. */
static uint64_t trafficLastWindowDurationUs(void) {
    return server.traffic_manager ? spaceSavingManagerFrozenDurationUs(server.traffic_manager) : 0;
}

/* Bytes sampled in the in-progress window so far. Partial by definition — the
 * window has not closed — which is the point: a human (or a dashboards probe)
 * can watch traffic accumulate in real time instead of waiting for the freeze
 * that makes it reportable. 0 when tracking is disabled. */
static uint64_t trafficLiveWindowBytes(void) {
    return server.traffic_manager ? spaceSavingManagerLiveTotal(server.traffic_manager) : 0;
}

/* Append the fields of the INFO "traffic" section. The caller emits the
 * section header; this owns which fields the section carries. */
sds genTrafficInfoString(sds info) {
    /* N for the last completed window (sampled bytes — scale by 100/percentage
     * for the real total): the detection floor of a report. */
    info = sdscatprintf(info, "traffic_last_window_bytes:%llu\r\n", (unsigned long long)trafficLastWindowBytes());
    /* The real span the report was measured over, and the bytes/sec
     * denominator. */
    info =
        sdscatprintf(info, "traffic_last_window_duration_ms:%llu\r\n",
                     (unsigned long long)(trafficLastWindowDurationUs() / 1000));
    /* The real-time peek into the still-open window. */
    info = sdscatprintf(info, "traffic_live_window_bytes:%llu\r\n", (unsigned long long)trafficLiveWindowBytes());
    return info;
}

/* Reconfigure the manager in place from the current config: the in-progress
 * (live) window is reset (its counts were gathered under the old config), but
 * the last completed (frozen) window is KEPT along with the config that
 * produced it, so an operator's in-flight TRAFFIC GET still sees it. No-op
 * when tracking is disabled (no manager). Use TRAFFIC RESET to discard
 * everything. */
static void trafficManagerReconfigure(void) {
    spaceSavingManager *ms[2] = {server.traffic_manager, server.traffic_clients_manager};
    for (int i = 0; i < 2; i++) {
        if (!ms[i]) continue;
        spaceSavingManagerReconfigure(ms[i], server.traffic_top_k, (uint64_t)server.traffic_window_seconds * 1000000ULL,
                                      getMonotonicUs());
        spaceSavingManagerSetLiveSamplingPercentage(ms[i], server.traffic_sampling_percentage);
    }
}

/* Create or free the manager to match the enabled state. */
static void trafficManagerSetEnabled(int enabled) {
    if (enabled) {
        if (!server.traffic_manager) server.traffic_manager = trafficCreateManager();
        if (!server.traffic_clients_manager) server.traffic_clients_manager = trafficCreateManager();
    } else {
        if (server.traffic_manager) {
            spaceSavingManagerRelease(server.traffic_manager);
            server.traffic_manager = NULL;
        }
        if (server.traffic_clients_manager) {
            spaceSavingManagerRelease(server.traffic_clients_manager);
            server.traffic_clients_manager = NULL;
        }
    }
}

/* Bring up traffic tracking at server startup (creates the manager if enabled). */
void trafficInit(void) {
    trafficManagerSetEnabled(trafficEnabled());
}

/* ===========================================================================
 * Config callbacks
 * ==========================================================================*/

/* Sampling percentage only changes how much traffic is sampled; reconfigure in
 * place so a live query still sees the last completed window (no-op if disabled). */
int trafficSamplingCallback(const char **err) {
    UNUSED(err);
    trafficManagerReconfigure();
    return 1;
}

/* top-k is also the on/off switch (0 disables), so it drives the manager
 * lifecycle: crossing 0 creates or frees it, while a change that stays enabled
 * reconfigures in place and keeps the last completed window. */
int trafficTopKCallback(const char **err) {
    UNUSED(err);
    if (trafficEnabled() && server.traffic_manager)
        trafficManagerReconfigure();
    else
        trafficManagerSetEnabled(trafficEnabled());
    return 1;
}

int trafficWindowCallback(const char **err) {
    UNUSED(err);
    trafficManagerReconfigure();
    return 1;
}
