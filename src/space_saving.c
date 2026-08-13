/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Generic Space-Saving top-K. See space_saving.h for the algorithm, the
 * ownership model, and usage. Depends only on the zmalloc allocator.
 *
 * Storage is a flat, unordered array of `capacity` slots. Membership lookup and
 * smallest-count selection are done in a single linear scan; K is expected to
 * be small (tens), so the scan is a handful of cache lines and beats the
 * bookkeeping of a heap/linked structure at this size.
 */

#include "zmalloc.h" /* zmalloc / zcalloc / zfree */
#include "space_saving.h"

/* ===========================================================================
 * spaceSavingWindow — a single summary
 * ==========================================================================*/

typedef struct {
    void *item;     /* Owned identity (produced by type->dup), or NULL if free */
    uint32_t hash;  /* Cached type->hash(item) for fast reject (0 if no hash fn) */
    uint64_t count; /* Estimated count (upper bound on the true count) */
    uint64_t error; /* Maximum overestimate vs. the true count */
} spaceSavingSlot;

/* A single Space-Saving summary. Internal to this file; callers use the
 * spaceSavingManager (frozen-window) API in space_saving.h. */
typedef struct spaceSavingWindow spaceSavingWindow;

struct spaceSavingWindow {
    spaceSavingSlot *slots; /* capacity-sized array */
    int capacity;           /* K */
    int size;               /* number of occupied slots (0..capacity) */
    uint64_t total;         /* total observations recorded in this window (N) */
    void *ctx;              /* opaque caller context describing how to interpret
                               this window's counts (owned by the caller) */
    spaceSavingType *type;  /* caller-provided item vtable */
};

static spaceSavingWindow *spaceSavingWindowCreate(int k, spaceSavingType *type) {
    if (k <= 0 || !type || !type->cmp || !type->dup || !type->free) return NULL;
    spaceSavingWindow *w = zcalloc(sizeof(*w));
    w->slots = zcalloc((size_t)k * sizeof(spaceSavingSlot));
    w->capacity = k;
    w->size = 0;
    w->total = 0;
    w->ctx = NULL;
    w->type = type;
    return w;
}

static void spaceSavingWindowReset(spaceSavingWindow *w) {
    if (!w) return;
    for (int i = 0; i < w->size; i++) {
        if (w->slots[i].item) w->type->free(w->slots[i].item);
        w->slots[i].item = NULL;
    }
    w->size = 0;
    w->total = 0;
    w->ctx = NULL;
}

static void spaceSavingWindowRelease(spaceSavingWindow *w) {
    if (!w) return;
    spaceSavingWindowReset(w);
    zfree(w->slots);
    zfree(w);
}

static void recordSpaceSavingWindowSample(spaceSavingWindow *w, const void *item) {
    if (!w || !item) return;
    w->total++;
    uint32_t h = w->type->hash ? w->type->hash(item) : 0;

    /* Single pass: look for an existing slot (fast-rejecting on the cached hash
     * before the potentially expensive cmp) while tracking the smallest-count
     * slot for the eviction path. */
    int min_idx = 0;
    uint64_t min_count = UINT64_MAX;
    for (int i = 0; i < w->size; i++) {
        spaceSavingSlot *e = &w->slots[i];
        if (e->hash == h && w->type->cmp(item, e->item) == 0) {
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
        e->item = w->type->dup(item);
        e->hash = h;
        e->count = 1;
        e->error = 0;
        return;
    }

    /* Full: evict the smallest-count slot. The new item inherits count = the
     * evicted count + 1; error records the maximum possible overestimate. */
    spaceSavingSlot *e = &w->slots[min_idx];
    w->type->free(e->item);
    e->item = w->type->dup(item);
    e->hash = h;
    e->count = min_count + 1;
    e->error = min_count;
}

static int spaceSavingWindowCount(spaceSavingWindow *w) {
    return w ? w->size : 0;
}

static void spaceSavingWindowAt(spaceSavingWindow *w, int i, void **item, uint64_t *count, uint64_t *error) {
    if (!w || i < 0 || i >= w->size) return;
    spaceSavingSlot *e = &w->slots[i];
    if (item) *item = e->item;
    if (count) *count = e->count;
    if (error) *error = e->error;
}

static void spaceSavingWindowRemoveIf(spaceSavingWindow *w, int (*pred)(const void *item, void *arg), void *arg) {
    if (!w || !pred) return;
    int out = 0;
    for (int i = 0; i < w->size; i++) {
        if (pred(w->slots[i].item, arg)) {
            w->type->free(w->slots[i].item);
            continue; /* drop: do not advance the write cursor */
        }
        if (out != i) w->slots[out] = w->slots[i];
        out++;
    }
    w->size = out;
}

/* ===========================================================================
 * spaceSavingManager — frozen-window top-K over fixed time windows.
 * See space_saving.h for the model and usage.
 * ==========================================================================*/

struct spaceSavingManager {
    spaceSavingWindow *live;   /* Current (open) window */
    spaceSavingWindow *frozen; /* Last completed window (read path) */
    uint64_t live_window_length_us; /* Length of the current (live) window, in microseconds */
    uint64_t live_window_start_us;  /* Start time of the current (live) window */
};

spaceSavingManager *spaceSavingManagerCreate(int k, uint64_t window_us, uint64_t now_us, spaceSavingType *type) {
    spaceSavingManager *m = zcalloc(sizeof(*m));
    m->live = spaceSavingWindowCreate(k, type);
    m->frozen = spaceSavingWindowCreate(k, type);
    m->live_window_length_us = window_us;
    m->live_window_start_us = now_us;
    if (!m->live || !m->frozen) {
        spaceSavingManagerRelease(m);
        return NULL;
    }
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
    spaceSavingWindowReset(m->live);
    spaceSavingWindowReset(m->frozen);
    m->live_window_start_us = now_us;
}

/* Freeze the live window: the previous snapshot is discarded, the live window
 * becomes the new frozen snapshot, and a fresh empty live window starts.
 * A pointer swap + reset, so it is O(K) with no reallocation and transfers item
 * ownership without copying. */
static void spaceSavingManagerFreeze(spaceSavingManager *m) {
    spaceSavingWindow *tmp = m->frozen;
    m->frozen = m->live;
    m->live = tmp;
    spaceSavingWindowReset(m->live);
    /* The frozen window (old live) carries the context it ran under — it travels
     * with the window on the swap. Carry that same context forward into the new
     * live window so subsequent samples keep the current config until the caller
     * sets a new one. */
    m->live->ctx = m->frozen->ctx;
}

void spaceSavingManagerRotate(spaceSavingManager *m, uint64_t now_us) {
    uint64_t elapsed, windows;
    if (!m || m->live_window_length_us == 0) return;
    if (now_us <= m->live_window_start_us) return; /* monotonic guard */
    elapsed = now_us - m->live_window_start_us;
    if (elapsed < m->live_window_length_us) return; /* current window still open */
    windows = elapsed / m->live_window_length_us;   /* full windows elapsed */

    /* One freeze snapshots the just-completed window. If two or more whole
     * windows elapsed (an idle gap), a second freeze pushes that snapshot out
     * too, so the frozen window correctly ends up empty. Any freeze past the
     * second is a no-op on an already-empty summary, so cap at two. */
    int freeze_count = (windows >= 2) ? 2 : 1;
    for (int i = 0; i < freeze_count; i++) spaceSavingManagerFreeze(m);
    m->live_window_start_us += windows * m->live_window_length_us;
}

/* Record one observation into the current (live) window. Does NOT rotate: the
 * caller drives window boundaries by calling spaceSavingManagerRotate() on a
 * timer, which keeps this hot path free of any clock read. */
void recordSpaceSavingManagerSample(spaceSavingManager *m, const void *item) {
    if (!m) return;
    recordSpaceSavingWindowSample(m->live, item);
}

int spaceSavingManagerCount(spaceSavingManager *m) {
    return m ? spaceSavingWindowCount(m->frozen) : 0;
}

void spaceSavingManagerAt(spaceSavingManager *m, int i, void **item, uint64_t *count, uint64_t *error) {
    if (m) spaceSavingWindowAt(m->frozen, i, item, count, error);
}

void spaceSavingManagerRemoveIf(spaceSavingManager *m, int (*pred)(const void *item, void *arg), void *arg) {
    if (!m) return;
    spaceSavingWindowRemoveIf(m->live, pred, arg);
    spaceSavingWindowRemoveIf(m->frozen, pred, arg);
}

/* Set the opaque context for the current (live) window. Copied to the frozen
 * window each time a window is frozen. The caller owns the pointed-to data. */
void spaceSavingManagerSetLiveContext(spaceSavingManager *m, void *ctx) {
    if (m) m->live->ctx = ctx;
}

/* Return the opaque context that was active for the last completed (frozen)
 * window, or NULL if none. */
void *spaceSavingManagerFrozenContext(spaceSavingManager *m) {
    return m ? m->frozen->ctx : NULL;
}

/* Total number of observations recorded in the last completed (frozen) window. */
uint64_t spaceSavingManagerFrozenTotal(spaceSavingManager *m) {
    return m ? m->frozen->total : 0;
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
        for (int i = new_k; i < w->size; i++)
            if (w->slots[i].item) w->type->free(w->slots[i].item);
        w->size = new_k;
    }
    w->slots = zrealloc(w->slots, (size_t)new_k * sizeof(spaceSavingSlot));
    for (int i = (w->capacity < new_k ? w->capacity : new_k); i < new_k; i++) {
        w->slots[i].item = NULL;
        w->slots[i].hash = 0;
        w->slots[i].count = 0;
        w->slots[i].error = 0;
    }
    w->capacity = new_k;
}

/* Reconfigure the manager: reset only the live window (its counts were gathered
 * under the previous config and are no longer comparable) and start a fresh
 * window at `now_us` with the new capacity and window length, while KEEPING the
 * last completed (frozen) window and its context intact so an in-flight query
 * still sees it. Use this instead of releasing/recreating on a config change. */
void spaceSavingManagerReconfigure(spaceSavingManager *m, int new_k, uint64_t new_window_us, uint64_t now_us) {
    if (!m) return;
    spaceSavingWindowReset(m->live);
    if (new_k > 0 && new_k != m->live->capacity) {
        spaceSavingWindowResize(m->live, new_k);
        spaceSavingWindowResize(m->frozen, new_k);
    }
    m->live_window_length_us = new_window_us;
    m->live_window_start_us = now_us;
}
