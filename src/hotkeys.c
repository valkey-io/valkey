/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "server.h"
#include "hotkeys.h"
#include "cluster.h"
#include "monotonic.h"
#include "space_saving.h"

/* ---------------------------------------------------------------------------
 * Hot-key detection
 *
 * A frozen-window Space-Saving manager (spaceSavingManager, see space_saving.h)
 * does the heavy lifting: it tracks the top-K (key, db) pairs, keeping a live
 * window accumulating the current `hotkeys-window-seconds` and a frozen snapshot
 * of the last completed window, which is what HOTKEYS GET reports. This file
 * supplies the policy around it: the sampling/enable configuration, the
 * invalidation predicates, and the HOTKEYS commands.
 * --------------------------------------------------------------------------*/

/* Create a frozen-window manager sized and timed from the current config. */
static spaceSavingManager *hotkeyCreateManager(void) {
    uint64_t window_us = (uint64_t)server.hotkey_window_seconds * 1000000ULL;
    spaceSavingManager *m = spaceSavingManagerCreate(server.hotkey_top_k, window_us, getMonotonicUs());
    if (m) spaceSavingManagerSetLiveSamplingPercentage(m, server.hotkey_sampling_percentage);
    return m;
}

/* ===========================================================================
 * Invalidation helpers
 * ==========================================================================*/

void hotkeyPurgeAll(void) {
    if (!server.hotkey_manager) return;
    /* Reset preserves the live window's sampling percentage, so there is nothing
     * to re-establish here. */
    spaceSavingManagerReset(server.hotkey_manager, getMonotonicUs());
}

/* Periodic maintenance from serverCron: close any window that has fully elapsed
 * so a completed window is frozen on schedule even when there is no traffic (and
 * so any future window-boundary work — history, notifications — has a place to
 * hang). Cheap: a subtract and a compare unless a boundary was actually crossed.
 * No-op when detection is disabled. */
void hotkeyCron(void) {
    if (server.hotkey_manager) spaceSavingManagerRotate(server.hotkey_manager, getMonotonicUs());
}

/* The cluster hash slot is not stored per entry — it is derived from the key
 * name on demand, only when a slot-scoped purge asks for it. */
static int hotkeyItemInSlot(sds key, int dbid, void *arg) {
    UNUSED(dbid);
    return (int)keyHashSlot(key, (int)sdslen(key)) == *(int *)arg;
}

static int hotkeyItemInDb(sds key, int dbid, void *arg) {
    UNUSED(key);
    return dbid == *(int *)arg;
}

/* Drop every entry on `slot` from both windows, so a removed slot's keys
 * disappear from reports immediately and do not resurface on rotation. */
void hotkeyPurgeSlot(int slot) {
    if (server.hotkey_manager) spaceSavingManagerRemoveIf(server.hotkey_manager, hotkeyItemInSlot, &slot);
}

/* Drop every entry in database `dbid` from both windows. */
void hotkeyPurgeDb(int dbid) {
    if (server.hotkey_manager) spaceSavingManagerRemoveIf(server.hotkey_manager, hotkeyItemInDb, &dbid);
}

/* Note: RENAME / MOVE / SWAPDB are intentionally NOT re-attributed. An entry is
 * keyed by (key name, db), so after one of these commands a tracked entry keeps
 * its old identity and may briefly be reported under the pre-command name/db.
 * This is accepted for simplicity: the stale entry is harmless and ages out
 * with the window — it stops accruing new hits immediately and disappears once
 * the window rotates (from the live window on the next rotation, from the
 * frozen snapshot one rotation later), so it lingers at most for the reporting
 * window. */

/* ===========================================================================
 * Per-access detection hook
 * ==========================================================================*/

/* Record one sampled access (read or write) of `key` in database `dbid`. */
static void recordHotKeySample(robj *key, int dbid) {
    spaceSavingManager *m = server.hotkey_manager;
    if (!m || !key) return;
    sds k = objectGetVal(key);
    if (!k) return;
    recordSpaceSavingManagerSample(m, k, dbid);
}

/* True when the current activity is a genuine client executing a command — a
 * real client that is actually processing a command, and is not the replication
 * link/AOF, and not RDB/AOF loading, and not an administrative bulk slot
 * deletion (delKeysInSlot, e.g. CLUSTER FLUSHSLOT / slot migration — that is not
 * user key access and must not feed or evict the sampler). Importing traffic is
 * user-driven load and is counted. */
static bool hotkeyShouldRecord(void) {
    client *c = server.current_client;
    return c != NULL && c->flag.executing_command && !mustObeyClient(c) && !server.loading &&
           !server.server_del_keys_in_slot;
}

/* Charge a sampled read/write access of `key` in `dbid`, for a lookup carrying
 * `lookup_flags` (LOOKUP_*).
 *
 * Lookups flagged LOOKUP_NOHOTKEY are skipped as introspection (OBJECT, DEBUG,
 * the cluster redirect lookup). Note this tests that dedicated bit and NOT
 * LOOKUP_NOEFFECTS, which is a mask of several flags: a lookup carrying only
 * LOOKUP_NOTOUCH (EXISTS/TYPE/TTL, or any hit from a CLIENT NO-TOUCH client) is
 * a genuine client access. */
void hotkeyRecordLookup(robj *key, int dbid, int lookup_flags) {
    if (!hotkeyEnabled() || (lookup_flags & LOOKUP_NOHOTKEY)) return;
    if (!hotkeyShouldRecord()) return;
    if (!bernoulliSampleHit(server.hotkey_sampling_percentage)) return;
    recordHotKeySample(key, dbid);
}

/* Charge a sampled removal of `key` in `dbid`. `del_flags` are the DB_FLAG_*
 * deletion reasons: only a genuine client-issued DEL/UNLINK counts, not passive
 * expiry or eviction (DB_FLAG_KEY_EXPIRED / DB_FLAG_KEY_EVICTED). A deletion is
 * activity on the key, so it is charged like any other access. */
void hotkeyRecordDelete(robj *key, int dbid, int del_flags) {
    if (!hotkeyEnabled() || !(del_flags & DB_FLAG_KEY_DELETED)) return;
    if (!hotkeyShouldRecord()) return;
    if (!bernoulliSampleHit(server.hotkey_sampling_percentage)) return;
    recordHotKeySample(key, dbid);
}

/* ===========================================================================
 * HOTKEYS commands
 * ==========================================================================*/

typedef struct {
    sds key;
    uint64_t qps;
    int dbid;
} hotkeyCollected;

static int hotkeyCollectedCmpDesc(const void *a, const void *b) {
    const hotkeyCollected *ea = a;
    const hotkeyCollected *eb = b;
    if (eb->qps > ea->qps) return 1;
    if (eb->qps < ea->qps) return -1;
    return 0;
}

/* Compute (a * b) / c rounded to nearest, without overflowing the intermediate
 * product. Uses a 128-bit intermediate where the compiler has one (as
 * monotonic.c does); the uint64 fallback is exact for every reachable input,
 * since overflowing it would take upwards of 9e10 sampled hits on one key
 * inside a single window. */
static uint64_t hotkeyMulDivRound(uint64_t a, uint64_t b, uint64_t c) {
#ifdef __SIZEOF_INT128__
    __uint128_t num = (__uint128_t)a * b;
    return (uint64_t)((num + c / 2) / c);
#else
    return (a * b + c / 2) / c;
#endif
}

/* Recover a per-second rate from a frozen (count, error) pair whose counts were
 * Bernoulli-sampled at `sample_percentage` percent over a window that really
 * lasted `duration_us` microseconds. Uses the midpoint of the [count-error,
 * count] band (the *2 keeps error/2 exact) and scales the sampled count back up
 * by 100/sample_percentage.
 *
 * The denominator is the window's MEASURED duration, not the configured
 * `hotkeys-window-seconds`. Rotation is driven by serverCron, so a window is
 * closed at or after its nominal boundary and holds the traffic of that whole
 * real interval; dividing by the nominal length would over-report by the
 * rotation lag (up to ~1/server.hz, i.e. ~10% at the default hz with a 1s
 * window) and always in the same direction. Integer arithmetic, rounded to
 * nearest; 0 for non-positive inputs. */
static uint64_t hotkeyEstimateQps(uint64_t count, uint64_t error, int sample_percentage, uint64_t duration_us) {
    if (sample_percentage <= 0 || duration_us == 0) return 0;
    uint64_t twice_midpoint = 2 * count - error;
    uint64_t den = 2ULL * (uint64_t)sample_percentage * duration_us;
    return hotkeyMulDivRound(twice_midpoint, 100ULL * 1000000ULL, den);
}

void hotkeysGetCommand(client *c) {
    /* Report an empty result rather than an error when detection is off, as
     * SLOWLOG GET and LATENCY HISTORY do: a polling client then has one shape to
     * parse and does not have to match on an error string to tell "disabled"
     * from "nothing is hot". */
    if (!hotkeyEnabled()) {
        addReplyArrayLen(c, 0);
        return;
    }
    /* Detection is enabled, so the manager must already exist (created by
     * hotkeyInit / the config callbacks whenever top-k is turned on). */
    spaceSavingManager *m = server.hotkey_manager;
    serverAssert(m != NULL);

    /* Close any window that has fully elapsed so we report the latest
     * completed window. */
    spaceSavingManagerRotate(m, getMonotonicUs());

    int cap = spaceSavingManagerCount(m);
    if (cap == 0) {
        addReplyArrayLen(c, 0);
        return;
    }

    hotkeyCollected *arr = zmalloc(cap * sizeof(hotkeyCollected));
    /* Estimate with the sampling percentage that produced the frozen window (the
     * current config may have changed since) and the interval it really spanned. */
    int frozen_pct = spaceSavingManagerFrozenSamplingPercentage(m);
    uint64_t frozen_duration_us = spaceSavingManagerFrozenDurationUs(m);
    for (int i = 0; i < cap; i++) {
        uint64_t count, error;
        spaceSavingManagerAt(m, i, &arr[i].key, &arr[i].dbid, &count, &error);
        arr[i].qps = hotkeyEstimateQps(count, error, frozen_pct, frozen_duration_us);
    }

    qsort(arr, cap, sizeof(hotkeyCollected), hotkeyCollectedCmpDesc);

    int limit = cap < server.hotkey_top_k ? cap : server.hotkey_top_k;
    addReplyArrayLen(c, limit);
    for (int j = 0; j < limit; j++) {
        addReplyMapLen(c, 3);
        addReplyBulkCString(c, "key");
        addReplyBulkCBuffer(c, arr[j].key, sdslen(arr[j].key));
        addReplyBulkCString(c, "db");
        addReplyLongLong(c, arr[j].dbid);
        addReplyBulkCString(c, "qps");
        addReplyLongLong(c, arr[j].qps);
    }
    zfree(arr);
}

void hotkeysResetCommand(client *c) {
    /* Nothing to clear when detection is off; still report success, so callers
     * need not special-case the disabled state. */
    if (hotkeyEnabled()) hotkeyPurgeAll();
    addReply(c, shared.ok);
}

void hotkeysHelpCommand(client *c) {
    const char *help[] = {
        "GET",
        "    Return the hottest keys of the last completed window, ordered by",
        "    estimated accesses per second (descending). Each entry reports the",
        "    key name, the database it was accessed in, and the estimated QPS.",
        "RESET",
        "    Clear all collected hot key statistics.",
        NULL,
    };
    addReplyHelp(c, help);
}

/* ===========================================================================
 * Generic hotkey API
 * ==========================================================================*/

/* Is hot-key detection currently enabled? Tracking zero keys is the same thing
 * as not tracking, so `hotkeys-top-k` doubles as the on/off switch: 0 disables
 * detection, any positive value enables it and sets the Space-Saving capacity.
 * The sampling percentage only sets how much traffic is sampled while enabled. */
bool hotkeyEnabled(void) {
    return server.hotkey_top_k > 0;
}

/* Number of sampled observations in the last completed window (N). The
 * Space-Saving guarantee is stated relative to N: only keys with frequency
 * above N/K are guaranteed tracked, so operators use it to gauge the detection
 * floor and how much to trust a given entry. 0 when detection is disabled. */
uint64_t hotkeyLastWindowSamples(void) {
    return server.hotkey_manager ? spaceSavingManagerFrozenTotal(server.hotkey_manager) : 0;
}

/* Real duration of the last completed window, in microseconds. Reported in INFO
 * so an operator can tell an empty report apart from one measured over an
 * unusually short or long window (rotation runs on the serverCron tick, so the
 * span is the configured length plus that lag). 0 means there is no completed
 * window: detection was just enabled or reset, or the last window was dropped
 * for spanning more than twice the configured length — those cases are not
 * distinguishable from this field alone. */
uint64_t hotkeyLastWindowDurationUs(void) {
    return server.hotkey_manager ? spaceSavingManagerFrozenDurationUs(server.hotkey_manager) : 0;
}

/* Reconfigure the manager in place from the current config: the in-progress
 * (live) window is reset (its counts were gathered under the old config), but
 * the last completed (frozen) window is KEPT along with the config that
 * produced it, so an operator's in-flight HOTKEYS GET still sees it. No-op when
 * detection is disabled (no manager). Use HOTKEYS RESET to discard everything. */
static void hotkeyManagerReconfigure(void) {
    if (!server.hotkey_manager) return;
    spaceSavingManagerReconfigure(server.hotkey_manager, server.hotkey_top_k,
                                  (uint64_t)server.hotkey_window_seconds * 1000000ULL, getMonotonicUs());
    spaceSavingManagerSetLiveSamplingPercentage(server.hotkey_manager, server.hotkey_sampling_percentage);
}

/* Create or free the manager to match the enabled state. */
static void hotkeyManagerSetEnabled(int enabled) {
    if (enabled && !server.hotkey_manager) {
        server.hotkey_manager = hotkeyCreateManager();
    } else if (!enabled && server.hotkey_manager) {
        spaceSavingManagerRelease(server.hotkey_manager);
        server.hotkey_manager = NULL;
    }
}

/* Bring up hot-key detection at server startup (creates the manager if enabled). */
void hotkeyInit(void) {
    hotkeyManagerSetEnabled(hotkeyEnabled());
}

/* ===========================================================================
 * Config callbacks
 * ==========================================================================*/

/* Sampling percentage only changes how much traffic is sampled; reconfigure in
 * place so a live query still sees the last completed window (no-op if disabled). */
int hotKeySamplingCallback(const char **err) {
    UNUSED(err);
    hotkeyManagerReconfigure();
    return 1;
}

/* top-k is also the on/off switch (0 disables), so it drives the manager
 * lifecycle: crossing 0 creates or frees it, while a change that stays enabled
 * reconfigures in place and keeps the last completed window. */
int hotKeyTopKCallback(const char **err) {
    UNUSED(err);
    if (hotkeyEnabled() && server.hotkey_manager)
        hotkeyManagerReconfigure();
    else
        hotkeyManagerSetEnabled(hotkeyEnabled());
    return 1;
}

int hotKeyWindowCallback(const char **err) {
    UNUSED(err);
    hotkeyManagerReconfigure();
    return 1;
}
