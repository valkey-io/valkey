/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Space-Saving top-K over fixed time windows. See space_saving.h for the
 * algorithm, the ownership model, and usage.
 *
 * Storage is a flat, unordered array of `capacity` slots. Membership lookup and
 * smallest-count selection are done in a single linear scan; K is expected to
 * be small (tens), so the scan is a handful of cache lines and beats the
 * bookkeeping of a heap/linked structure at this size.
 */

#include "zmalloc.h" /* zmalloc / zcalloc / zrealloc / zfree */
#include "space_saving.h"

/* ===========================================================================
 * spaceSavingWindow — a single summary
 * ==========================================================================*/

typedef struct {
    sds key;        /* Owned copy of the key name, or NULL if the slot is free */
    int dbid;       /* Database the key was accessed in */
    uint32_t hash;  /* Cached hash of (key, dbid) for fast reject before compare */
    uint64_t count; /* Estimated count (upper bound on the true count) */
    uint64_t error; /* Maximum overestimate vs. the true count */
} spaceSavingSlot;

/* A single Space-Saving summary. Internal to this file; callers use the
 * spaceSavingManager (frozen-window) API in space_saving.h. */
typedef struct spaceSavingWindow spaceSavingWindow;

struct spaceSavingWindow {
    spaceSavingSlot *slots;  /* capacity-sized array */
    int capacity;            /* K */
    int size;                /* number of occupied slots (0..capacity) */
    uint64_t total;          /* total observations recorded in this window (N) */
    uint64_t start_us;       /* when this window started accepting observations */
    uint64_t end_us;         /* when it was frozen (0 while still live) */
    int sampling_percentage; /* Sampling % these counts were gathered under (0 if unset) */
};

/* FNV-1a over the key name, folded with the db id so identical names in
 * different databases fast-reject without a full compare. */
static uint32_t spaceSavingHashItem(const sds key, int dbid) {
    uint32_t h = 2166136261u;
    size_t klen = sdslen(key);
    for (size_t i = 0; i < klen; i++) {
        h ^= (unsigned char)key[i];
        h *= 16777619u;
    }
    return h ^ (uint32_t)dbid;
}

static spaceSavingWindow *spaceSavingWindowCreate(int k) {
    if (k <= 0) return NULL;
    spaceSavingWindow *w = zcalloc(sizeof(*w));
    w->slots = zcalloc((size_t)k * sizeof(spaceSavingSlot));
    w->capacity = k;
    w->size = 0;
    w->total = 0;
    w->start_us = 0;
    w->end_us = 0;
    w->sampling_percentage = 0;
    return w;
}

static void spaceSavingWindowReset(spaceSavingWindow *w) {
    if (!w) return;
    for (int i = 0; i < w->size; i++) {
        sdsfree(w->slots[i].key);
        w->slots[i].key = NULL;
    }
    w->size = 0;
    w->total = 0;
    w->start_us = 0;
    w->end_us = 0;
    w->sampling_percentage = 0;
}

static void spaceSavingWindowRelease(spaceSavingWindow *w) {
    if (!w) return;
    spaceSavingWindowReset(w);
    zfree(w->slots);
    zfree(w);
}

static void recordSpaceSavingWindowSample(spaceSavingWindow *w, sds key, int dbid) {
    if (!w || !key) return;
    w->total++;
    uint32_t h = spaceSavingHashItem(key, dbid);

    /* Single pass: look for an existing slot (fast-rejecting on the cached hash
     * before the full compare) while tracking the smallest-count slot for the
     * eviction path. */
    int min_idx = 0;
    uint64_t min_count = UINT64_MAX;
    for (int i = 0; i < w->size; i++) {
        spaceSavingSlot *e = &w->slots[i];
        if (e->hash == h && e->dbid == dbid && sdscmp(e->key, key) == 0) {
            e->count += 1;
            return;
        }
        if (e->count < min_count) {
            min_count = e->count;
            min_idx = i;
        }
    }

    /* Room available: insert with count = 1, error = 0. */
    if (w->size < w->capacity) {
        spaceSavingSlot *e = &w->slots[w->size++];
        e->key = sdsdup(key);
        e->dbid = dbid;
        e->hash = h;
        e->count = 1;
        e->error = 0;
        return;
    }

    /* Full: evict the smallest-count slot. The new item inherits count = the
     * evicted count + 1; error records the maximum possible overestimate. */
    spaceSavingSlot *e = &w->slots[min_idx];
    sdsfree(e->key);
    e->key = sdsdup(key);
    e->dbid = dbid;
    e->hash = h;
    e->count = min_count + 1;
    e->error = min_count;
}

static void spaceSavingWindowRemoveIf(spaceSavingWindow *w, int (*pred)(sds key, int dbid, void *arg), void *arg) {
    if (!w || !pred) return;
    int out = 0;
    for (int i = 0; i < w->size; i++) {
        if (pred(w->slots[i].key, w->slots[i].dbid, arg)) {
            sdsfree(w->slots[i].key);
            continue; /* drop: do not advance the write cursor */
        }
        if (out != i) w->slots[out] = w->slots[i];
        out++;
    }
    w->size = out;
}

/* Resize a window to `new_k` capacity, keeping the highest-count entries.
 * Grow: preserve all entries; shrink: keep the top `new_k` by count. */
static void spaceSavingWindowResize(spaceSavingWindow *w, int new_k) {
    if (!w || new_k <= 0 || new_k == w->capacity) return;
    if (new_k < w->size) {
        /* Selection of the top new_k by count (K is small, O(K^2) is fine). */
        for (int i = 0; i < new_k; i++) {
            int max_idx = i;
            for (int j = i + 1; j < w->size; j++)
                if (w->slots[j].count > w->slots[max_idx].count) max_idx = j;
            if (max_idx != i) {
                spaceSavingSlot t = w->slots[i];
                w->slots[i] = w->slots[max_idx];
                w->slots[max_idx] = t;
            }
        }
        for (int i = new_k; i < w->size; i++) sdsfree(w->slots[i].key);
        w->size = new_k;
    }
    w->slots = zrealloc(w->slots, (size_t)new_k * sizeof(spaceSavingSlot));
    for (int i = (w->capacity < new_k ? w->capacity : new_k); i < new_k; i++) {
        w->slots[i].key = NULL;
        w->slots[i].dbid = 0;
        w->slots[i].hash = 0;
        w->slots[i].count = 0;
        w->slots[i].error = 0;
    }
    w->capacity = new_k;
}

/* ===========================================================================
 * spaceSavingManager — frozen-window top-K over fixed time windows.
 * See space_saving.h for the model and usage.
 * ==========================================================================*/

struct spaceSavingManager {
    spaceSavingWindow *live;        /* Current (open) window */
    spaceSavingWindow *frozen;      /* Last completed window (read path) */
    uint64_t live_window_length_us; /* Configured length of a window, in microseconds */
};

spaceSavingManager *spaceSavingManagerCreate(int k, uint64_t window_us, uint64_t now_us) {
    spaceSavingManager *m = zcalloc(sizeof(*m));
    m->live = spaceSavingWindowCreate(k);
    m->frozen = spaceSavingWindowCreate(k);
    m->live_window_length_us = window_us;
    if (!m->live || !m->frozen) {
        spaceSavingManagerRelease(m);
        return NULL;
    }
    m->live->start_us = now_us;
    return m;
}

void spaceSavingManagerRelease(spaceSavingManager *m) {
    if (!m) return;
    spaceSavingWindowRelease(m->live);
    spaceSavingWindowRelease(m->frozen);
    zfree(m);
}

void spaceSavingManagerReset(spaceSavingManager *m, uint64_t now_us) {
    if (!m) return;
    /* The sampling percentage is configuration, not measurement: it describes
     * how the NEXT observations will be gathered, so it outlives the data being
     * dropped. Preserving it here is what makes this safe to call from the
     * rotate path — clearing it would leave the live window at 0 and every
     * subsequent estimate would come back as zero until a config change. */
    int live_pct = m->live->sampling_percentage;
    spaceSavingWindowReset(m->live);
    spaceSavingWindowReset(m->frozen);
    m->live->sampling_percentage = live_pct;
    m->live->start_us = now_us;
}

/* Freeze the live window: the previous snapshot is discarded, the live window
 * becomes the new frozen snapshot, and a fresh empty live window starts.
 * A pointer swap + reset, so it is O(K) with no reallocation and transfers key
 * ownership without copying.
 *
 * `now_us` is stamped as the outgoing window's real end and the incoming
 * window's real start. Rotation is driven by a timer, so a window is closed at
 * or after its nominal boundary, never before: recording the actual interval
 * lets the reader divide by the traffic's real duration rather than the
 * configured length, which would otherwise over-report by the rotation lag. */
static void spaceSavingManagerFreeze(spaceSavingManager *m, uint64_t now_us) {
    m->live->end_us = now_us;
    spaceSavingWindow *tmp = m->frozen;
    m->frozen = m->live;
    m->live = tmp;
    spaceSavingWindowReset(m->live);
    m->live->start_us = now_us;
    /* The frozen window (old live) carries the sampling percentage it ran under
     * — it travels with the window on the swap. Carry it forward into the new
     * live window so subsequent samples keep the current setting until the
     * caller records a new one. */
    m->live->sampling_percentage = m->frozen->sampling_percentage;
}

/* Close the live window once its configured length has fully elapsed. Rotation
 * is timer-driven, so a window is closed at or after its nominal boundary and
 * the snapshot carries the real interval it accumulated over.
 *
 * If the timer ran so late that the live window covers more than twice the
 * configured length, its counts span too coarse an interval to publish as "the
 * last window", so they are dropped rather than reported. That does discard
 * whatever traffic arrived during the stall, which is the accepted cost of
 * bounding how stale a report can be: a frozen window always spans
 * [length, 2 * length), so HOTKEYS GET can never quietly return a long-run
 * average under a one-window label.
 *
 * Boundaries are measured from when the live window really started, not from a
 * nominal grid. A window is therefore never SHORTER than the configured length
 * (it is length + however late this call ran), and a late rotation cannot
 * shorten the following window — at the cost of the boundaries drifting against
 * the wall clock, so a long run sees slightly fewer windows than
 * elapsed / length. For a sampled estimator that reports its own measured span,
 * never-shorter-than-configured is the more useful guarantee: it keeps N per
 * window from collapsing after a hiccup. */
void spaceSavingManagerRotate(spaceSavingManager *m, uint64_t now_us) {
    if (!m || m->live_window_length_us == 0) return;
    uint64_t len = m->live_window_length_us;
    uint64_t start_us = m->live->start_us;
    if (now_us < start_us + len) return; /* current window still open */
    if (now_us >= start_us + 2 * len)
        spaceSavingManagerReset(m, now_us); /* too coarse to report: drop it */
    else
        spaceSavingManagerFreeze(m, now_us);
}

void recordSpaceSavingManagerSample(spaceSavingManager *m, sds key, int dbid) {
    if (!m) return;
    recordSpaceSavingWindowSample(m->live, key, dbid);
}

int spaceSavingManagerCount(spaceSavingManager *m) {
    return m ? m->frozen->size : 0;
}

void spaceSavingManagerAt(spaceSavingManager *m, int i, sds *key, int *dbid, uint64_t *count, uint64_t *error) {
    if (!m || i < 0 || i >= m->frozen->size) return;
    spaceSavingSlot *e = &m->frozen->slots[i];
    if (key) *key = e->key;
    if (dbid) *dbid = e->dbid;
    if (count) *count = e->count;
    if (error) *error = e->error;
}

void spaceSavingManagerRemoveIf(spaceSavingManager *m, int (*pred)(sds key, int dbid, void *arg), void *arg) {
    if (!m) return;
    spaceSavingWindowRemoveIf(m->live, pred, arg);
    spaceSavingWindowRemoveIf(m->frozen, pred, arg);
}

uint64_t spaceSavingManagerFrozenTotal(spaceSavingManager *m) {
    return m ? m->frozen->total : 0;
}

void spaceSavingManagerSetLiveSamplingPercentage(spaceSavingManager *m, int sampling_percentage) {
    if (m) m->live->sampling_percentage = sampling_percentage;
}

int spaceSavingManagerFrozenSamplingPercentage(spaceSavingManager *m) {
    return m ? m->frozen->sampling_percentage : 0;
}

/* Real time the last completed window spent accumulating, in microseconds. This
 * is the correct denominator for a rate: it is the window's actual span, which
 * is its configured length plus however late the rotation ran. 0 when there is
 * no completed window yet. */
uint64_t spaceSavingManagerFrozenDurationUs(spaceSavingManager *m) {
    if (!m || m->frozen->end_us <= m->frozen->start_us) return 0;
    return m->frozen->end_us - m->frozen->start_us;
}

/* Reconfigure the manager: reset only the live window (its counts were gathered
 * under the previous config and are no longer comparable) and start a fresh
 * window at `now_us` with the new capacity and window length, while KEEPING the
 * last completed (frozen) window and its sampling config intact so an in-flight
 * query still sees it. Use this instead of releasing/recreating on a config
 * change. */
void spaceSavingManagerReconfigure(spaceSavingManager *m, int new_k, uint64_t new_window_us, uint64_t now_us) {
    if (!m) return;
    int live_pct = m->live->sampling_percentage; /* configuration outlives the data */
    spaceSavingWindowReset(m->live);
    m->live->sampling_percentage = live_pct;
    if (new_k > 0 && new_k != m->live->capacity) {
        spaceSavingWindowResize(m->live, new_k);
        spaceSavingWindowResize(m->frozen, new_k);
    }
    m->live_window_length_us = new_window_us;
    m->live->start_us = now_us;
}
