/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "server.h"
#include "hotkey.h"
#include "cluster.h"
#include "space_saving.h"

/* ---------------------------------------------------------------------------
 * Hot-key detection
 *
 * A generic frozen-window Space-Saving manager (spaceSavingManager, see
 * space_saving.h) does the heavy lifting: it keeps a live window accumulating
 * the current `hotkey-window-seconds` and a frozen snapshot of the last
 * completed window, which is what HOTKEYS GET reports. This file supplies only
 * the Valkey specifics: the hot-key item type (key name + db), the
 * sampling/enable policy, config wiring, and the HOTKEYS commands.
 * --------------------------------------------------------------------------*/

/* ===========================================================================
 * Tracked item: (key name, db)
 *
 * The Space-Saving core is item-agnostic; the hot-key identity is a key name
 * plus the database it lives in. The cluster hash slot, when needed for
 * slot-scoped invalidation, is derived from the key on demand.
 * ==========================================================================*/

typedef struct {
    sds key;
    int dbid;
} hotkeyItem;

static int hotkeyItemCmp(const void *a, const void *b) {
    const hotkeyItem *x = a, *y = b;
    if (x->dbid != y->dbid) return 1;
    return sdscmp(x->key, y->key); /* 0 == equal */
}

static void *hotkeyItemDup(const void *a) {
    const hotkeyItem *x = a;
    hotkeyItem *c = zmalloc(sizeof(*c));
    c->key = sdsdup(x->key);
    c->dbid = x->dbid;
    return c;
}

static void hotkeyItemFree(void *a) {
    hotkeyItem *x = a;
    sdsfree(x->key);
    zfree(x);
}

/* FNV-1a over the key name, folded with the db id so identical names in
 * different databases fast-reject without a full compare. */
static uint32_t hotkeyItemHash(const void *a) {
    const hotkeyItem *x = a;
    uint32_t h = 2166136261u;
    size_t klen = sdslen(x->key);
    for (size_t i = 0; i < klen; i++) {
        h ^= (unsigned char)x->key[i];
        h *= 16777619u;
    }
    return h ^ (uint32_t)x->dbid;
}

static spaceSavingType hotkeyItemType = {hotkeyItemCmp, hotkeyItemDup, hotkeyItemFree, hotkeyItemHash};

/* Create a frozen-window manager sized and timed from the current config. */
static spaceSavingManager *hotkeyCreateManager(void) {
    return spaceSavingManagerCreate(server.hotkey_top_k, (uint64_t)server.hotkey_window_seconds * 1000000ULL,
                                    (uint64_t)server.ustime, &hotkeyItemType);
}

/* ===========================================================================
 * Invalidation helpers
 * ==========================================================================*/

void hotkeyPurgeAll(void) {
    if (server.hotkey_manager) spaceSavingManagerReset(server.hotkey_manager, (uint64_t)server.ustime);
}

static int hotkeyItemInSlot(const void *item, void *arg) {
    const hotkeyItem *hi = item;
    return (int)keyHashSlot(hi->key, (int)sdslen(hi->key)) == *(int *)arg;
}

static int hotkeyItemInDb(const void *item, void *arg) {
    return ((const hotkeyItem *)item)->dbid == *(int *)arg;
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

/* Record one sampled access (read or write) of `key` in database `dbid`. Builds
 * the tracked item, bumps the sampled-count metric, and feeds the manager. */
void recordHotKeySample(robj *key, int dbid) {
    spaceSavingManager *m = server.hotkey_manager;
    if (!m || !key) return;
    sds k = objectGetVal(key);
    if (!k) return;
    hotkeyItem probe = {k, dbid};
    recordSpaceSavingManagerSample(m, (uint64_t)server.ustime, &probe);
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

void hotkeysGetCommand(client *c) {
    if (!hotkeyEnabled()) {
        addReplyError(c, "Hotkey detection is disabled");
        return;
    }
    /* Detection is enabled, so the manager must already exist (created by
     * hotkeyInit / the config callbacks whenever sampling is turned on). */
    spaceSavingManager *m = server.hotkey_manager;
    serverAssert(m != NULL);

    /* Close any window that has fully elapsed so we report the latest
     * completed window. */
    spaceSavingManagerRotate(m, (uint64_t)server.ustime);

    int cap = spaceSavingManagerCount(m);
    if (cap == 0) {
        addReplyArrayLen(c, 0);
        return;
    }

    hotkeyCollected *arr = zmalloc(cap * sizeof(hotkeyCollected));
    int n = 0;
    for (int i = 0; i < cap; i++) {
        void *item;
        uint64_t count, error;
        spaceSavingManagerAt(m, i, &item, &count, &error);
        hotkeyItem *hi = item;
        arr[n].key = hi->key;
        arr[n].dbid = hi->dbid;
        arr[n].qps = spaceSavingEstimateRate(count, error, server.hotkey_sampling_percentage, server.hotkey_window_seconds);
        n++;
    }

    qsort(arr, n, sizeof(hotkeyCollected), hotkeyCollectedCmpDesc);

    int limit = n < server.hotkey_top_k ? n : server.hotkey_top_k;
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
    if (!hotkeyEnabled()) {
        addReplyError(c, "Hotkey detection is disabled");
        return;
    }
    hotkeyPurgeAll();
    addReply(c, shared.ok);
}

/* ===========================================================================
 * Generic hotkey API
 * ==========================================================================*/

/* Is hot-key detection currently enabled? Detection is on whenever a non-zero
 * sampling percentage is configured — there is no separate on/off switch. */
int hotkeyEnabled(void) {
    return server.hotkey_sampling_percentage > 0;
}

/* Tear down any existing manager and, if detection is enabled, create a fresh
 * one from the current config. Called at startup and on every hotkey config
 * change, so the manager always reflects the latest sampling / top-k / window
 * (recreating discards the in-progress window, which is fine for a rare admin
 * config change). */
static void hotkeyManagerRefresh(void) {
    if (server.hotkey_manager) {
        spaceSavingManagerRelease(server.hotkey_manager);
        server.hotkey_manager = NULL;
    }
    if (hotkeyEnabled()) server.hotkey_manager = hotkeyCreateManager();
}

/* Bring up hot-key detection at server startup (creates the manager if
 * detection is enabled). */
void hotkeyInit(void) {
    hotkeyManagerRefresh();
}

/* ===========================================================================
 * Config callbacks — every hotkey config change refreshes the manager.
 * ==========================================================================*/

int hotKeySamplingCallback(const char **err) {
    UNUSED(err);
    hotkeyManagerRefresh();
    return 1;
}

int hotKeyTopKCallback(const char **err) {
    UNUSED(err);
    hotkeyManagerRefresh();
    return 1;
}

int hotKeyWindowCallback(const char **err) {
    UNUSED(err);
    hotkeyManagerRefresh();
    return 1;
}
